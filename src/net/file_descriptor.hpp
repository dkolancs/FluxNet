#pragma once

namespace fluxnet::detail {

// Move-only RAII wrapper. close() is issued once. On Linux, close releases the
// descriptor even when it returns EINTR, so we do not retry (a retry could close
// a number the kernel has already reused).
class FileDescriptor {
  public:
    FileDescriptor() noexcept = default;
    explicit FileDescriptor(int fd) noexcept : fd_(fd) {}
    ~FileDescriptor() { reset(); }

    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;

    FileDescriptor(FileDescriptor&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
    FileDescriptor& operator=(FileDescriptor&& other) noexcept {
        if (this != &other) {
            reset(other.fd_);
            other.fd_ = -1;
        }
        return *this;
    }

    int get() const noexcept { return fd_; }
    explicit operator bool() const noexcept { return fd_ >= 0; }

    int release() noexcept {
        int fd = fd_;
        fd_ = -1;
        return fd;
    }

    void reset(int fd = -1) noexcept;

  private:
    int fd_ = -1;
};

} // namespace fluxnet::detail
