#include "observability/log_histogram.hpp"
#include "protocol/codec.hpp"

#include <fcntl.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Options {
    std::string host = "127.0.0.1";
    std::uint16_t port = 9000;
    int connections = 10;
    int payload = 64;
    int threads = 1;
    double warmup_s = 1;
    double duration_s = 3;
    int outstanding = 1;
    int rate = 0;
    std::uint64_t seed = 1;
    std::string out = "results/raw/bench.json";
};

Options parse(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto need = [&](const char* name) {
            if (i + 1 >= argc) {
                throw std::runtime_error(std::string("missing value for ") + name);
            }
            return argv[++i];
        };
        if (arg == "--host") {
            opt.host = need("--host");
        } else if (arg == "--port") {
            opt.port = static_cast<std::uint16_t>(std::atoi(need("--port")));
        } else if (arg == "--connections") {
            opt.connections = std::atoi(need("--connections"));
        } else if (arg == "--payload") {
            opt.payload = std::atoi(need("--payload"));
        } else if (arg == "--threads") {
            opt.threads = std::atoi(need("--threads"));
        } else if (arg == "--warmup") {
            opt.warmup_s = std::atof(need("--warmup"));
        } else if (arg == "--duration") {
            opt.duration_s = std::atof(need("--duration"));
        } else if (arg == "--outstanding") {
            opt.outstanding = std::atoi(need("--outstanding"));
        } else if (arg == "--rate") {
            opt.rate = std::atoi(need("--rate"));
        } else if (arg == "--seed") {
            opt.seed = std::strtoull(need("--seed"), nullptr, 10);
        } else if (arg == "--out") {
            opt.out = need("--out");
        } else {
            throw std::runtime_error("unknown arg " + arg);
        }
    }
    if (opt.connections < 1 || opt.payload < 1 || opt.threads < 1 || opt.outstanding < 1) {
        throw std::runtime_error("connections, payload, threads, and outstanding must be >= 1");
    }
    return opt;
}

int connect_one(const std::string& host, std::uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        ::close(fd);
        return -1;
    }
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
    }
    int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    return fd;
}

struct Conn {
    int fd = -1;
    fluxnet::detail::FrameDecoder decoder{1 << 20, 2 << 20};
    std::vector<std::byte> in;
    std::vector<std::byte> out;
    std::size_t out_off = 0;
    int inflight = 0;
    std::uint64_t seq = 0;
    std::vector<std::chrono::steady_clock::time_point> sent;
};

std::vector<std::byte> make_frame(int payload, std::uint64_t seq, std::uint64_t seed,
                                  int conn_index) {
    std::vector<std::byte> body(static_cast<std::size_t>(payload));
    for (int i = 0; i < payload; ++i) {
        body[static_cast<std::size_t>(i)] = static_cast<std::byte>(
            (seed + seq + static_cast<std::uint64_t>(conn_index) + static_cast<std::uint64_t>(i)) &
            0xffu);
    }
    std::vector<std::byte> frame(8 + body.size());
    std::size_t written = 0;
    fluxnet::detail::write_frame(frame.data(), frame.size(), fluxnet::MessageType::Data, 0, body,
                                 1 << 20, written);
    frame.resize(written);
    return frame;
}

bool payload_ok(std::span<const std::byte> body, std::uint64_t seq, std::uint64_t seed,
                int conn_index) {
    for (std::size_t i = 0; i < body.size(); ++i) {
        const auto expect = static_cast<std::byte>(
            (seed + seq + static_cast<std::uint64_t>(conn_index) + i) & 0xffu);
        if (body[i] != expect) {
            return false;
        }
    }
    return true;
}

struct Totals {
    std::uint64_t completed = 0;
    std::uint64_t failures = 0;
    std::uint64_t bytes = 0;
    std::uint64_t behind = 0;
    fluxnet::detail::LogHistogram hist;
};

void run_thread(const Options& opt, int thread_index, int conns,
                std::chrono::steady_clock::time_point warmup_end,
                std::chrono::steady_clock::time_point stop, Totals& out) {
    const int ep = ::epoll_create1(EPOLL_CLOEXEC);
    std::vector<Conn> connections(static_cast<std::size_t>(conns));
    for (int i = 0; i < conns; ++i) {
        connections[static_cast<std::size_t>(i)].fd = connect_one(opt.host, opt.port);
        if (connections[static_cast<std::size_t>(i)].fd < 0) {
            out.failures++;
            continue;
        }
        epoll_event ev{};
        ev.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP;
        ev.data.u32 = static_cast<std::uint32_t>(i);
        ::epoll_ctl(ep, EPOLL_CTL_ADD, connections[static_cast<std::size_t>(i)].fd, &ev);
    }
    const double per_thread_rate = opt.rate > 0 ? static_cast<double>(opt.rate) / opt.threads : 0;
    auto next_slot = std::chrono::steady_clock::now();
    const auto interval = per_thread_rate > 0
                              ? std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                    std::chrono::duration<double>(1.0 / per_thread_rate))
                              : std::chrono::steady_clock::duration::zero();

    auto queue_send = [&](Conn& conn, int index) {
        if (conn.fd < 0 || conn.inflight >= opt.outstanding || !conn.out.empty()) {
            return false;
        }
        if (opt.rate > 0) {
            const auto now = std::chrono::steady_clock::now();
            if (now + interval * 2 < next_slot) {
                // Not time yet. Caller decides.
            }
            if (now > next_slot + interval * 2) {
                out.behind++;
            }
            if (now < next_slot) {
                return false;
            }
            next_slot += interval;
        }
        auto frame = make_frame(opt.payload, conn.seq, opt.seed, thread_index * 100000 + index);
        conn.out = std::move(frame);
        conn.out_off = 0;
        conn.sent.push_back(std::chrono::steady_clock::now());
        conn.inflight++;
        conn.seq++;
        return true;
    };

    while (std::chrono::steady_clock::now() < stop) {
        for (int i = 0; i < conns; ++i) {
            queue_send(connections[static_cast<std::size_t>(i)], i);
        }
        epoll_event events[64];
        const int n = ::epoll_wait(ep, events, 64, 10);
        for (int e = 0; e < n; ++e) {
            auto& conn = connections[events[e].data.u32];
            if ((events[e].events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) != 0 &&
                (events[e].events & EPOLLIN) == 0) {
                out.failures++;
                ::close(conn.fd);
                conn.fd = -1;
                continue;
            }
            if ((events[e].events & EPOLLOUT) != 0 && !conn.out.empty()) {
                const ssize_t w = ::send(conn.fd, conn.out.data() + conn.out_off,
                                         conn.out.size() - conn.out_off, MSG_NOSIGNAL);
                if (w > 0) {
                    conn.out_off += static_cast<std::size_t>(w);
                    if (conn.out_off >= conn.out.size()) {
                        conn.out.clear();
                        conn.out_off = 0;
                    }
                }
            }
            if ((events[e].events & EPOLLIN) != 0 && conn.fd >= 0) {
                std::byte buf[8192];
                const ssize_t r = ::recv(conn.fd, buf, sizeof(buf), 0);
                if (r > 0) {
                    conn.decoder.feed(
                        buf, static_cast<std::size_t>(r), [&](fluxnet::MessageView msg) {
                            if (conn.inflight > 0 && !conn.sent.empty()) {
                                const auto start = conn.sent.front();
                                conn.sent.erase(conn.sent.begin());
                                conn.inflight--;
                                const auto now = std::chrono::steady_clock::now();
                                if (now >= warmup_end) {
                                    if (!payload_ok(msg.payload(),
                                                    conn.seq -
                                                        static_cast<std::uint64_t>(conn.inflight) -
                                                        1,
                                                    opt.seed,
                                                    thread_index * 100000 +
                                                        static_cast<int>(events[e].data.u32))) {
                                        out.failures++;
                                    } else {
                                        out.completed++;
                                        out.bytes += msg.payload().size();
                                        const auto ns =
                                            std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                now - start)
                                                .count();
                                        out.hist.record(static_cast<std::uint64_t>(ns));
                                    }
                                } else if (!payload_ok(
                                               msg.payload(),
                                               conn.seq -
                                                   static_cast<std::uint64_t>(conn.inflight) - 1,
                                               opt.seed,
                                               thread_index * 100000 +
                                                   static_cast<int>(events[e].data.u32))) {
                                    out.failures++;
                                }
                            }
                            return true;
                        });
                } else if (r == 0 || (r < 0 && errno != EAGAIN && errno != EINTR)) {
                    out.failures++;
                    ::close(conn.fd);
                    conn.fd = -1;
                }
            }
        }
    }
    for (auto& conn : connections) {
        if (conn.fd >= 0) {
            ::close(conn.fd);
        }
    }
    ::close(ep);
}

std::string read_file(const char* path) {
    std::ifstream in(path);
    std::stringstream buffer;
    buffer << in.rdbuf();
    std::string text = buffer.str();
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) {
        text.pop_back();
    }
    for (char& ch : text) {
        if (ch == '"' || ch == '\\') {
            ch = '\'';
        }
    }
    return text;
}

std::string cpu_model() {
    std::ifstream in("/proc/cpuinfo");
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("model name", 0) == 0) {
            auto pos = line.find(':');
            if (pos != std::string::npos) {
                return line.substr(pos + 2);
            }
        }
    }
    return "unknown";
}

std::string git_hash() {
    FILE* pipe = ::popen("git rev-parse --short HEAD 2>/dev/null", "r");
    if (pipe == nullptr) {
        return "unknown";
    }
    char buf[64] = {};
    if (std::fgets(buf, sizeof(buf), pipe) == nullptr) {
        ::pclose(pipe);
        return "unknown";
    }
    ::pclose(pipe);
    std::string hash(buf);
    while (!hash.empty() && (hash.back() == '\n' || hash.back() == '\r')) {
        hash.pop_back();
    }
    return hash.empty() ? "unknown" : hash;
}

} // namespace

int main(int argc, char** argv) {
    Options opt;
    try {
        opt = parse(argc, argv);
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << "\n";
        return 2;
    }
    const auto start = std::chrono::steady_clock::now();
    const auto warmup_end = start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                        std::chrono::duration<double>(opt.warmup_s));
    const auto stop = warmup_end + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                       std::chrono::duration<double>(opt.duration_s));
    const int base = opt.connections / opt.threads;
    int extra = opt.connections % opt.threads;
    std::vector<Totals> totals(static_cast<std::size_t>(opt.threads));
    std::vector<std::thread> threads;
    for (int t = 0; t < opt.threads; ++t) {
        const int conns = base + (extra > 0 ? 1 : 0);
        if (extra > 0) {
            --extra;
        }
        threads.emplace_back(run_thread, std::cref(opt), t, conns, warmup_end, stop,
                             std::ref(totals[static_cast<std::size_t>(t)]));
    }
    for (auto& thread : threads) {
        thread.join();
    }
    Totals all;
    for (const auto& item : totals) {
        all.completed += item.completed;
        all.failures += item.failures;
        all.bytes += item.bytes;
        all.behind += item.behind;
    }
    // Merge thread-local buckets before percentiles. The max of per-thread
    // percentiles is not the percentile of the combined sample.
    fluxnet::detail::LogHistogram merged;
    for (const auto& item : totals) {
        merged.merge(item.hist);
    }
    const std::uint64_t p50 = merged.percentile(0.50);
    const std::uint64_t p95 = merged.percentile(0.95);
    const std::uint64_t p99 = merged.percentile(0.99);
    const std::uint64_t p999 = merged.percentile(0.999);
    const std::uint64_t max_v = merged.max();
    const double seconds = opt.duration_s > 0 ? opt.duration_s : 1;
    const double msgs_s = static_cast<double>(all.completed) / seconds;
    const double bytes_s = static_cast<double>(all.bytes) / seconds;
    std::error_code ec;
    // directory may not exist; the caller creates it.
    std::ofstream out(opt.out);
    if (!out) {
        std::cerr << "cannot write " << opt.out << "\n";
        return 1;
    }
    out << "{\n";
    out << "  \"git\": \"" << git_hash() << "\",\n";
    out << "  \"cpu\": \"" << cpu_model() << "\",\n";
    out << "  \"cores\": " << std::thread::hardware_concurrency() << ",\n";
    out << "  \"os\": \"" << read_file("/proc/sys/kernel/ostype") << "\",\n";
    out << "  \"compiler\": \"" << __VERSION__ << "\",\n";
    out << "  \"host\": \"" << opt.host << "\",\n";
    out << "  \"port\": " << opt.port << ",\n";
    out << "  \"connections\": " << opt.connections << ",\n";
    out << "  \"payload\": " << opt.payload << ",\n";
    out << "  \"threads\": " << opt.threads << ",\n";
    out << "  \"outstanding\": " << opt.outstanding << ",\n";
    out << "  \"rate\": " << opt.rate << ",\n";
    out << "  \"warmup_s\": " << opt.warmup_s << ",\n";
    out << "  \"duration_s\": " << opt.duration_s << ",\n";
    out << "  \"seed\": " << opt.seed << ",\n";
    out << "  \"completed\": " << all.completed << ",\n";
    out << "  \"failures\": " << all.failures << ",\n";
    out << "  \"bytes\": " << all.bytes << ",\n";
    out << "  \"behind_schedule\": " << all.behind << ",\n";
    out << "  \"msgs_per_s\": " << msgs_s << ",\n";
    out << "  \"bytes_per_s\": " << bytes_s << ",\n";
    out << "  \"p50_ns\": " << p50 << ",\n";
    out << "  \"p95_ns\": " << p95 << ",\n";
    out << "  \"p99_ns\": " << p99 << ",\n";
    out << "  \"p999_ns\": " << p999 << ",\n";
    out << "  \"max_ns\": " << max_v << ",\n";
    out << "  \"histogram\": \"log2 x 64 sub-buckets, percentile is the bucket upper bound; "
           "multi-thread percentiles are merged bucket counts\"\n";
    out << "}\n";
    std::cout << "completed=" << all.completed << " failures=" << all.failures
              << " msgs/s=" << msgs_s << " p50_ns=" << p50 << " p99_ns=" << p99 << "\n";
    return all.completed == 0 ? 1 : 0;
}
