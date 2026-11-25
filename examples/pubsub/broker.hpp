#pragma once

#include "fluxnet/server.hpp"

#include <algorithm>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fluxnet::examples {

// Topic map is owned here, guarded by mu_. It stores ConnectionId values, never
// Connection pointers, because a connection is private to its worker.
// Fan-out copies the subscriber list, drops the lock, then try_send's so the
// lock is not held across another worker's queue.
class Broker {
  public:
    explicit Broker(Server& server, std::size_t max_subs = 1024)
        : server_(server), max_subs_(max_subs) {}

    void on_message(Connection& connection, MessageView message) {
        const auto text = to_text(message.payload());
        if (text.rfind("SUB ", 0) == 0) {
            subscribe(connection.id(), text.substr(4));
            reply(connection, "OK");
        } else if (text.rfind("UNSUB ", 0) == 0) {
            unsubscribe(connection.id(), text.substr(6));
            reply(connection, "OK");
        } else if (text.rfind("PUB ", 0) == 0) {
            const auto nl = text.find('\n');
            if (nl == std::string::npos || nl < 5) {
                reply(connection, "ERR format");
                return;
            }
            const auto topic = text.substr(4, nl - 4);
            const auto body = text.substr(nl + 1);
            publish(connection.id(), topic, body);
        } else {
            reply(connection, "ERR command");
        }
    }

    void on_disconnect(Connection& connection) {
        std::lock_guard lock(mu_);
        auto it = by_conn_.find(connection.id());
        if (it == by_conn_.end()) {
            return;
        }
        for (const auto& topic : it->second) {
            auto topic_it = topics_.find(topic);
            if (topic_it == topics_.end()) {
                continue;
            }
            std::erase_if(topic_it->second,
                          [&](const ConnectionId& id) { return id == connection.id(); });
            if (topic_it->second.empty()) {
                topics_.erase(topic_it);
            }
        }
        by_conn_.erase(it);
    }

    std::size_t topics() const {
        std::lock_guard lock(mu_);
        return topics_.size();
    }

  private:
    static std::string to_text(std::span<const std::byte> bytes) {
        std::string text;
        text.resize(bytes.size());
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            text[i] = static_cast<char>(bytes[i]);
        }
        return text;
    }

    static std::vector<std::byte> bytes_of(std::string_view text) {
        std::vector<std::byte> out(text.size());
        for (std::size_t i = 0; i < text.size(); ++i) {
            out[i] = static_cast<std::byte>(text[i]);
        }
        return out;
    }

    void reply(Connection& connection, std::string_view text) {
        auto bytes = bytes_of(text);
        connection.send(MessageType::Data, bytes, 0);
    }

    void subscribe(ConnectionId id, std::string topic) {
        if (topic.empty() || topic.find(' ') != std::string::npos) {
            return;
        }
        std::lock_guard lock(mu_);
        auto& subs = topics_[topic];
        if (subs.size() >= max_subs_) {
            return;
        }
        if (std::find(subs.begin(), subs.end(), id) == subs.end()) {
            subs.push_back(id);
        }
        by_conn_[id].insert(topic);
    }

    void unsubscribe(ConnectionId id, std::string topic) {
        std::lock_guard lock(mu_);
        auto it = topics_.find(topic);
        if (it != topics_.end()) {
            std::erase_if(it->second, [&](const ConnectionId& item) { return item == id; });
            if (it->second.empty()) {
                topics_.erase(it);
            }
        }
        auto by = by_conn_.find(id);
        if (by != by_conn_.end()) {
            by->second.erase(topic);
        }
    }

    void publish(ConnectionId from, const std::string& topic, const std::string& body) {
        std::vector<ConnectionId> subs;
        {
            std::lock_guard lock(mu_);
            auto it = topics_.find(topic);
            if (it != topics_.end()) {
                subs = it->second;
            }
        }
        const auto payload = bytes_of("MSG " + topic + "\n" + body);
        for (const auto& id : subs) {
            if (id == from) {
                continue;
            }
            server_.try_send(id, MessageType::Data, payload, 0);
        }
    }

    Server& server_;
    std::size_t max_subs_;
    mutable std::mutex mu_;
    std::unordered_map<std::string, std::vector<ConnectionId>> topics_;
    std::unordered_map<ConnectionId, std::unordered_set<std::string>> by_conn_;
};

} // namespace fluxnet::examples
