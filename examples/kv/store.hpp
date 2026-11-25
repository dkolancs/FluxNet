#pragma once

#include "fluxnet/connection.hpp"

#include <mutex>
#include <span>
#include <string_view>
#include <vector>
#include <string>
#include <unordered_map>

namespace fluxnet::examples {

// One mutex for the whole map. This is an example, not a database.
// Stored bytes are capped. SET fails with "ERR full" instead of growing forever.
class Store {
  public:
    explicit Store(std::size_t max_bytes = 32u * 1024u * 1024u) : max_bytes_(max_bytes) {}

    std::string command(std::string_view text) {
        if (text == "PING") {
            return "PONG";
        }
        if (text.rfind("GET ", 0) == 0) {
            std::lock_guard lock(mu_);
            auto it = map_.find(std::string(text.substr(4)));
            if (it == map_.end()) {
                return "NOT_FOUND";
            }
            return "VALUE " + it->second;
        }
        if (text.rfind("DEL ", 0) == 0) {
            std::lock_guard lock(mu_);
            auto it = map_.find(std::string(text.substr(4)));
            if (it == map_.end()) {
                return "NOT_FOUND";
            }
            bytes_ -= it->second.size() + it->first.size();
            map_.erase(it);
            return "OK";
        }
        if (text.rfind("SET ", 0) == 0) {
            const auto rest = text.substr(4);
            const auto sp = rest.find(' ');
            if (sp == std::string_view::npos) {
                return "ERR format";
            }
            std::string key(rest.substr(0, sp));
            std::string value(rest.substr(sp + 1));
            if (key.size() > 256 || value.size() > 64 * 1024) {
                return "ERR size";
            }
            std::lock_guard lock(mu_);
            std::size_t old = 0;
            if (auto it = map_.find(key); it != map_.end()) {
                old = it->first.size() + it->second.size();
            }
            const std::size_t next = bytes_ - old + key.size() + value.size();
            if (next > max_bytes_) {
                return "ERR full";
            }
            map_[std::move(key)] = std::move(value);
            bytes_ = next;
            return "OK";
        }
        return "ERR command";
    }

    std::size_t bytes() const {
        std::lock_guard lock(mu_);
        return bytes_;
    }

  private:
    std::size_t max_bytes_;
    mutable std::mutex mu_;
    std::unordered_map<std::string, std::string> map_;
    std::size_t bytes_ = 0;
};

inline std::string payload_text(std::span<const std::byte> bytes) {
    std::string text(bytes.size(), '\0');
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        text[i] = static_cast<char>(bytes[i]);
    }
    return text;
}

inline std::vector<std::byte> to_bytes(std::string_view text) {
    std::vector<std::byte> out(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        out[i] = static_cast<std::byte>(text[i]);
    }
    return out;
}

} // namespace fluxnet::examples
