#include "net/file_descriptor.hpp"

#include <unistd.h>

namespace fluxnet::detail {

void FileDescriptor::reset(int fd) noexcept {
    if (fd_ >= 0) {
        ::close(fd_);
    }
    fd_ = fd;
}

} // namespace fluxnet::detail
