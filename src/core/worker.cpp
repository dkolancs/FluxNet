#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "core/worker.hpp"

#include "net/socket.hpp"
#include "observability/logger.hpp"

#include <pthread.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <sched.h>
#include <stdexcept>
#include <utility>

namespace fluxnet::detail {
namespace {

const char* reason_text(CloseReason why) {
    switch (why) {
    case CloseReason::Local:
        return "local close";
    case CloseReason::Eof:
        return "eof";
    case CloseReason::Reset:
        return "reset";
    case CloseReason::Error:
        return "error";
    case CloseReason::Protocol:
        return "protocol";
    case CloseReason::Idle:
        return "idle timeout";
    case CloseReason::SlowConsumer:
        return "slow consumer";
    case CloseReason::Callback:
        return "callback exception";
    case CloseReason::Shutdown:
        return "shutdown";
    case CloseReason::ConnectFailed:
        return "connect failed";
    }
    return "close";
}

std::string peer_of(int fd) {
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    if (::getpeername(fd, reinterpret_cast<sockaddr*>(&addr), &len) < 0) {
        return {};
    }
    char ip[INET_ADDRSTRLEN] = {};
    if (::inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip)) == nullptr) {
        return {};
    }
    return std::string(ip) + ":" + std::to_string(ntohs(addr.sin_port));
}

int so_error(int fd) {
    int err = 0;
    socklen_t len = sizeof(err);
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0) {
        return errno;
    }
    return err;
}

} // namespace

std::uint64_t pack_token(std::uint32_t index, std::uint32_t generation) noexcept {
    return (static_cast<std::uint64_t>(index) << 32) | generation;
}

void unpack_token(std::uint64_t token, std::uint32_t& index, std::uint32_t& generation) noexcept {
    index = static_cast<std::uint32_t>(token >> 32);
    generation = static_cast<std::uint32_t>(token & 0xffffffffu);
}

Worker::Worker(std::uint32_t index, WorkerOptions opt, Callbacks callbacks,
               std::atomic<std::uint32_t>* active, std::atomic<bool>* stop,
               std::atomic<bool>* drain, std::atomic<std::int64_t>* drain_deadline_ns)
    : index_(index), opt_(std::move(opt)), cb_(std::move(callbacks)), active_(active), stop_(stop),
      drain_(drain), drain_deadline_ns_(drain_deadline_ns), wake_fd_(true), timer_fd_(true),
      pool_(opt_.net.pool_max_per_class, opt_.net.use_buffer_pool),
      handoff_spsc_(opt_.net.queue_capacity), handoff_mutex_(opt_.net.queue_capacity),
      tasks_(opt_.net.queue_capacity) {}

Worker::~Worker() {
    if (stop_ != nullptr) {
        stop_->store(true, std::memory_order_release);
    }
    wake_fd_.notify();
    join();
}

void Worker::start() {
    thread_ = std::thread([this] { thread_main(nullptr); });
}

void Worker::arrive_startup(std::barrier<>* barrier) {
    if (thread_.joinable()) {
        return;
    }
    thread_ = std::thread([this, barrier] { thread_main(barrier); });
}

void Worker::join() {
    if (thread_.joinable()) {
        thread_.join();
    }
}

bool Worker::try_handoff_fd(int fd) {
    const bool ok = opt_.spsc_handoff ? handoff_spsc_.try_push(fd) : handoff_mutex_.try_push(fd);
    if (ok) {
        // Publish the fd before the wake so the consumer cannot miss it.
        wake_fd_.notify();
    }
    return ok;
}

bool Worker::queues_empty() {
    if (opt_.spsc_handoff) {
        if (!handoff_spsc_.empty()) {
            return false;
        }
    } else if (!handoff_mutex_.empty()) {
        return false;
    }
    return tasks_.empty() && deferred_reads_.empty() && deferred_writes_.empty();
}

void Worker::apply_affinity() const {
    if (!opt_.net.cpu_affinity) {
        return;
    }
    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    if (::sched_getaffinity(0, sizeof(allowed), &allowed) != 0) {
        Logger::log(LogLevel::Warn, "sched_getaffinity failed: %s", std::strerror(errno));
        return;
    }
    int cpus[CPU_SETSIZE];
    int n = 0;
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (CPU_ISSET(cpu, &allowed)) {
            cpus[n++] = cpu;
        }
    }
    if (n == 0) {
        return;
    }
    const int chosen = cpus[static_cast<int>(index_) % n];
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(chosen, &set);
    if (::pthread_setaffinity_np(pthread_self(), sizeof(set), &set) != 0) {
        Logger::log(LogLevel::Warn, "pthread_setaffinity_np(cpu %d) failed: %s", chosen,
                    std::strerror(errno));
    } else {
        Logger::log(LogLevel::Info, "worker %u pinned to cpu %d", index_, chosen);
    }
}

void Worker::thread_main(std::barrier<>* barrier) {
    thread_id_ = std::this_thread::get_id();
    try {
        apply_affinity();
    } catch (const std::exception& ex) {
        Logger::log(LogLevel::Error, "worker %u affinity: %s", index_, ex.what());
        failed_.store(true, std::memory_order_relaxed);
    }
    if (barrier != nullptr) {
        barrier->arrive_and_wait();
    }
    if (failed_.load(std::memory_order_relaxed)) {
        return;
    }
    try {
        if (opt_.backend == IoBackend::IoUring) {
            UringDriver::run(this);
        } else {
            run_epoll();
        }
    } catch (const std::exception& ex) {
        Logger::log(LogLevel::Error, "worker %u loop: %s", index_, ex.what());
        failed_.store(true, std::memory_order_relaxed);
        close_all(CloseReason::Error);
        flush_closes();
    }
}

bool Worker::reserve_slot() {
    if (active_ == nullptr) {
        return true;
    }
    const auto prev = active_->fetch_add(1, std::memory_order_relaxed);
    if (prev >= opt_.net.max_connections) {
        active_->fetch_sub(1, std::memory_order_relaxed);
        rejected_.add();
        return false;
    }
    return true;
}

void Worker::release_reservation() {
    if (active_ != nullptr) {
        active_->fetch_sub(1, std::memory_order_relaxed);
    }
}

Conn* Worker::get(std::uint32_t index, std::uint32_t generation) {
    if (index >= slots_.size()) {
        return nullptr;
    }
    Conn* conn = slots_[index].get();
    if (conn == nullptr || conn->destroyed || conn->generation != generation) {
        return nullptr;
    }
    return conn;
}

Conn* Worker::install(int fd, bool already_reserved, bool connecting) {
    if (!already_reserved && !reserve_slot()) {
        ::close(fd);
        return nullptr;
    }
    if (fd < 0) {
        if (already_reserved) {
            release_reservation();
        }
        return nullptr;
    }
    std::uint32_t index = 0;
    if (!free_list_.empty()) {
        index = free_list_.back();
        free_list_.pop_back();
    } else {
        if (slots_.size() >= 0x00fffff0u) {
            ::close(fd);
            release_reservation();
            return nullptr;
        }
        index = static_cast<std::uint32_t>(slots_.size());
        slots_.push_back(nullptr);
        generations_.push_back(1);
    }
    auto owned = std::make_unique<Conn>(opt_.net.max_frame_size, opt_.net.max_input_buffer);
    Conn* conn = owned.get();
    conn->worker = this;
    conn->fd = fd;
    conn->index = index;
    conn->generation = generations_[index];
    conn->counted = true;
    conn->connecting = connecting;
    conn->connected = !connecting;
    conn->last_activity = std::chrono::steady_clock::now();
    conn->peer = peer_of(fd);
    slots_[index] = std::move(owned);
    local_conns_.fetch_add(1, std::memory_order_relaxed);
    arm_idle(conn);
    if (opt_.backend == IoBackend::IoUring && uring_ring_ != nullptr && !connecting) {
        UringDriver::submit_recv(this, conn);
    } else if (opt_.backend != IoBackend::IoUring) {
        try {
            update_epoll(conn);
        } catch (const std::exception& ex) {
            Logger::log(LogLevel::Warn, "epoll add conn: %s", ex.what());
            request_close(conn, errno, CloseReason::Error);
        }
    }
    return conn;
}

void Worker::invoke_connect(Conn* conn) {
    if (!cb_.on_connect || conn->closing) {
        return;
    }
    ++callback_depth_;
    try {
        Connection view(conn);
        cb_.on_connect(view);
    } catch (const std::exception& ex) {
        callback_exceptions_.add();
        Logger::log(LogLevel::Warn, "on_connect exception: %s", ex.what());
        request_close(conn, 0, CloseReason::Callback);
    } catch (...) {
        callback_exceptions_.add();
        request_close(conn, 0, CloseReason::Callback);
    }
    --callback_depth_;
}

void Worker::invoke_backpressure(Conn* conn, bool paused) {
    if (!cb_.on_backpressure || conn->destroyed) {
        return;
    }
    ++callback_depth_;
    try {
        Connection view(conn);
        cb_.on_backpressure(view, paused);
    } catch (const std::exception& ex) {
        callback_exceptions_.add();
        Logger::log(LogLevel::Warn, "on_backpressure exception: %s", ex.what());
        request_close(conn, 0, CloseReason::Callback);
    } catch (...) {
        callback_exceptions_.add();
        request_close(conn, 0, CloseReason::Callback);
    }
    --callback_depth_;
}

void Worker::touch(Conn* conn) {
    conn->last_activity = std::chrono::steady_clock::now();
}

void Worker::defer_read(Conn* conn) {
    if (conn->deferred_read || conn->closing || conn->destroyed) {
        return;
    }
    conn->deferred_read = true;
    deferred_reads_.push_back(conn->index);
}

void Worker::defer_write(Conn* conn) {
    if (conn->deferred_write || conn->closing || conn->destroyed) {
        return;
    }
    conn->deferred_write = true;
    deferred_writes_.push_back(conn->index);
}

void Worker::update_epoll(Conn* conn) {
    if (conn->fd < 0 || conn->destroyed) {
        return;
    }
    epoll_event event{};
    event.events = EPOLLRDHUP;
    if (opt_.net.epoll_trigger == EpollTrigger::Edge) {
        event.events |= EPOLLET;
    }
    if (!conn->closing && conn->connecting) {
        event.events |= EPOLLOUT;
    } else if (!conn->closing) {
        if (!conn->read_paused && !conn->read_closed) {
            event.events |= EPOLLIN;
        }
        if (conn->has_output()) {
            event.events |= EPOLLOUT;
        }
    }
    event.data.u64 = pack_token(conn->index, conn->generation);
    if (!conn->in_epoll) {
        epoll_.add(conn->fd, event);
        conn->in_epoll = true;
    } else if (!conn->closing) {
        epoll_.mod(conn->fd, event);
    }
}

int Worker::fill_iov(Conn* conn, iovec* iov, int max_count) {
    int count = 0;
    for (auto& chunk : conn->out) {
        if (count >= max_count) {
            break;
        }
        if (chunk.off >= chunk.buf.size()) {
            continue;
        }
        const std::size_t left = chunk.buf.size() - chunk.off;
        iov[count].iov_base = chunk.buf.data() + chunk.off;
        iov[count].iov_len = left;
        ++count;
    }
    return count;
}

void Worker::consume_output(Conn* conn, std::size_t n) {
    while (n > 0 && !conn->out.empty()) {
        auto& chunk = conn->out.front();
        const std::size_t left = chunk.buf.size() > chunk.off ? chunk.buf.size() - chunk.off : 0;
        if (n < left) {
            chunk.off += n;
            conn->out_bytes = conn->out_bytes > n ? conn->out_bytes - n : 0;
            return;
        }
        n -= left;
        conn->out_bytes = conn->out_bytes > left ? conn->out_bytes - left : 0;
        conn->out.pop_front();
    }
}

void Worker::note_high_watermark(Conn* conn) {
    if (conn->read_paused || conn->closing) {
        return;
    }
    conn->read_paused = true;
    arm_slow(conn);
    if (opt_.backend != IoBackend::IoUring) {
        try {
            update_epoll(conn);
        } catch (const std::exception& ex) {
            Logger::log(LogLevel::Warn, "epoll pause: %s", ex.what());
        }
    }
    invoke_backpressure(conn, true);
}

void Worker::note_low_watermark(Conn* conn) {
    if (!conn->read_paused || conn->out_bytes > opt_.net.output_low_watermark) {
        return;
    }
    conn->read_paused = false;
    ++conn->slow_epoch;
    invoke_backpressure(conn, false);
    if (opt_.backend == IoBackend::IoUring) {
        UringDriver::submit_recv(this, conn);
    } else {
        try {
            update_epoll(conn);
        } catch (const std::exception& ex) {
            Logger::log(LogLevel::Warn, "epoll resume: %s", ex.what());
        }
        // ET may not deliver a new edge just because interest was restored.
        defer_read(conn);
    }
}

void Worker::arm_idle(Conn* conn) {
    if (opt_.net.idle_timeout.count() <= 0 || conn->closing) {
        return;
    }
    TimerNode node;
    node.when = conn->last_activity + opt_.net.idle_timeout;
    node.index = conn->index;
    node.generation = conn->generation;
    node.kind = TimerNode::Kind::Idle;
    timers_.push(node);
    rearm_timerfd();
}

void Worker::arm_slow(Conn* conn) {
    if (opt_.net.slow_consumer_timeout.count() <= 0) {
        return;
    }
    TimerNode node;
    node.when = std::chrono::steady_clock::now() + opt_.net.slow_consumer_timeout;
    node.index = conn->index;
    node.generation = conn->generation;
    node.epoch = conn->slow_epoch;
    node.kind = TimerNode::Kind::Slow;
    timers_.push(node);
    rearm_timerfd();
}

void Worker::arm_connect(Conn* conn) {
    if (opt_.connect_timeout.count() <= 0) {
        return;
    }
    TimerNode node;
    node.when = std::chrono::steady_clock::now() + opt_.connect_timeout;
    node.index = conn->index;
    node.generation = conn->generation;
    node.kind = TimerNode::Kind::Connect;
    timers_.push(node);
    rearm_timerfd();
}

void Worker::rearm_timerfd() {
    if (timers_.empty()) {
        timer_fd_.disarm();
        return;
    }
    const auto delay = std::chrono::duration_cast<std::chrono::nanoseconds>(
        timers_.top().when - std::chrono::steady_clock::now());
    timer_fd_.arm_relative(delay);
}

void Worker::on_timer() {
    const auto now = std::chrono::steady_clock::now();
    int guard = 0;
    while (!timers_.empty() && timers_.top().when <= now && guard < 100000) {
        ++guard;
        const TimerNode node = timers_.top();
        timers_.pop();
        Conn* conn = get(node.index, node.generation);
        if (conn == nullptr || conn->closing) {
            continue;
        }
        if (node.kind == TimerNode::Kind::Idle) {
            if (opt_.net.idle_timeout.count() > 0 &&
                now - conn->last_activity >= opt_.net.idle_timeout) {
                timeouts_idle_.add();
                request_close(conn, 0, CloseReason::Idle);
            } else {
                arm_idle(conn);
            }
        } else if (node.kind == TimerNode::Kind::Slow) {
            if (node.epoch != conn->slow_epoch) {
                continue;
            }
            if (conn->read_paused || conn->out_bytes >= opt_.net.output_high_watermark) {
                timeouts_slow_.add();
                request_close(conn, 0, CloseReason::SlowConsumer);
            }
        } else if (node.kind == TimerNode::Kind::Connect) {
            if (conn->connecting) {
                request_close(conn, ETIMEDOUT, CloseReason::ConnectFailed);
            }
        }
    }
    rearm_timerfd();
}

void Worker::request_close(Conn* conn, int err, CloseReason why) {
    if (conn == nullptr || conn->destroyed || conn->closing) {
        return;
    }
    conn->closing = true;
    conn->close_errno = err;
    conn->close_reason = why;
    if (opt_.backend == IoBackend::IoUring && uring_ring_ != nullptr &&
        (conn->recv_inflight || conn->send_inflight)) {
        UringDriver::cancel(this, conn);
        if (conn->uring_ops > 0) {
            return;
        }
    }
    pending_close_.emplace_back(conn->index, conn->generation);
}

void Worker::flush_closes() {
    if (callback_depth_ > 0 || io_depth_ > 0) {
        return;
    }
    std::vector<std::pair<std::uint32_t, std::uint32_t>> local;
    local.swap(pending_close_);
    for (const auto& item : local) {
        Conn* conn = get(item.first, item.second);
        if (conn == nullptr || !conn->closing || conn->destroyed) {
            continue;
        }
        if (conn->recv_inflight || conn->send_inflight || conn->uring_ops > 0) {
            pending_close_.push_back(item);
            continue;
        }
        destroy_conn(conn);
    }
}

void Worker::destroy_conn(Conn* conn) {
    if (conn == nullptr || conn->destroyed) {
        return;
    }
    conn->destroyed = true;
    const std::uint32_t index = conn->index;
    const std::uint32_t generation = conn->generation;
    if (conn->in_epoll) {
        epoll_.del(conn->fd);
        conn->in_epoll = false;
    }
    if (conn->fd >= 0) {
        ::close(conn->fd);
        conn->fd = -1;
    }
    if (conn->connect_wait) {
        conn->connect_wait->finish(false, {}, conn->close_errno);
        conn->connect_wait.reset();
    }
    if (conn->connected && cb_.on_disconnect) {
        ++callback_depth_;
        try {
            Connection view(conn);
            cb_.on_disconnect(view);
        } catch (const std::exception& ex) {
            callback_exceptions_.add();
            Logger::log(LogLevel::Warn, "on_disconnect exception: %s", ex.what());
        } catch (...) {
            callback_exceptions_.add();
        }
        --callback_depth_;
    } else if (!conn->connected && cb_.on_error && conn->close_reason != CloseReason::Shutdown) {
        ++callback_depth_;
        try {
            Connection view(conn);
            cb_.on_error(view, conn->close_errno, reason_text(conn->close_reason));
        } catch (...) {
            callback_exceptions_.add();
        }
        --callback_depth_;
    }
    if (conn->connected) {
        disconnects_.add();
    }
    if (conn->counted) {
        release_reservation();
        conn->counted = false;
    }
    local_conns_.fetch_sub(1, std::memory_order_relaxed);
    std::uint32_t next = generation + 1;
    if (next == 0) {
        next = 1;
    }
    generations_[index] = next;
    conn->out.clear();
    slots_[index].reset();
    free_list_.push_back(index);
}

void Worker::close_all(CloseReason why) {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> ids;
    ids.reserve(slots_.size());
    for (const auto& slot : slots_) {
        if (slot && !slot->destroyed && !slot->closing) {
            ids.emplace_back(slot->index, slot->generation);
        }
    }
    for (const auto& id : ids) {
        if (Conn* conn = get(id.first, id.second)) {
            request_close(conn, 0, why);
        }
    }
}

bool Worker::feed(Conn* conn, const std::byte* data, std::size_t n) {
    const bool ok = conn->decoder.feed(data, n, [&](MessageView msg) {
        if (conn->closing) {
            return false;
        }
        messages_in_.add();
        if (cb_.on_message) {
            ++callback_depth_;
            try {
                Connection view(conn);
                cb_.on_message(view, msg);
            } catch (const std::exception& ex) {
                callback_exceptions_.add();
                Logger::log(LogLevel::Warn, "on_message exception: %s", ex.what());
                request_close(conn, 0, CloseReason::Callback);
            } catch (...) {
                callback_exceptions_.add();
                request_close(conn, 0, CloseReason::Callback);
            }
            --callback_depth_;
        }
        return !conn->closing;
    });
    if (!ok) {
        protocol_errors_.add();
        if (cb_.on_error) {
            ++callback_depth_;
            try {
                Connection view(conn);
                cb_.on_error(view, EPROTO,
                             conn->decoder.error() == DecodeError::FrameTooLarge ? "frame too large"
                             : conn->decoder.error() == DecodeError::BadType     ? "bad type"
                                                                                 : "protocol");
            } catch (...) {
                callback_exceptions_.add();
            }
            --callback_depth_;
        }
        request_close(conn, EPROTO, CloseReason::Protocol);
        return false;
    }
    return !conn->closing;
}

void Worker::on_readable(Conn* conn) {
    if (conn == nullptr) {
        return;
    }
    conn->deferred_read = false;
    if (conn->closing || conn->destroyed || conn->connecting || conn->read_closed ||
        conn->read_paused) {
        return;
    }
    int loops = 0;
    std::size_t budget = 0;
    while (!conn->closing && !conn->read_paused && loops < kIoLoops && budget < kIoBudget) {
        const ssize_t n = ::recv(conn->fd, conn->recv_buf.data(), conn->recv_buf.size(), 0);
        if (n > 0) {
            ++loops;
            budget += static_cast<std::size_t>(n);
            bytes_in_.add(static_cast<std::uint64_t>(n));
            touch(conn);
            if (!feed(conn, conn->recv_buf.data(), static_cast<std::size_t>(n))) {
                return;
            }
            if (conn->destroyed) {
                return;
            }
            continue;
        }
        if (n == 0) {
            conn->read_closed = true;
            if (!conn->has_output()) {
                request_close(conn, 0, CloseReason::Eof);
            } else if (opt_.backend != IoBackend::IoUring) {
                try {
                    update_epoll(conn);
                } catch (...) {
                }
            }
            return;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        }
        const CloseReason why = (errno == ECONNRESET) ? CloseReason::Reset : CloseReason::Error;
        request_close(conn, errno, why);
        return;
    }
    if (!conn->closing && !conn->destroyed && !conn->read_paused && !conn->read_closed) {
        defer_read(conn);
    }
}

void Worker::on_writable(Conn* conn) {
    if (conn == nullptr || conn->destroyed) {
        return;
    }
    if (conn->connecting) {
        const int err = so_error(conn->fd);
        if (err != 0) {
            request_close(conn, err, CloseReason::ConnectFailed);
            return;
        }
        conn->connecting = false;
        conn->connected = true;
        connects_.add();
        touch(conn);
        const auto index = conn->index;
        const auto generation = conn->generation;
        auto wait = conn->connect_wait;
        invoke_connect(conn);
        conn = get(index, generation);
        if (conn == nullptr) {
            if (wait) {
                wait->finish(false, {}, 0);
            }
            return;
        }
        if (wait) {
            if (conn->closing) {
                wait->finish(false, {}, conn->close_errno);
            } else {
                wait->finish(true, ConnectionId{index_, conn->index, conn->generation}, 0);
            }
            conn->connect_wait.reset();
        }
        if (conn->closing) {
            return;
        }
    }
    std::size_t budget = 0;
    while (conn->has_output() && !conn->closing && budget < kIoBudget) {
        iovec iov[64];
        const int max_iov = opt_.net.scatter_gather ? 64 : 1;
        const int count = fill_iov(conn, iov, max_iov);
        if (count <= 0) {
            break;
        }
        msghdr msg{};
        msg.msg_iov = iov;
        msg.msg_iovlen = static_cast<std::size_t>(count);
        ssize_t n = -1;
        do {
            n = ::sendmsg(conn->fd, &msg, MSG_NOSIGNAL);
        } while (n < 0 && errno == EINTR);
        if (n > 0) {
            budget += static_cast<std::size_t>(n);
            bytes_out_.add(static_cast<std::uint64_t>(n));
            consume_output(conn, static_cast<std::size_t>(n));
            touch(conn);
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        }
        request_close(conn, errno,
                      (errno == EPIPE || errno == ECONNRESET) ? CloseReason::Reset
                                                              : CloseReason::Error);
        return;
    }
    if (!conn->destroyed && conn->has_output() && budget >= kIoBudget && !conn->closing) {
        defer_write(conn);
    }
    if (!conn->destroyed && !conn->closing) {
        note_low_watermark(conn);
        if (conn->read_closed && !conn->has_output()) {
            request_close(conn, 0, CloseReason::Eof);
        }
    }
}

SendResult Worker::enqueue(Conn* conn, MessageType type, std::span<const std::byte> payload,
                           std::uint16_t flags) {
    if (!on_this_thread()) {
        send_closed_.add();
        return SendResult::Closed;
    }
    if (conn == nullptr || conn->destroyed || conn->closing || conn->fd < 0 || conn->connecting) {
        send_closed_.add();
        return SendResult::Closed;
    }
    if (payload.size() > opt_.net.max_frame_size ||
        !valid_message_type(static_cast<std::uint16_t>(type))) {
        send_invalid_.add();
        return SendResult::InvalidMessage;
    }
    const std::size_t frame = payload.size() + 8;
    if (conn->out_bytes >= opt_.net.output_high_watermark ||
        conn->out_bytes + frame > opt_.net.max_pending_output) {
        send_backpressured_.add();
        note_high_watermark(conn);
        return SendResult::Backpressured;
    }
    PooledBuffer encoded = encode_frame(pool_, type, flags, payload, opt_.net.max_frame_size);
    if (!encoded) {
        send_invalid_.add();
        return SendResult::InvalidMessage;
    }
    const std::size_t nbytes = encoded.size();
    conn->out.push_back(Conn::OutChunk{std::move(encoded), 0});
    conn->out_bytes += nbytes;
    messages_out_.add();
    if (conn->out_bytes >= opt_.net.output_high_watermark) {
        note_high_watermark(conn);
    }
    if (conn->closing || conn->destroyed) {
        return SendResult::Accepted;
    }
    if (opt_.backend == IoBackend::IoUring) {
        UringDriver::submit_send(this, conn);
    } else {
        on_writable(conn);
        if (!conn->destroyed && !conn->closing) {
            try {
                update_epoll(conn);
            } catch (const std::exception& ex) {
                Logger::log(LogLevel::Warn, "epoll write interest: %s", ex.what());
            }
        }
    }
    return SendResult::Accepted;
}

void Worker::drain_handoff() {
    int guard = 0;
    while (guard++ < 10000) {
        if (drain_deadline_reached()) {
            break;
        }
        int fd = -1;
        bool got = false;
        if (opt_.spsc_handoff) {
            got = handoff_spsc_.try_pop(fd);
        } else {
            got = handoff_mutex_.try_pop(fd);
        }
        if (!got) {
            break;
        }
        Conn* conn = install(fd, true, false);
        if (conn == nullptr) {
            continue;
        }
        const auto index = conn->index;
        const auto generation = conn->generation;
        connects_.add();
        invoke_connect(conn);
        conn = get(index, generation);
        if (conn != nullptr && !conn->closing && opt_.backend != IoBackend::IoUring) {
            try {
                update_epoll(conn);
            } catch (...) {
            }
        }
    }
}

void Worker::drain_tasks(bool all) {
    const std::size_t limit = all ? opt_.net.queue_capacity + 8 : 64;
    tasks_.drain(
        [&](Task task) {
            if (task.fn) {
                task.fn();
            }
            flush_closes();
        },
        limit);
    if (!all && !tasks_.empty()) {
        wake_fd_.notify();
    }
}

void Worker::process_deferred() {
    std::vector<std::uint32_t> reads;
    std::vector<std::uint32_t> writes;
    reads.swap(deferred_reads_);
    writes.swap(deferred_writes_);
    ++io_depth_;
    const bool uring = opt_.backend == IoBackend::IoUring && uring_ring_ != nullptr;
    for (std::uint32_t index : reads) {
        if (index >= slots_.size() || slots_[index] == nullptr) {
            continue;
        }
        Conn* conn = slots_[index].get();
        if (!conn->deferred_read || conn->destroyed) {
            continue;
        }
        if (uring) {
            conn->deferred_read = false;
            UringDriver::submit_recv(this, conn);
        } else {
            on_readable(conn);
        }
    }
    for (std::uint32_t index : writes) {
        if (index >= slots_.size() || slots_[index] == nullptr) {
            continue;
        }
        Conn* conn = slots_[index].get();
        if (!conn->deferred_write || conn->destroyed) {
            continue;
        }
        conn->deferred_write = false;
        if (uring) {
            UringDriver::submit_send(this, conn);
        } else {
            on_writable(conn);
            conn = (index < slots_.size()) ? slots_[index].get() : nullptr;
            if (conn != nullptr && !conn->destroyed && !conn->closing) {
                try {
                    update_epoll(conn);
                } catch (...) {
                }
            }
        }
    }
    --io_depth_;
    flush_closes();
}

bool Worker::drain_deadline_reached() const {
    if (drain_ == nullptr || !drain_->load(std::memory_order_acquire)) {
        return false;
    }
    if (drain_deadline_ns_ == nullptr) {
        return false;
    }
    const auto deadline = drain_deadline_ns_->load(std::memory_order_acquire);
    if (deadline < 0) {
        return false;
    }
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    return now >= deadline;
}

int Worker::wait_timeout_ms() const {
    if (!deferred_reads_.empty() || !deferred_writes_.empty()) {
        return 0;
    }
    if (drain_ != nullptr && drain_->load(std::memory_order_acquire) &&
        drain_deadline_ns_ != nullptr) {
        const auto deadline = drain_deadline_ns_->load(std::memory_order_acquire);
        if (deadline >= 0) {
            const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
            if (now >= deadline) {
                return 0;
            }
            const auto left_ns = deadline - now;
            auto ms = left_ns / 1000000;
            if (ms < 1) {
                ms = 1;
            }
            if (ms > 200) {
                ms = 200;
            }
            return static_cast<int>(ms);
        }
    }
    return -1;
}

void Worker::run_epoll() {
    epoll_event wake_event{};
    wake_event.events = EPOLLIN;
    wake_event.data.u64 = kTokenWake;
    epoll_.add(wake_fd_.fd(), wake_event);
    epoll_event timer_event{};
    timer_event.events = EPOLLIN;
    timer_event.data.u64 = kTokenTimer;
    epoll_.add(timer_fd_.fd(), timer_event);

    while (!stop_->load(std::memory_order_acquire) ||
           (drain_ != nullptr && drain_->load(std::memory_order_acquire))) {
        drain_handoff();
        drain_tasks(false);
        process_deferred();
        if (drain_deadline_reached()) {
            break;
        }
        if (drain_ != nullptr && drain_->load(std::memory_order_acquire) &&
            local_conns_.load() == 0 && queues_empty()) {
            break;
        }
        if (stop_->load(std::memory_order_acquire) &&
            (drain_ == nullptr || !drain_->load(std::memory_order_acquire))) {
            break;
        }
        epoll_event events[64];
        const int timeout = wait_timeout_ms();
        ++io_depth_;
        const int n = epoll_.wait(events, 64, timeout);
        if (n < 0) {
            --io_depth_;
            Logger::log(LogLevel::Error, "epoll_wait: %s", std::strerror(errno));
            break;
        }
        for (int i = 0; i < n; ++i) {
            const std::uint64_t token = events[i].data.u64;
            const std::uint32_t ev = events[i].events;
            if (token == kTokenWake) {
                wake_fd_.drain();
                continue;
            }
            if (token == kTokenTimer) {
                timer_fd_.drain();
                on_timer();
                continue;
            }
            std::uint32_t index = 0;
            std::uint32_t generation = 0;
            unpack_token(token, index, generation);
            Conn* conn = get(index, generation);
            if (conn == nullptr) {
                continue;
            }
            if ((ev & EPOLLIN) != 0) {
                on_readable(conn);
                conn = get(index, generation);
                if (conn == nullptr) {
                    continue;
                }
            }
            if ((ev & EPOLLOUT) != 0 && conn != nullptr && !conn->destroyed) {
                on_writable(conn);
                conn = get(index, generation);
            }
            if (conn == nullptr || conn->destroyed) {
                continue;
            }
            if ((ev & (EPOLLERR | EPOLLHUP)) != 0 && !conn->closing) {
                const int err = so_error(conn->fd);
                request_close(conn, err != 0 ? err : EIO,
                              err == ECONNRESET ? CloseReason::Reset : CloseReason::Error);
                continue;
            }
            if ((ev & EPOLLRDHUP) != 0 && conn->read_closed && !conn->has_output() &&
                !conn->closing) {
                request_close(conn, 0, CloseReason::Eof);
                continue;
            }
            if (!conn->closing && !conn->destroyed) {
                try {
                    update_epoll(conn);
                } catch (const std::exception& ex) {
                    Logger::log(LogLevel::Warn, "epoll refresh: %s", ex.what());
                    request_close(conn, errno, CloseReason::Error);
                }
            }
        }
        --io_depth_;
        // Recheck the queues after handling the eventfd so a push that landed
        // between the drain and the next wait cannot be lost.
        drain_handoff();
        drain_tasks(false);
        flush_closes();
    }
    drain_tasks(true);
    drain_handoff();
    close_all(CloseReason::Shutdown);
    for (int i = 0; i < 4; ++i) {
        flush_closes();
    }
}

SendResult Worker::send_external(ConnectionId id, MessageType type,
                                 std::span<const std::byte> payload, std::uint16_t flags,
                                 bool block, std::chrono::milliseconds wait) {
    if (id.worker != index_ || !id.valid()) {
        return SendResult::Closed;
    }
    if (on_this_thread()) {
        return enqueue(get(id.index, id.generation), type, payload, flags);
    }
    if (payload.size() > opt_.net.max_frame_size) {
        send_invalid_.add();
        return SendResult::InvalidMessage;
    }
    std::vector<std::byte> copy(payload.begin(), payload.end());
    if (!block) {
        const bool queued =
            tasks_.try_push(Task{[this, id, type, flags, buf = std::move(copy)]() mutable {
                Conn* conn = get(id.index, id.generation);
                if (conn == nullptr) {
                    send_closed_.add();
                    return;
                }
                enqueue(conn, type, buf, flags);
            }});
        if (!queued) {
            task_queue_full_.add();
            return SendResult::Backpressured;
        }
        wake_fd_.notify();
        return SendResult::Accepted;
    }
    auto waiter = std::make_shared<ResultWait>();
    const bool queued =
        tasks_.try_push(Task{[this, id, type, flags, buf = std::move(copy), waiter]() mutable {
            Conn* conn = get(id.index, id.generation);
            if (conn == nullptr) {
                waiter->set(SendResult::Closed);
                return;
            }
            waiter->set(enqueue(conn, type, buf, flags));
        }});
    if (!queued) {
        task_queue_full_.add();
        return SendResult::Backpressured;
    }
    wake_fd_.notify();
    return waiter->get(wait);
}

PostResult Worker::post(ConnectionId id, std::function<void(Connection&)> fn) {
    if (!id.valid() || id.worker != index_) {
        return PostResult::Invalid;
    }
    if (on_this_thread()) {
        Conn* conn = get(id.index, id.generation);
        if (conn == nullptr) {
            return PostResult::Closed;
        }
        ++callback_depth_;
        try {
            Connection view(conn);
            fn(view);
        } catch (const std::exception& ex) {
            callback_exceptions_.add();
            Logger::log(LogLevel::Warn, "posted task exception: %s", ex.what());
            request_close(conn, 0, CloseReason::Callback);
        } catch (...) {
            callback_exceptions_.add();
            request_close(conn, 0, CloseReason::Callback);
        }
        --callback_depth_;
        flush_closes();
        return PostResult::Queued;
    }
    const bool queued = tasks_.try_push(Task{[this, id, fn = std::move(fn)]() mutable {
        Conn* conn = get(id.index, id.generation);
        if (conn == nullptr || !fn) {
            return;
        }
        ++callback_depth_;
        try {
            Connection view(conn);
            fn(view);
        } catch (...) {
            callback_exceptions_.add();
            request_close(conn, 0, CloseReason::Callback);
        }
        --callback_depth_;
    }});
    if (!queued) {
        task_queue_full_.add();
        return PostResult::Full;
    }
    wake_fd_.notify();
    return PostResult::Queued;
}

void Worker::close_id(ConnectionId id) {
    if (!id.valid() || id.worker != index_) {
        return;
    }
    if (on_this_thread()) {
        if (Conn* conn = get(id.index, id.generation)) {
            request_close(conn, 0, CloseReason::Local);
            flush_closes();
        }
        return;
    }
    const bool queued = tasks_.try_push(Task{[this, id] {
        if (Conn* conn = get(id.index, id.generation)) {
            request_close(conn, 0, CloseReason::Local);
        }
    }});
    if (!queued) {
        task_queue_full_.add();
        return;
    }
    wake_fd_.notify();
}

void Worker::start_connect(const std::string& host, std::uint16_t port,
                           const std::shared_ptr<ConnectWait>& wait) {
    if (!reserve_slot()) {
        wait->finish(false, {}, EMFILE);
        return;
    }
    Socket socket = Socket::tcp_v4();
    socket.set_nonblocking(true);
    if (opt_.net.tcp_nodelay) {
        socket.set_nodelay(true);
    }
    const ConnectResult result = socket.connect_v4(host, port);
    const int fd = socket.release().release();
    if (result.status == ConnectStatus::Failed) {
        ::close(fd);
        release_reservation();
        wait->finish(false, {}, result.sys_errno);
        return;
    }
    const bool pending = result.status == ConnectStatus::InProgress;
    Conn* conn = install(fd, true, pending);
    if (conn == nullptr) {
        wait->finish(false, {}, EIO);
        return;
    }
    conn->connect_wait = wait;
    if (!pending) {
        conn->connecting = false;
        conn->connected = true;
        connects_.add();
        const auto index = conn->index;
        const auto generation = conn->generation;
        invoke_connect(conn);
        conn = get(index, generation);
        if (conn == nullptr || conn->closing) {
            wait->finish(false, {}, conn ? conn->close_errno : 0);
            if (conn) {
                conn->connect_wait.reset();
            }
            return;
        }
        wait->finish(true, ConnectionId{index_, conn->index, conn->generation}, 0);
        conn->connect_wait.reset();
        return;
    }
    arm_connect(conn);
    if (opt_.backend == IoBackend::IoUring && uring_ring_ != nullptr) {
        // Completion of connect is observed via a send of zero? We use poll through epoll
        // interest only on the epoll backend. io_uring connect uses EPOLLOUT equivalent:
        // submit a recv is wrong. Poll the fd by attempting submit_send of nothing and
        // checking SO_ERROR in a zero-length path handled in uring by submit of poll.
        UringDriver::submit_send(this, conn);
    }
}

ConnectionId Worker::connect_external(const std::string& host, std::uint16_t port,
                                      std::chrono::milliseconds timeout) {
    if (on_this_thread()) {
        Logger::log(LogLevel::Warn, "connect() from the owning worker would deadlock");
        return {};
    }
    auto wait = std::make_shared<ConnectWait>();
    const bool queued =
        tasks_.try_push(Task{[this, host, port, wait] { start_connect(host, port, wait); }});
    if (!queued) {
        task_queue_full_.add();
        return {};
    }
    wake_fd_.notify();
    return wait->wait_for(timeout);
}

void Worker::collect(MetricsSnapshot& into) const {
    into.connects += connects_.get();
    into.disconnects += disconnects_.get();
    into.messages_in += messages_in_.get();
    into.messages_out += messages_out_.get();
    into.bytes_in += bytes_in_.get();
    into.bytes_out += bytes_out_.get();
    into.send_backpressured += send_backpressured_.get();
    into.send_invalid += send_invalid_.get();
    into.send_closed += send_closed_.get();
    into.protocol_errors += protocol_errors_.get();
    into.callback_exceptions += callback_exceptions_.get();
    into.timeouts_idle += timeouts_idle_.get();
    into.timeouts_slow += timeouts_slow_.get();
    into.task_queue_full += task_queue_full_.get();
    into.sq_full += sq_full_.get();
    into.rejected_admission += rejected_.get();
    const PoolStats stats = pool_.stats();
    into.pool_acquires += stats.acquires;
    into.pool_reuses += stats.reuses;
    into.pool_allocs += stats.allocs;
    into.pool_bytes_in_use += stats.bytes_in_use;
    into.pool_bytes_high_watermark += stats.bytes_high_watermark;
}

} // namespace fluxnet::detail
