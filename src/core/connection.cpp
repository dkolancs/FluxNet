#include "fluxnet/connection.hpp"

#include "core/worker.hpp"

namespace fluxnet {

ConnectionId Connection::id() const noexcept {
    if (conn_ == nullptr || conn_->worker == nullptr || conn_->destroyed) {
        return {};
    }
    return ConnectionId{conn_->worker->index(), conn_->index, conn_->generation};
}

const std::string& Connection::peer() const noexcept {
    static const std::string empty;
    if (conn_ == nullptr) {
        return empty;
    }
    return conn_->peer;
}

SendResult Connection::send(MessageType type, std::span<const std::byte> payload,
                            std::uint16_t flags) {
    if (conn_ == nullptr || conn_->worker == nullptr) {
        return SendResult::Closed;
    }
    return conn_->worker->enqueue(conn_, type, payload, flags);
}

void Connection::close() {
    if (conn_ == nullptr || conn_->worker == nullptr) {
        return;
    }
    if (conn_->worker->on_this_thread()) {
        conn_->worker->request_close(conn_, 0, detail::CloseReason::Local);
        return;
    }
    conn_->worker->close_id(id());
}

bool Connection::is_open() const noexcept {
    return conn_ != nullptr && !conn_->destroyed && !conn_->closing && conn_->fd >= 0 &&
           !conn_->connecting;
}

} // namespace fluxnet
