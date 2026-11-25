#pragma once

#include "fluxnet/config.hpp"
#include "fluxnet/connection.hpp"
#include "fluxnet/metrics.hpp"

#include "backends/epoll/epoll_reactor.hpp"
#include "concurrency/mutex_queue.hpp"
#include "concurrency/spsc_ring.hpp"
#include "concurrency/wake.hpp"
#include "memory/buffer_pool.hpp"
#include "protocol/codec.hpp"

#include <atomic>
#include <barrier>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <shared_mutex>
#include <string>
#include <sys/uio.h>
#include <thread>
#include <utility>
#include <vector>

namespace fluxnet::detail {

struct WorkerOptions {
    NetConfig net;
    bool spsc_handoff = true;
    IoBackend backend = IoBackend::Epoll;
    std::chrono::milliseconds connect_timeout{2000};
};

struct Callbacks {
    ConnectCallback on_connect;
    MessageCallback on_message;
    DisconnectCallback on_disconnect;
    ErrorCallback on_error;
    BackpressureCallback on_backpressure;
};

enum class CloseReason : std::uint8_t {
    Local,
    Eof,
    Reset,
    Error,
    Protocol,
    Idle,
    SlowConsumer,
    Callback,
    Shutdown,
    ConnectFailed,
};

struct ConnectWait {
    std::mutex mu;
    std::condition_variable cv;
    bool done = false;
    bool ok = false;
    ConnectionId id{};
    int err = 0;

    void finish(bool success, ConnectionId ident, int error) {
        std::lock_guard lock(mu);
        if (done) {
            return;
        }
        ok = success;
        id = ident;
        err = error;
        done = true;
        cv.notify_all();
    }

    ConnectionId wait_for(std::chrono::milliseconds timeout) {
        std::unique_lock lock(mu);
        cv.wait_for(lock, timeout, [&] { return done; });
        return ok ? id : ConnectionId{};
    }
};

struct ResultWait {
    std::mutex mu;
    std::condition_variable cv;
    bool done = false;
    SendResult result = SendResult::Closed;

    void set(SendResult value) {
        std::lock_guard lock(mu);
        if (done) {
            return;
        }
        result = value;
        done = true;
        cv.notify_all();
    }

    SendResult get(std::chrono::milliseconds timeout) {
        std::unique_lock lock(mu);
        if (!cv.wait_for(lock, timeout, [&] { return done; })) {
            return SendResult::Backpressured;
        }
        return result;
    }
};

struct Task {
    std::function<void()> fn;
};

struct Counter {
    std::atomic<std::uint64_t> value{0};
    void add(std::uint64_t n = 1) noexcept { value.fetch_add(n, std::memory_order_relaxed); }
    std::uint64_t get() const noexcept { return value.load(std::memory_order_relaxed); }
};

class Worker;

struct Conn {
    Worker* worker = nullptr;
    int fd = -1;
    std::uint32_t index = 0;
    std::uint32_t generation = 0;
    bool counted = false;
    bool connecting = false;
    bool connected = false;
    bool closing = false;
    bool destroyed = false;
    bool read_paused = false;
    bool read_closed = false;
    bool in_epoll = false;
    bool deferred_read = false;
    bool deferred_write = false;
    bool recv_inflight = false;
    bool send_inflight = false;
    bool recv_cancel_sent = false;
    bool send_cancel_sent = false;
    int uring_ops = 0;
    int close_errno = 0;
    CloseReason close_reason = CloseReason::Local;
    std::uint64_t recv_token = 0;
    std::uint64_t send_token = 0;
    std::size_t send_len = 0;
    std::string peer;
    std::vector<std::byte> recv_buf;
    FrameDecoder decoder;
    struct OutChunk {
        PooledBuffer buf;
        std::size_t off = 0;
    };
    std::deque<OutChunk> out;
    std::size_t out_bytes = 0;
    std::chrono::steady_clock::time_point last_activity{};
    std::uint32_t slow_epoch = 0;
    std::shared_ptr<ConnectWait> connect_wait;

    Conn(std::size_t max_frame, std::size_t max_input)
        : recv_buf(16 * 1024), decoder(max_frame, max_input) {}

    bool has_output() const noexcept { return !out.empty(); }
};

struct TimerNode {
    std::chrono::steady_clock::time_point when{};
    std::uint32_t index = 0;
    std::uint32_t generation = 0;
    std::uint32_t epoch = 0;
    enum class Kind : std::uint8_t { Idle, Slow, Connect } kind = Kind::Idle;
};

struct EarlierTimer {
    bool operator()(const TimerNode& a, const TimerNode& b) const { return a.when > b.when; }
};

// Per-slot outbound admission. The slot object outlives Conn so a non-owner
// thread can admit a frame without touching a connection the owner might free.
// Accepted means the frame is in `frames` (or already spliced) and `admitted`
// counts it. Backpressured means it was not pushed.
struct OutboundSlot {
    std::mutex mu;
    std::uint32_t generation = 0;
    std::size_t admitted = 0;
    bool open = false;
    bool scheduled = false;
    bool flushing = false; // owning worker only
    std::deque<std::vector<std::byte>> frames;
};

// Defined in uring.cpp. Header stays free of liburing.
class UringDriver {
  public:
    static void run(Worker* worker);
    static void submit_recv(Worker* worker, Conn* conn);
    static void submit_send(Worker* worker, Conn* conn);
    static void cancel(Worker* worker, Conn* conn);

  private:
    static void handle_cqe(Worker* worker, void* cqe);
    static void submit_poll(Worker* worker, int fd, unsigned mask, std::uint64_t token);
    static void finish_if_idle(Worker* worker, Conn* conn);
    static bool conns_busy(Worker* worker);
    static void* next_sqe(Worker* worker);
};

class Worker {
  public:
    Worker(std::uint32_t index, WorkerOptions opt, Callbacks callbacks,
           std::atomic<std::uint32_t>* active, std::atomic<bool>* stop, std::atomic<bool>* drain,
           std::atomic<std::int64_t>* drain_deadline_ns);
    ~Worker();

    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    void start();
    void wake() noexcept { wake_fd_.notify(); }
    void join();

    std::uint32_t index() const noexcept { return index_; }
    std::uint32_t load() const noexcept { return local_conns_.load(std::memory_order_relaxed); }
    bool on_this_thread() const noexcept { return std::this_thread::get_id() == thread_id_; }
    std::thread::id thread_id() const noexcept { return thread_id_; }
    bool failed() const noexcept { return failed_.load(std::memory_order_relaxed); }

    // Called by the acceptor only. fd is already counted in *active.
    bool try_handoff_fd(int fd);

    SendResult send_external(ConnectionId id, MessageType type, std::span<const std::byte> payload,
                             std::uint16_t flags, bool block, std::chrono::milliseconds wait);
    PostResult post(ConnectionId id, std::function<void(Connection&)> fn);
    void close_id(ConnectionId id);
    ConnectionId connect_external(const std::string& host, std::uint16_t port,
                                  std::chrono::milliseconds timeout);

    SendResult enqueue(Conn* conn, MessageType type, std::span<const std::byte> payload,
                       std::uint16_t flags);
    // Any thread. Admission completes before this returns. See DESIGN.md.
    SendResult admit(ConnectionId id, MessageType type, std::span<const std::byte> payload,
                     std::uint16_t flags);
    void request_close(Conn* conn, int err, CloseReason why);

    void collect(MetricsSnapshot& into) const;
    bool queues_empty();

    // Startup barrier: each worker calls arrive_and_wait inside its thread.
    void arrive_startup(std::barrier<>* barrier);

  private:
    friend class UringDriver;

    void thread_main(std::barrier<>* barrier);
    void run_epoll();
    void drain_handoff();
    void drain_tasks(bool all);
    void process_deferred();
    void on_readable(Conn* conn);
    void on_writable(Conn* conn);
    bool feed(Conn* conn, const std::byte* data, std::size_t n);
    void update_epoll(Conn* conn);
    void note_high_watermark(Conn* conn);
    void note_low_watermark(Conn* conn);
    void arm_idle(Conn* conn);
    void arm_slow(Conn* conn);
    void arm_connect(Conn* conn);
    void on_timer();
    void rearm_timerfd();
    void flush_closes();
    void destroy_conn(Conn* conn);
    void close_all(CloseReason why);
    Conn* get(std::uint32_t index, std::uint32_t generation);
    Conn* install(int fd, bool already_reserved, bool connecting);
    void start_connect(const std::string& host, std::uint16_t port,
                       const std::shared_ptr<ConnectWait>& wait);
    void defer_read(Conn* conn);
    void defer_write(Conn* conn);
    int fill_iov(Conn* conn, struct iovec* iov, int max_count);
    void consume_output(Conn* conn, std::size_t n);
    void bind_outbound(std::uint32_t index, std::uint32_t generation, bool open);
    void clear_outbound(std::uint32_t index);
    void allow_output(Conn* conn);
    void flush_slot(std::uint32_t index);
    void drain_outbound();
    OutboundSlot* find_outbound(std::uint32_t index);
    std::size_t admitted_bytes(const Conn* conn);
    void touch(Conn* conn);
    void invoke_connect(Conn* conn);
    void invoke_backpressure(Conn* conn, bool paused);
    bool drain_deadline_reached() const;
    int wait_timeout_ms() const;
    bool reserve_slot();
    void release_reservation();
    void apply_affinity() const;

    static constexpr std::uint64_t kTokenWake = ~std::uint64_t{0};
    static constexpr std::uint64_t kTokenTimer = ~std::uint64_t{0} - 1;
    static constexpr std::size_t kIoBudget = 256 * 1024;
    static constexpr int kIoLoops = 32;

    std::uint32_t index_;
    WorkerOptions opt_;
    Callbacks cb_;
    std::atomic<std::uint32_t>* active_ = nullptr;
    std::atomic<bool>* stop_ = nullptr;
    std::atomic<bool>* drain_ = nullptr;
    std::atomic<std::int64_t>* drain_deadline_ns_ = nullptr;

    EpollReactor epoll_;
    EventFd wake_fd_;
    TimerFd timer_fd_;
    BufferPool pool_;
    SpscRing<int> handoff_spsc_;
    MutexQueue<int> handoff_mutex_;
    MutexQueue<Task> tasks_;

    std::vector<std::unique_ptr<Conn>> slots_;
    std::vector<std::uint32_t> generations_;
    // Grown under outbound_map_mu_. Slot objects are never freed, so a raw
    // pointer taken under the shared lock stays valid.
    std::shared_mutex outbound_map_mu_;
    std::vector<std::unique_ptr<OutboundSlot>> outbound_;
    std::mutex dirty_mu_;
    std::vector<std::uint32_t> dirty_;
    std::atomic<std::uint32_t> outbound_scheduled_{0};
    std::vector<std::uint32_t> free_list_;
    std::vector<std::uint32_t> deferred_reads_;
    std::vector<std::uint32_t> deferred_writes_;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> pending_close_;
    std::priority_queue<TimerNode, std::vector<TimerNode>, EarlierTimer> timers_;

    std::atomic<std::uint32_t> local_conns_{0};
    std::thread thread_;
    std::thread::id thread_id_{};
    std::atomic<bool> started_{false};
    std::atomic<bool> failed_{false};
    int callback_depth_ = 0;
    int io_depth_ = 0;
    void* uring_ring_ = nullptr;
    int uring_inflight_polls_ = 0;

    Counter connects_;
    Counter disconnects_;
    Counter messages_in_;
    Counter messages_out_;
    Counter bytes_in_;
    Counter bytes_out_;
    Counter send_backpressured_;
    Counter send_invalid_;
    Counter send_closed_;
    Counter protocol_errors_;
    Counter callback_exceptions_;
    Counter timeouts_idle_;
    Counter timeouts_slow_;
    Counter task_queue_full_;
    Counter sq_full_;
    Counter rejected_;
};

std::uint64_t pack_token(std::uint32_t index, std::uint32_t generation) noexcept;
void unpack_token(std::uint64_t token, std::uint32_t& index, std::uint32_t& generation) noexcept;

} // namespace fluxnet::detail
