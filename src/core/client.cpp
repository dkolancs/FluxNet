#include "fluxnet/client.hpp"

#include "core/worker.hpp"
#include "observability/logger.hpp"

#include <barrier>
#include <cstdio>
#include <stdexcept>
#include <vector>

namespace fluxnet {
namespace {
std::int64_t now_ns() {
    return std::chrono::steady_clock::now().time_since_epoch().count();
}
} // namespace

struct Client::Impl {
    explicit Impl(ClientConfig cfg) : config(std::move(cfg)) { validate(config); }

    ~Impl() {
        stop();
        if (run_thread == std::this_thread::get_id()) {
            return;
        }
        std::unique_lock lock(mu);
        if (!cv.wait_for(lock, config.shutdown_timeout + std::chrono::seconds(3), [&] {
                return state.load(std::memory_order_acquire) == ServerState::Stopped;
            })) {
            std::fprintf(stderr, "fluxnet: Client destroyed while run() is still active\n");
            std::abort();
        }
    }

    void ensure_stopped_config() const {
        if (state.load(std::memory_order_acquire) != ServerState::Stopped) {
            throw std::logic_error("configure the client before run()");
        }
    }

    void begin_shutdown() noexcept {
        // The flag and the notify must happen while mu is held. Otherwise run()'s
        // wait can check the flag, miss the notify, and sleep forever.
        std::lock_guard lock(mu);
        const auto deadline = now_ns() + config.shutdown_timeout.count() * 1000000LL;
        drain_deadline_ns.store(deadline, std::memory_order_release);
        drain.store(true, std::memory_order_release);
        stop_flag.store(true, std::memory_order_release);
        for (auto& worker : workers) {
            if (worker) {
                worker->wake();
            }
        }
        cv.notify_all();
    }

    void stop() noexcept { begin_shutdown(); }

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

    ClientConfig config;
    ConnectCallback on_connect;
    MessageCallback on_message;
    DisconnectCallback on_disconnect;
    ErrorCallback on_error;
    BackpressureCallback on_backpressure;
    std::atomic<ServerState> state{ServerState::Stopped};
    std::atomic<bool> startup_settled{false};
    std::atomic<bool> stop_flag{false};
    std::atomic<bool> drain{false};
    std::atomic<std::int64_t> drain_deadline_ns{-1};
    std::atomic<std::uint32_t> active{0};
    std::atomic<std::uint32_t> rr{0};
    std::atomic<IoBackend> backend{IoBackend::Epoll};
    std::string note;
    mutable std::mutex mu;
    mutable std::condition_variable cv;
    std::thread::id run_thread{};
    std::vector<std::unique_ptr<detail::Worker>> workers;
};

Client::Client(ClientConfig config) : impl_(std::make_unique<Impl>(std::move(config))) {}
Client::~Client() = default;

void Client::set_on_connect(ConnectCallback cb) {
    impl_->ensure_stopped_config();
    impl_->on_connect = std::move(cb);
}
void Client::set_on_message(MessageCallback cb) {
    impl_->ensure_stopped_config();
    impl_->on_message = std::move(cb);
}
void Client::set_on_disconnect(DisconnectCallback cb) {
    impl_->ensure_stopped_config();
    impl_->on_disconnect = std::move(cb);
}
void Client::set_on_error(ErrorCallback cb) {
    impl_->ensure_stopped_config();
    impl_->on_error = std::move(cb);
}
void Client::set_on_backpressure(BackpressureCallback cb) {
    impl_->ensure_stopped_config();
    impl_->on_backpressure = std::move(cb);
}

void Client::run() {
    if (impl_->state.load(std::memory_order_acquire) != ServerState::Stopped) {
        throw std::logic_error("Client::run() is already active");
    }
    impl_->run_thread = std::this_thread::get_id();
    impl_->active.store(0, std::memory_order_relaxed);
    {
        std::lock_guard lock(impl_->mu);
        impl_->workers.clear();
        impl_->stop_flag.store(false, std::memory_order_release);
        impl_->drain.store(false, std::memory_order_release);
        impl_->drain_deadline_ns.store(-1, std::memory_order_release);
        impl_->startup_settled.store(false, std::memory_order_release);
        impl_->state.store(ServerState::Starting, std::memory_order_release);
    }
    detail::Logger::set_level(impl_->config.log_level);

    IoBackend resolved = IoBackend::Epoll;
    impl_->note.clear();
    if (impl_->config.io_backend == IoBackend::Epoll) {
        resolved = IoBackend::Epoll;
    } else {
#ifdef FLUXNET_HAS_IO_URING
        resolved = IoBackend::IoUring;
#else
        resolved = IoBackend::Epoll;
        impl_->note = "liburing was not found at build time";
#endif
    }
    impl_->backend.store(resolved, std::memory_order_release);

    detail::WorkerOptions opt;
    opt.net = impl_->config;
    opt.backend = resolved;
    opt.connect_timeout = impl_->config.connect_timeout;
    detail::Callbacks callbacks;
    callbacks.on_connect = impl_->on_connect;
    callbacks.on_message = impl_->on_message;
    callbacks.on_disconnect = impl_->on_disconnect;
    callbacks.on_error = impl_->on_error;
    callbacks.on_backpressure = impl_->on_backpressure;

    const int n = impl_->config.worker_threads;
    {
        std::lock_guard lock(impl_->mu);
        impl_->workers.reserve(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i) {
            impl_->workers.push_back(std::make_unique<detail::Worker>(
                static_cast<std::uint32_t>(i), opt, callbacks, &impl_->active, &impl_->stop_flag,
                &impl_->drain, &impl_->drain_deadline_ns));
        }
    }
    std::barrier barrier(static_cast<std::ptrdiff_t>(n + 1));
    for (auto& worker : impl_->workers) {
        worker->arrive_startup(&barrier);
    }
    barrier.arrive_and_wait();
    {
        std::lock_guard lock(impl_->mu);
        impl_->state.store(ServerState::Running, std::memory_order_release);
        impl_->startup_settled.store(true, std::memory_order_release);
        impl_->cv.notify_all();
    }

    {
        std::unique_lock lock(impl_->mu);
        impl_->cv.wait(lock, [&] { return impl_->stop_flag.load(std::memory_order_acquire); });
    }
    if (!impl_->drain.load(std::memory_order_acquire)) {
        impl_->begin_shutdown();
    }
    std::vector<detail::Worker*> to_join;
    {
        std::lock_guard lock(impl_->mu);
        impl_->state.store(ServerState::Draining, std::memory_order_release);
        for (auto& worker : impl_->workers) {
            if (worker) {
                worker->wake();
                to_join.push_back(worker.get());
            }
        }
    }
    for (auto* worker : to_join) {
        worker->join();
    }
    {
        std::lock_guard lock(impl_->mu);
        impl_->workers.clear();
        impl_->state.store(ServerState::Stopped, std::memory_order_release);
        impl_->startup_settled.store(true, std::memory_order_release);
        impl_->cv.notify_all();
    }
    impl_->run_thread = {};
}

void Client::stop() noexcept {
    impl_->stop();
}

bool Client::wait_until_running(std::chrono::milliseconds timeout) const {
    std::unique_lock lock(impl_->mu);
    impl_->cv.wait_for(lock, timeout,
                       [&] { return impl_->startup_settled.load(std::memory_order_acquire); });
    return impl_->state.load(std::memory_order_acquire) == ServerState::Running;
}

ServerState Client::state() const noexcept {
    return impl_->state.load(std::memory_order_acquire);
}
IoBackend Client::active_backend() const noexcept {
    return impl_->backend.load(std::memory_order_acquire);
}
std::string Client::backend_note() const {
    return impl_->note;
}

MetricsSnapshot Client::metrics() const {
    MetricsSnapshot snapshot;
    for (const auto& worker : impl_->workers) {
        if (worker) {
            worker->collect(snapshot);
        }
    }
    snapshot.active_connections = impl_->active.load(std::memory_order_relaxed);
    return snapshot;
}

ConnectionId Client::connect() {
    return connect_for(impl_->config.connect_timeout);
}

ConnectionId Client::connect_for(std::chrono::milliseconds timeout) {
    if (impl_->state.load(std::memory_order_acquire) != ServerState::Running ||
        impl_->workers.empty()) {
        return {};
    }
    const auto ticket = impl_->rr.fetch_add(1, std::memory_order_relaxed);
    auto* worker = impl_->workers[ticket % impl_->workers.size()].get();
    return worker->connect_external(impl_->config.host, impl_->config.port, timeout);
}

SendResult Client::send(ConnectionId id, MessageType type, std::span<const std::byte> payload,
                        std::uint16_t flags) {
    auto* worker = impl_->worker_for(id);
    if (worker == nullptr) {
        return SendResult::Closed;
    }
    if (worker->on_this_thread() || impl_->on_worker_thread()) {
        return worker->send_external(id, type, payload, flags, false,
                                     impl_->config.shutdown_timeout);
    }
    return worker->send_external(id, type, payload, flags, true, impl_->config.shutdown_timeout);
}

SendResult Client::try_send(ConnectionId id, MessageType type, std::span<const std::byte> payload,
                            std::uint16_t flags) {
    auto* worker = impl_->worker_for(id);
    if (worker == nullptr) {
        return SendResult::Closed;
    }
    return worker->send_external(id, type, payload, flags, false, impl_->config.shutdown_timeout);
}

PostResult Client::post(ConnectionId id, std::function<void(Connection&)> fn) {
    auto* worker = impl_->worker_for(id);
    if (worker == nullptr) {
        return PostResult::Invalid;
    }
    return worker->post(id, std::move(fn));
}

void Client::close(ConnectionId id) {
    if (auto* worker = impl_->worker_for(id)) {
        worker->close_id(id);
    }
}

} // namespace fluxnet
