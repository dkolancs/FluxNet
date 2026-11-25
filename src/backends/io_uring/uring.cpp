#include "core/worker.hpp"

#include "observability/logger.hpp"

#include <cerrno>
#include <cstring>
#include <liburing.h>
#include <poll.h>
#include <sys/socket.h>

namespace fluxnet::detail {
namespace {

constexpr std::uint64_t kOpShift = 56;
enum class Uop : std::uint8_t {
    Recv = 1,
    Send = 2,
    PollWake = 3,
    PollTimer = 4,
    Cancel = 5,
    PollConn = 6
};

std::uint64_t make_token(Uop op, std::uint32_t index, std::uint32_t generation) {
    return (static_cast<std::uint64_t>(op) << kOpShift) |
           ((static_cast<std::uint64_t>(index) & 0xffffffu) << 32) | generation;
}

Uop token_op(std::uint64_t token) {
    return static_cast<Uop>(static_cast<std::uint8_t>(token >> kOpShift));
}

} // namespace

void* UringDriver::next_sqe(Worker* worker) {
    auto* ring = static_cast<io_uring*>(worker->uring_ring_);
    if (ring == nullptr) {
        return nullptr;
    }
    io_uring_sqe* sqe = io_uring_get_sqe(ring);
    if (sqe == nullptr) {
        io_uring_submit(ring);
        sqe = io_uring_get_sqe(ring);
    }
    if (sqe == nullptr) {
        worker->sq_full_.add();
    }
    return sqe;
}

void UringDriver::finish_if_idle(Worker* worker, Conn* conn) {
    if (conn == nullptr || !conn->closing) {
        return;
    }
    if (conn->recv_inflight || conn->send_inflight || conn->uring_ops > 0) {
        return;
    }
    worker->pending_close_.emplace_back(conn->index, conn->generation);
}

void UringDriver::submit_poll(Worker* worker, int fd, unsigned mask, std::uint64_t token) {
    auto* sqe = static_cast<io_uring_sqe*>(next_sqe(worker));
    if (sqe == nullptr) {
        return;
    }
    io_uring_prep_poll_add(sqe, fd, mask);
    io_uring_sqe_set_data64(sqe, token);
    worker->uring_inflight_polls_++;
}

bool UringDriver::conns_busy(Worker* worker) {
    for (const auto& slot : worker->slots_) {
        if (slot && (slot->uring_ops > 0 || slot->recv_inflight || slot->send_inflight)) {
            return true;
        }
    }
    return false;
}

void UringDriver::submit_recv(Worker* worker, Conn* conn) {
    if (worker == nullptr || conn == nullptr || conn->destroyed || conn->closing ||
        conn->connecting || conn->read_paused || conn->read_closed || conn->recv_inflight ||
        conn->fd < 0) {
        return;
    }
    auto* sqe = static_cast<io_uring_sqe*>(next_sqe(worker));
    if (sqe == nullptr) {
        worker->defer_read(conn);
        return;
    }
    io_uring_prep_recv(sqe, conn->fd, conn->recv_buf.data(), conn->recv_buf.size(), 0);
    const std::uint64_t token = make_token(Uop::Recv, conn->index, conn->generation);
    io_uring_sqe_set_data64(sqe, token);
    conn->recv_token = token;
    conn->recv_inflight = true;
    conn->uring_ops++;
}

void UringDriver::submit_send(Worker* worker, Conn* conn) {
    if (worker == nullptr || conn == nullptr || conn->destroyed || conn->fd < 0) {
        return;
    }
    if (conn->connecting) {
        if (conn->send_inflight || conn->closing) {
            return;
        }
        auto* sqe = static_cast<io_uring_sqe*>(next_sqe(worker));
        if (sqe == nullptr) {
            worker->defer_write(conn);
            return;
        }
        io_uring_prep_poll_add(sqe, conn->fd, POLLOUT);
        const std::uint64_t token = make_token(Uop::PollConn, conn->index, conn->generation);
        io_uring_sqe_set_data64(sqe, token);
        conn->send_token = token;
        conn->send_inflight = true;
        conn->uring_ops++;
        return;
    }
    if (conn->closing || conn->send_inflight || !conn->has_output()) {
        return;
    }
    auto& chunk = conn->out.front();
    if (chunk.off >= chunk.buf.size()) {
        return;
    }
    const std::size_t left = chunk.buf.size() - chunk.off;
    auto* sqe = static_cast<io_uring_sqe*>(next_sqe(worker));
    if (sqe == nullptr) {
        worker->defer_write(conn);
        return;
    }
    io_uring_prep_send(sqe, conn->fd, chunk.buf.data() + chunk.off, left, MSG_NOSIGNAL);
    const std::uint64_t token = make_token(Uop::Send, conn->index, conn->generation);
    io_uring_sqe_set_data64(sqe, token);
    conn->send_token = token;
    conn->send_len = left;
    conn->send_inflight = true;
    conn->uring_ops++;
}

void UringDriver::cancel(Worker* worker, Conn* conn) {
    if (worker == nullptr || conn == nullptr || worker->uring_ring_ == nullptr) {
        return;
    }
    auto arm = [&](bool inflight, bool& sent, std::uint64_t token) {
        if (!inflight || sent) {
            return;
        }
        auto* sqe = static_cast<io_uring_sqe*>(next_sqe(worker));
        if (sqe == nullptr) {
            return;
        }
        io_uring_prep_cancel64(sqe, token, 0);
        io_uring_sqe_set_data64(sqe, make_token(Uop::Cancel, conn->index, conn->generation));
        sent = true;
        conn->uring_ops++;
    };
    arm(conn->recv_inflight, conn->recv_cancel_sent, conn->recv_token);
    arm(conn->send_inflight, conn->send_cancel_sent, conn->send_token);
}

void UringDriver::handle_cqe(Worker* worker, void* raw) {
    auto* cqe = static_cast<io_uring_cqe*>(raw);
    const std::uint64_t token = cqe->user_data;
    const int res = cqe->res;
    const Uop op = token_op(token);
    std::uint32_t index = 0;
    std::uint32_t generation = 0;
    unpack_token(token, index, generation);
    index &= 0x00ffffffu;

    if (op == Uop::PollWake) {
        worker->uring_inflight_polls_--;
        worker->wake_fd_.drain();
        if (!worker->stop_->load(std::memory_order_acquire) ||
            (worker->drain_ != nullptr && worker->drain_->load(std::memory_order_acquire))) {
            submit_poll(worker, worker->wake_fd_.fd(), POLLIN, make_token(Uop::PollWake, 0, 0));
        }
        return;
    }
    if (op == Uop::PollTimer) {
        worker->uring_inflight_polls_--;
        worker->timer_fd_.drain();
        worker->on_timer();
        if (!worker->stop_->load(std::memory_order_acquire) ||
            (worker->drain_ != nullptr && worker->drain_->load(std::memory_order_acquire))) {
            submit_poll(worker, worker->timer_fd_.fd(), POLLIN, make_token(Uop::PollTimer, 0, 0));
        }
        return;
    }

    Conn* conn = worker->get(index, generation);
    if (conn == nullptr) {
        return;
    }
    if (op == Uop::Cancel) {
        conn->uring_ops = conn->uring_ops > 0 ? conn->uring_ops - 1 : 0;
        finish_if_idle(worker, conn);
        return;
    }
    if (op == Uop::PollConn) {
        conn->send_inflight = false;
        conn->uring_ops = conn->uring_ops > 0 ? conn->uring_ops - 1 : 0;
        if (conn->closing || res < 0) {
            if (!conn->closing) {
                worker->request_close(conn, res < 0 ? -res : 0, CloseReason::ConnectFailed);
            }
            finish_if_idle(worker, conn);
            return;
        }
        int err = 0;
        socklen_t len = sizeof(err);
        if (::getsockopt(conn->fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0) {
            err = errno;
        }
        if (err != 0) {
            worker->request_close(conn, err, CloseReason::ConnectFailed);
            finish_if_idle(worker, conn);
            return;
        }
        conn->connecting = false;
        conn->connected = true;
        worker->connects_.add();
        worker->touch(conn);
        const auto id_index = conn->index;
        const auto id_gen = conn->generation;
        auto wait = conn->connect_wait;
        worker->invoke_connect(conn);
        conn = worker->get(id_index, id_gen);
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
                wait->finish(true, ConnectionId{worker->index_, conn->index, conn->generation}, 0);
            }
            conn->connect_wait.reset();
        }
        if (!conn->closing) {
            submit_recv(worker, conn);
        }
        finish_if_idle(worker, conn);
        return;
    }
    if (op == Uop::Recv) {
        conn->recv_inflight = false;
        conn->uring_ops = conn->uring_ops > 0 ? conn->uring_ops - 1 : 0;
        if (res > 0 && !conn->closing) {
            worker->bytes_in_.add(static_cast<std::uint64_t>(res));
            worker->touch(conn);
            worker->feed(conn, conn->recv_buf.data(), static_cast<std::size_t>(res));
            conn = worker->get(index, generation);
            if (conn != nullptr && !conn->closing && !conn->read_paused && !conn->read_closed) {
                submit_recv(worker, conn);
            }
        } else if (conn != nullptr && res == 0 && !conn->closing) {
            conn->read_closed = true;
            if (!conn->has_output()) {
                worker->request_close(conn, 0, CloseReason::Eof);
            }
        } else if (res == -EAGAIN || res == -EINTR || res == -ENOBUFS) {
            if (conn != nullptr && !conn->closing && !conn->read_paused && !conn->read_closed) {
                submit_recv(worker, conn);
            }
        } else if (res < 0 && res != -ECANCELED && conn != nullptr && !conn->closing) {
            const int err = -res;
            const CloseReason why =
                (err == ECONNRESET || err == EPIPE) ? CloseReason::Reset : CloseReason::Error;
            worker->request_close(conn, err, why);
        }
        if (conn != nullptr) {
            finish_if_idle(worker, conn);
        }
        return;
    }
    if (op == Uop::Send) {
        conn->send_inflight = false;
        conn->uring_ops = conn->uring_ops > 0 ? conn->uring_ops - 1 : 0;
        if (res > 0) {
            worker->bytes_out_.add(static_cast<std::uint64_t>(res));
            worker->consume_output(conn, static_cast<std::size_t>(res));
            worker->touch(conn);
            worker->note_low_watermark(conn);
            if (!conn->destroyed && !conn->closing && conn->has_output()) {
                submit_send(worker, conn);
            }
            if (!conn->destroyed && !conn->closing && conn->read_closed && !conn->has_output()) {
                worker->request_close(conn, 0, CloseReason::Eof);
            }
        } else if ((res == -EAGAIN || res == -EINTR || res == -ENOBUFS) && !conn->closing) {
            submit_send(worker, conn);
        } else if (res < 0 && res != -ECANCELED && !conn->closing) {
            const int err = -res;
            const CloseReason why =
                (err == ECONNRESET || err == EPIPE) ? CloseReason::Reset : CloseReason::Error;
            worker->request_close(conn, err, why);
        }
        finish_if_idle(worker, conn);
    }
}

void UringDriver::run(Worker* worker) {
    io_uring ring{};
    int rc = -1;
    unsigned entries = 256;
    for (int attempt = 0; attempt < 3; ++attempt) {
        rc = io_uring_queue_init(entries, &ring, 0);
        if (rc == 0) {
            break;
        }
        entries = entries > 32 ? entries / 2 : entries;
    }
    if (rc < 0) {
        Logger::log(LogLevel::Warn,
                    "worker %u io_uring_queue_init failed: %s; falling back to epoll",
                    worker->index_, std::strerror(-rc));
        worker->opt_.backend = IoBackend::Epoll;
        worker->run_epoll();
        return;
    }
    worker->uring_ring_ = &ring;
    submit_poll(worker, worker->wake_fd_.fd(), POLLIN, make_token(Uop::PollWake, 0, 0));
    submit_poll(worker, worker->timer_fd_.fd(), POLLIN, make_token(Uop::PollTimer, 0, 0));

    while (!worker->stop_->load(std::memory_order_acquire) ||
           (worker->drain_ != nullptr && worker->drain_->load(std::memory_order_acquire))) {
        worker->drain_handoff();
        worker->drain_tasks(false);
        worker->process_deferred();
        if (worker->drain_deadline_reached()) {
            break;
        }
        if (worker->drain_ != nullptr && worker->drain_->load(std::memory_order_acquire) &&
            worker->local_conns_.load() == 0 && worker->queues_empty()) {
            break;
        }
        if (worker->stop_->load(std::memory_order_acquire) &&
            (worker->drain_ == nullptr || !worker->drain_->load(std::memory_order_acquire))) {
            break;
        }
        io_uring_submit(&ring);
        io_uring_cqe* cqe = nullptr;
        const int timeout_ms = worker->wait_timeout_ms();
        if (timeout_ms < 0) {
            rc = io_uring_wait_cqe(&ring, &cqe);
        } else {
            __kernel_timespec ts{};
            ts.tv_sec = timeout_ms / 1000;
            ts.tv_nsec = static_cast<long>((timeout_ms % 1000) * 1000000L);
            rc = io_uring_wait_cqe_timeout(&ring, &cqe, &ts);
        }
        if (rc == -ETIME || rc == -EINTR) {
            continue;
        }
        if (rc < 0) {
            Logger::log(LogLevel::Error, "io_uring wait: %s", std::strerror(-rc));
            break;
        }
        worker->io_depth_++;
        handle_cqe(worker, cqe);
        io_uring_cqe_seen(&ring, cqe);
        while (io_uring_peek_cqe(&ring, &cqe) == 0) {
            handle_cqe(worker, cqe);
            io_uring_cqe_seen(&ring, cqe);
        }
        worker->io_depth_--;
        worker->drain_handoff();
        worker->drain_tasks(false);
        worker->flush_closes();
    }

    worker->drain_tasks(true);
    worker->close_all(CloseReason::Shutdown);
    io_uring_submit(&ring);
    const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (conns_busy(worker) && std::chrono::steady_clock::now() < give_up) {
        io_uring_cqe* cqe = nullptr;
        __kernel_timespec ts{};
        ts.tv_sec = 0;
        ts.tv_nsec = 50'000'000L;
        rc = io_uring_wait_cqe_timeout(&ring, &cqe, &ts);
        if (rc == -ETIME || rc == -EINTR) {
            io_uring_submit(&ring);
            continue;
        }
        if (rc < 0) {
            break;
        }
        handle_cqe(worker, cqe);
        io_uring_cqe_seen(&ring, cqe);
        while (io_uring_peek_cqe(&ring, &cqe) == 0) {
            handle_cqe(worker, cqe);
            io_uring_cqe_seen(&ring, cqe);
        }
        worker->flush_closes();
        io_uring_submit(&ring);
    }
    worker->uring_ring_ = nullptr;
    io_uring_queue_exit(&ring);
    for (auto& slot : worker->slots_) {
        if (slot && !slot->destroyed) {
            slot->recv_inflight = false;
            slot->send_inflight = false;
            slot->uring_ops = 0;
            worker->request_close(slot.get(), 0, CloseReason::Shutdown);
        }
    }
    for (int i = 0; i < 4; ++i) {
        worker->flush_closes();
    }
}

} // namespace fluxnet::detail
