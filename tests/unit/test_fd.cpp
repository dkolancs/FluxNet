#include "net/file_descriptor.hpp"

#include <gtest/gtest.h>
#include <unistd.h>

TEST(FileDescriptor, ClosesOnceOnMove) {
    int fds[2] = {-1, -1};
    ASSERT_EQ(::pipe(fds), 0);
    {
        fluxnet::detail::FileDescriptor a(fds[0]);
        fluxnet::detail::FileDescriptor b(std::move(a));
        EXPECT_FALSE(a);
        EXPECT_EQ(b.get(), fds[0]);
    }
    // The pipe read end is closed. A second close of the same number is not performed.
    EXPECT_EQ(::close(fds[0]), -1);
    EXPECT_EQ(errno, EBADF);
    EXPECT_EQ(::close(fds[1]), 0);
}

TEST(FileDescriptor, ReleaseTransfersOwnership) {
    int fds[2] = {-1, -1};
    ASSERT_EQ(::pipe(fds), 0);
    int raw = -1;
    {
        fluxnet::detail::FileDescriptor fd(fds[0]);
        raw = fd.release();
        EXPECT_FALSE(fd);
    }
    EXPECT_EQ(raw, fds[0]);
    EXPECT_EQ(::close(raw), 0);
    EXPECT_EQ(::close(fds[1]), 0);
}
