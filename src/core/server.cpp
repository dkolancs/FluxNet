#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "fluxnet/server.hpp"

#include "core/worker.hpp"
#include "net/socket.hpp"
#include "observability/logger.hpp"

#include <pthread.h>
#include <sched.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include <barrier>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>

#ifdef FLUXNET_HAS_IO_URING
#include <liburing.h>
#endif

namespace fluxnet {
namespace {

std::int64_t now_ns() {
    return std::chrono::steady_clock::now().time_since_epoch().count();
}

IoBackend resolve_backend(IoBackend requested, std::string& note) {
    note.clear();
    if (requested == IoBackend::Epoll) {
        return IoBackend::Epoll;
    }
#ifndef FLUXNET_HAS_IO_URING
    note = "liburing was not found at build time";
    return IoBackend::Epoll;
#else
    {
        std::ifstream in("/proc/sys/kernel/io_uring_disabled");
        int disabled = 0;
        if (in && (in >> disabled) && disabled != 0) {
            note = "io_uring_disabled=" + std::to_string(disabled);
            return IoBackend::Epoll;
        }
    }
    io_uring ring{};
    const int rc = io_uring_queue_init(32, &ring, 0);
    if (rc < 0) {
        note = std::string("io_uring_queue_init: ") + std::strerror(-rc);
        return IoBackend::Epoll;
    }
    io_uring_queue_exit(&ring);
    return IoBackend::IoUring;
#endif
}

} // namespace

struct Server::Impl {
    explicit Impl(ServerConfig cfg) : config(std::move(cfg)) { validate(config); }

    ~Impl() {
        stop();
        if (run_thread == std::this_thread::get_id()) {
            return;
        }
        std::unique_lock lock(mu);
        if (!cv.wait_for(lock, config.shutdown_timeout + std::chrono::seconds(3), [&] {
                return state.load(std::memory_order_acquire) == ServerState::Stopped;
            })) {
            std::fprintf(stderr, "fluxnet: Server destroyed while run() is still active\n");
            std::abort();
        }
    }

    void ensure_stopped_config() const {
        if (state.load(std::memory_order_acquire) != ServerState::Stopped) {
            throw std::logic_error("configure the server before run()");
        }
    }

    void begin_shutdown() noexcept {
        // Hold mu across the flag store and the notify so wait_until_running
        // cannot miss the wake.
        std::lock_guard lock(mu);
        const auto deadline = now_ns() + config.shutdown_timeout.count() * 1000000LL;
        drain_deadline_ns.store(deadline, std::memory_order_release);
        drain.store(true, std::memory_order_release);
        stop_flag.store(true, std::memory_order_release);
        wake.notify();
        for (auto& worker : workers) {
            if (worker) {
                worker->wake();
            }
        }
        cv.notify_all();
    }

    void stop() noexcept {
        if (state.load(std::memory_order_acquire) == ServerState::Stopped && workers.empty()) {
            stop_flag.store(true, std::memory_order_release);
            return;
        }
        begin_shutdown();
    }

    void finish_run(sigset_t old_mask, bool restore_mask, void (*old_pipe)(int)) {
        std::vector<detail::Worker*> to_join;
        {
            std::lock_guard lock(mu);
            to_join.reserve(workers.size());
            for (auto& worker : workers) {
                if (worker) {
                    worker->wake();
                    to_join.push_back(worker.get());
                }
            }
        }
        for (auto* worker : to_join) {
            worker->join();
        }
        if (restore_mask) {
            pthread_sigmask(SIG_SETMASK, &old_mask, nullptr);
        }
        if (old_pipe != nullptr) {
            std::signal(SIGPIPE, old_pipe);
        }
        {
            std::lock_guard lock(mu);
            workers.clear();
            listen = detail::Socket();
            signal_fd.reset();
            state.store(ServerState::Stopped, std::memory_order_release);
            startup_settled.store(true, std::memory_order_release);
            cv.notify_all();
        }
        run_thread = {};
    }

    void acceptor_loop() {
        detail::EpollReactor epoll;
        epoll_event wake_event{};
        wake_event.events = EPOLLIN;
        wake_event.data.u64 = 1;
        epoll.add(wake.fd(), wake_event);
        epoll_event listen_event{};
        listen_event.events = EPOLLIN;
        listen_event.data.u64 = 2;
        epoll.add(listen.fd(), listen_event);
        bool listen_armed = true;
        epoll_event signal_event{};
        if (signal_fd) {
            signal_event.events = EPOLLIN;
            signal_event.data.u64 = 3;
            epoll.add(signal_fd.get(), signal_event);
        }
        auto pause_until = std::chrono::steady_clock::time_point{};
        bool paused = false;

        while (!stop_flag.load(std::memory_order_acquire)) {
            if (paused && std::chrono::steady_clock::now() >= pause_until) {
                if (!listen_armed && listen) {
                    epoll.add(listen.fd(), listen_event);
                    listen_armed = true;
                }
                paused = false;
            }
            epoll_event events[32];
            const int timeout = paused ? 20 : 200;
            const int n = epoll.wait(events, 32, timeout);
            if (n < 0) {
                detail::Logger::log(LogLevel::Error, "acceptor epoll_wait: %s",
                                    std::strerror(errno));
                break;
            }
            for (int i = 0; i < n; ++i) {
                const auto token = events[i].data.u64;
                if (token == 1) {
                    wake.drain();
                } else if (token == 3 && signal_fd) {
                    signalfd_siginfo info{};
                    const ssize_t got = ::read(signal_fd.get(), &info, sizeof(info));
                    if (got == static_cast<ssize_t>(sizeof(info))) {
                        detail::Logger::log(LogLevel::Info, "signal %u, shutting down",
                                            info.ssi_signo);
                    }
                    begin_shutdown();
                } else if (token == 2) {
                    int taken = 0;
                    while (taken < 64 && !stop_flag.load(std::memory_order_acquire)) {
                        auto result = listen.accept_nb();
                        if (!result.fd) {
                            if (result.sys_errno == EAGAIN || result.sys_errno == EWOULDBLOCK ||
                                result.sys_errno == ECONNABORTED) {
                                break;
                            }
                            if (result.sys_errno == EINTR) {
                                continue;
                            }
                            if (result.sys_errno == EMFILE || result.sys_errno == ENFILE) {
                                detail::Logger::log(LogLevel::Warn, "accept: %s",
                                                    std::strerror(result.sys_errno));
                                if (listen_armed) {
                                    epoll.del(listen.fd());
                                    listen_armed = false;
                                }
                                paused = true;
                                pause_until = std::chrono::steady_clock::now() +
                                              std::chrono::milliseconds(50);
                                break;
                            }
                            detail::Logger::log(LogLevel::Error, "accept: %s",
                                                std::strerror(result.sys_errno));
                            break;
                        }
                        ++taken;
                        detail::Socket accepted(std::move(result.fd));
                        if (config.tcp_nodelay) {
                            accepted.set_nodelay(true);
                        }
                        const int fd = accepted.release().release();
                        const auto prev = active.fetch_add(1, std::memory_order_relaxed);
                        if (prev >= config.max_connections || workers.empty()) {
                            active.fetch_sub(1, std::memory_order_relaxed);
                            ::close(fd);
                            rejected.add();
                            continue;
                        }
                        detail::Worker* worker = pick_worker();
                        if (worker == nullptr || !worker->try_handoff_fd(fd)) {
                            ::close(fd);
                            active.fetch_sub(1, std::memory_order_relaxed);
                            dropped.add();
                            continue;
                        }
                        accepts.add();
                    }
                }
            }
        }
    }

    detail::Worker* pick_worker() {
        if (workers.empty()) {
            return nullptr;
        }
        if (config.load_balance == LoadBalance::LeastLoaded) {
            std::size_t best = 0;
            std::uint32_t best_load = workers[0]->load();
            for (std::size_t i = 1; i < workers.size(); ++i) {
                const auto load = workers[i]->load();
                if (load < best_load) {
                    best = i;
                    best_load = load;
                }
            }
            return workers[best].get();
        }
        const auto ticket = rr.fetch_add(1, std::memory_order_relaxed);
        return workers[ticket % workers.size()].get();
    }

    detail::Worker* worker_for(ConnectionId id) {
        if (!id.valid() || id.worker >= workers.size()) {
            return nullptr;
        }
        return workers[id.worker].get();
    }

    bool on_worker_thread() const {
        for (const auto& worker : workers) {
            if (worker && worker->on_this_thread()) {
                return true;
            }
        }
        return false;
    }

    ServerConfig config;
    ConnectCallback on_connect;
    MessageCallback on_message;
    DisconnectCallback on_disconnect;
    ErrorCallback on_error;
    BackpressureCallback on_backpressure;
    std::atomic<ServerState> state{ServerState::Stopped};
    // False until this run() has reached Running or failed back to Stopped.
    // wait_until_running must not treat the initial Stopped as "already finished".
    std::atomic<bool> startup_settled{false};
    std::atomic<bool> stop_flag{false};
    std::atomic<bool> drain{false};
    std::atomic<std::int64_t> drain_deadline_ns{-1};
    std::atomic<std::uint32_t> active{0};
    std::atomic<std::uint16_t> bound{0};
    std::atomic<IoBackend> backend{IoBackend::Epoll};
    std::string note;
    mutable std::mutex mu;
    mutable std::condition_variable cv;
    std::thread::id run_thread{};
    std::vector<std::unique_ptr<detail::Worker>> workers;
    detail::Counter accepts;
    detail::Counter dropped;
    detail::Counter rejected;
    std::atomic<std::uint32_t> rr{0};
    detail::Socket listen;
    detail::EventFd wake{true};
    detail::FileDescriptor signal_fd;
};

Server::Server(ServerConfig config) : impl_(std::make_unique<Impl>(std::move(config))) {}
Server::~Server() = default;

void Server::set_on_connect(ConnectCallback cb) {
    impl_->ensure_stopped_config();
    impl_->on_connect = std::move(cb);
}
void Server::set_on_message(MessageCallback cb) {
    impl_->ensure_stopped_config();
    impl_->on_message = std::move(cb);
}
void Server::set_on_disconnect(DisconnectCallback cb) {
    impl_->ensure_stopped_config();
    impl_->on_disconnect = std::move(cb);
}
void Server::set_on_error(ErrorCallback cb) {
    impl_->ensure_stopped_config();
    impl_->on_error = std::move(cb);
}
void Server::set_on_backpressure(BackpressureCallback cb) {
    impl_->ensure_stopped_config();
    impl_->on_backpressure = std::move(cb);
}

void Server::run() {
    if (impl_->state.load(std::memory_order_acquire) != ServerState::Stopped) {
        throw std::logic_error("Server::run() is already active");
    }
    impl_->run_thread = std::this_thread::get_id();
    impl_->active.store(0, std::memory_order_relaxed);
    impl_->workers.clear();
    {
        std::lock_guard lock(impl_->mu);
        impl_->stop_flag.store(false, std::memory_order_release);
        impl_->drain.store(false, std::memory_order_release);
        impl_->drain_deadline_ns.store(-1, std::memory_order_release);
        impl_->startup_settled.store(false, std::memory_order_release);
        impl_->state.store(ServerState::Starting, std::memory_order_release);
    }

    detail::Logger::set_level(impl_->config.log_level);
    sigset_t old_mask;
    sigemptyset(&old_mask);
    sigset_t block;
    sigemptyset(&block);
    sigaddset(&block, SIGINT);
    sigaddset(&block, SIGTERM);
    const bool masked = pthread_sigmask(SIG_BLOCK, &block, &old_mask) == 0;
    auto* old_pipe = std::signal(SIGPIPE, SIG_IGN);

    bool armed = false;
    try {
        const IoBackend resolved = resolve_backend(impl_->config.io_backend, impl_->note);
        impl_->backend.store(resolved, std::memory_order_release);
        if (!impl_->note.empty()) {
            detail::Logger::log(LogLevel::Warn, "backend fallback: %s", impl_->note.c_str());
        }
        impl_->listen = detail::Socket::tcp_v4();
        impl_->listen.set_reuseaddr(impl_->config.reuse_addr);
        if (impl_->config.reuse_port) {
            impl_->listen.set_reuseport(true);
        }
        impl_->listen.bind_v4(impl_->config.host, impl_->config.port);
        impl_->listen.listen(impl_->config.listen_backlog);
        impl_->listen.set_nonblocking(true);
        impl_->bound.store(impl_->listen.local_port(), std::memory_order_release);

        int sfd = ::signalfd(-1, &block, SFD_NONBLOCK | SFD_CLOEXEC);
        if (sfd >= 0) {
            impl_->signal_fd = detail::FileDescriptor(sfd);
        } else {
            detail::Logger::log(LogLevel::Warn, "signalfd: %s", std::strerror(errno));
        }

        detail::WorkerOptions opt;
        opt.net = impl_->config;
        opt.spsc_handoff = impl_->config.spsc_handoff;
        opt.backend = resolved;
        detail::Callbacks callbacks;
        callbacks.on_connect = impl_->on_connect;
        callbacks.on_message = impl_->on_message;
        callbacks.on_disconnect = impl_->on_disconnect;
        callbacks.on_error = impl_->on_error;
        callbacks.on_backpressure = impl_->on_backpressure;

        const int n = impl_->config.worker_threads;
        impl_->workers.reserve(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i) {
            impl_->workers.push_back(std::make_unique<detail::Worker>(
                static_cast<std::uint32_t>(i), opt, callbacks, &impl_->active, &impl_->stop_flag,
                &impl_->drain, &impl_->drain_deadline_ns));
        }
        std::barrier barrier(static_cast<std::ptrdiff_t>(n + 1));
        for (auto& worker : impl_->workers) {
            worker->arrive_startup(&barrier);
        }
        armed = true;
        barrier.arrive_and_wait();
        for (const auto& worker : impl_->workers) {
            if (worker->failed()) {
                throw std::runtime_error("worker failed during startup");
            }
        }
        if (impl_->config.cpu_affinity) {
            // Acceptor uses the last CPU in the cpuset when affinity is enabled.
            cpu_set_t allowed;
            CPU_ZERO(&allowed);
            if (::sched_getaffinity(0, sizeof(allowed), &allowed) == 0) {
                int last = -1;
                for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
                    if (CPU_ISSET(cpu, &allowed)) {
                        last = cpu;
                    }
                }
                if (last >= 0) {
                    cpu_set_t set;
                    CPU_ZERO(&set);
                    CPU_SET(last, &set);
                    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
                }
            }
        }
        {
            std::lock_guard lock(impl_->mu);
            impl_->state.store(ServerState::Running, std::memory_order_release);
            impl_->startup_settled.store(true, std::memory_order_release);
            impl_->cv.notify_all();
        }
        if (!impl_->stop_flag.load(std::memory_order_acquire)) {
            impl_->acceptor_loop();
        }
    } catch (...) {
        impl_->begin_shutdown();
        if (armed) {
            impl_->finish_run(old_mask, masked, old_pipe);
        } else {
            {
                std::lock_guard lock(impl_->mu);
                impl_->state.store(ServerState::Stopped, std::memory_order_release);
                impl_->startup_settled.store(true, std::memory_order_release);
                impl_->cv.notify_all();
            }
            if (masked) {
                pthread_sigmask(SIG_SETMASK, &old_mask, nullptr);
            }
            if (old_pipe != nullptr) {
                std::signal(SIGPIPE, old_pipe);
            }
        }
        throw;
    }

    {
        std::lock_guard lock(impl_->mu);
        impl_->state.store(ServerState::Draining, std::memory_order_release);
    }
    if (!impl_->drain.load(std::memory_order_acquire)) {
        impl_->begin_shutdown();
    }
    impl_->finish_run(old_mask, masked, old_pipe);
}

void Server::stop() noexcept {
    impl_->stop();
}

bool Server::wait_until_running(std::chrono::milliseconds timeout) const {
    std::unique_lock lock(impl_->mu);
    impl_->cv.wait_for(lock, timeout,
                       [&] { return impl_->startup_settled.load(std::memory_order_acquire); });
    return impl_->state.load(std::memory_order_acquire) == ServerState::Running;
}

ServerState Server::state() const noexcept {
    return impl_->state.load(std::memory_order_acquire);
}
std::uint16_t Server::bound_port() const noexcept {
    return impl_->bound.load(std::memory_order_acquire);
}
IoBackend Server::active_backend() const noexcept {
    return impl_->backend.load(std::memory_order_acquire);
}
std::string Server::backend_note() const {
    return impl_->note;
}

MetricsSnapshot Server::metrics() const {
    MetricsSnapshot snapshot;
    for (const auto& worker : impl_->workers) {
        if (worker) {
            worker->collect(snapshot);
        }
    }
    snapshot.accepts += impl_->accepts.get();
    snapshot.dropped_handoff += impl_->dropped.get();
    snapshot.rejected_admission += impl_->rejected.get();
    snapshot.active_connections = impl_->active.load(std::memory_order_relaxed);
    return snapshot;
}

SendResult Server::send(ConnectionId id, MessageType type, std::span<const std::byte> payload,
                        std::uint16_t flags) {
    auto* worker = impl_->worker_for(id);
    if (worker == nullptr) {
        return SendResult::Closed;
    }
    if (worker->on_this_thread()) {
        return worker->send_external(id, type, payload, flags, false,
                                     impl_->config.shutdown_timeout);
    }
    // A different worker must not block: the two loops could wait on each other.
    if (impl_->on_worker_thread()) {
        return worker->send_external(id, type, payload, flags, false,
                                     impl_->config.shutdown_timeout);
    }
    return worker->send_external(id, type, payload, flags, true, impl_->config.shutdown_timeout);
}

SendResult Server::try_send(ConnectionId id, MessageType type, std::span<const std::byte> payload,
                            std::uint16_t flags) {
    auto* worker = impl_->worker_for(id);
    if (worker == nullptr) {
        return SendResult::Closed;
    }
    return worker->send_external(id, type, payload, flags, false, impl_->config.shutdown_timeout);
}

PostResult Server::post(ConnectionId id, std::function<void(Connection&)> fn) {
    auto* worker = impl_->worker_for(id);
    if (worker == nullptr) {
        return PostResult::Invalid;
    }
    return worker->post(id, std::move(fn));
}

void Server::close(ConnectionId id) {
    if (auto* worker = impl_->worker_for(id)) {
        worker->close_id(id);
    }
}

} // namespace fluxnet
