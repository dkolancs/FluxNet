#include "protocol/codec.hpp"

#include <gtest/gtest.h>

using fluxnet::MessageType;
using fluxnet::MessageView;
using fluxnet::detail::DecodeError;
using fluxnet::detail::FrameDecoder;
using fluxnet::detail::write_frame;

namespace {
std::vector<std::byte> frame(MessageType type, std::uint16_t flags, std::string_view payload) {
    std::vector<std::byte> out(8 + payload.size());
    std::vector<std::byte> body(payload.size());
    for (std::size_t i = 0; i < payload.size(); ++i) {
        body[i] = static_cast<std::byte>(payload[i]);
    }
    std::size_t written = 0;
    EXPECT_TRUE(write_frame(out.data(), out.size(), type, flags, body, 1024 * 1024, written));
    out.resize(written);
    return out;
}
} // namespace

TEST(Codec, RoundTripAndEmpty) {
    FrameDecoder decoder(1024, 2048);
    auto bytes = frame(MessageType::Ping, 7, "");
    int calls = 0;
    ASSERT_TRUE(decoder.feed(bytes.data(), bytes.size(), [&](MessageView msg) {
        ++calls;
        EXPECT_EQ(msg.type(), MessageType::Ping);
        EXPECT_EQ(msg.flags(), 7);
        EXPECT_TRUE(msg.payload().empty());
        return true;
    }));
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(decoder.error(), DecodeError::Ok);
}

TEST(Codec, FragmentedHeaderAndPayload) {
    FrameDecoder decoder(1024, 4096);
    auto bytes = frame(MessageType::Data, 1, "hello");
    std::string got;
    for (std::byte b : bytes) {
        ASSERT_TRUE(decoder.feed(&b, 1, [&](MessageView msg) {
            for (auto byte : msg.payload()) {
                got.push_back(static_cast<char>(byte));
            }
            return true;
        }));
    }
    EXPECT_EQ(got, "hello");
}

TEST(Codec, MultipleFramesOneBuffer) {
    auto a = frame(MessageType::Data, 0, "a");
    auto b = frame(MessageType::Pong, 0, "bb");
    a.insert(a.end(), b.begin(), b.end());
    FrameDecoder decoder(1024, 4096);
    std::vector<std::string> got;
    ASSERT_TRUE(decoder.feed(a.data(), a.size(), [&](MessageView msg) {
        std::string text;
        for (auto byte : msg.payload()) {
            text.push_back(static_cast<char>(byte));
        }
        got.push_back(text);
        return true;
    }));
    ASSERT_EQ(got.size(), 2u);
    EXPECT_EQ(got[0], "a");
    EXPECT_EQ(got[1], "bb");
}

TEST(Codec, RejectsHugeLengthBeforeAlloc) {
    std::byte header[8] = {};
    header[0] = std::byte{0xff};
    header[1] = std::byte{0xff};
    header[2] = std::byte{0xff};
    header[3] = std::byte{0xff};
    header[4] = std::byte{0};
    header[5] = std::byte{1};
    FrameDecoder decoder(1024, 4096);
    EXPECT_FALSE(decoder.feed(header, 8, [](MessageView) { return true; }));
    EXPECT_EQ(decoder.error(), DecodeError::FrameTooLarge);
    EXPECT_LT(decoder.buffered(), 64u);
}

TEST(Codec, RejectsBadType) {
    std::byte header[8] = {};
    header[5] = std::byte{9};
    FrameDecoder decoder(1024, 4096);
    EXPECT_FALSE(decoder.feed(header, 8, [](MessageView) { return true; }));
    EXPECT_EQ(decoder.error(), DecodeError::BadType);
}
