#include "protocol/codec.hpp"

#include <gtest/gtest.h>

#include <random>

TEST(Fuzz, RandomSlicesStayInsideTheDecoder) {
    constexpr int kSeeds = 200;
    constexpr int kBytes = 256;
    for (int seed = 1; seed <= kSeeds; ++seed) {
        std::mt19937 rng(static_cast<std::uint32_t>(seed));
        std::uniform_int_distribution<int> byte_dist(0, 255);
        std::vector<std::byte> data(kBytes);
        for (auto& b : data) {
            b = static_cast<std::byte>(byte_dist(rng));
        }
        fluxnet::detail::FrameDecoder decoder(4096, 8192);
        std::size_t off = 0;
        bool failed = false;
        while (off < data.size()) {
            std::uniform_int_distribution<int> len_dist(1, static_cast<int>(data.size() - off));
            const std::size_t n = static_cast<std::size_t>(len_dist(rng));
            try {
                if (!decoder.feed(data.data() + off, n,
                                  [](fluxnet::MessageView) { return true; })) {
                    failed = true;
                    break;
                }
            } catch (...) {
                FAIL() << "decoder threw on seed " << seed;
            }
            off += n;
        }
        if (failed) {
            EXPECT_NE(decoder.error(), fluxnet::detail::DecodeError::Ok);
        }
    }
}

TEST(Fuzz, WellFormedFramesSurviveSlicing) {
    std::vector<std::byte> bytes;
    for (int i = 0; i < 30; ++i) {
        std::string payload(static_cast<std::size_t>(i), static_cast<char>('a' + (i % 26)));
        std::vector<std::byte> body(payload.size());
        for (std::size_t j = 0; j < payload.size(); ++j) {
            body[j] = static_cast<std::byte>(payload[j]);
        }
        std::vector<std::byte> one(8 + body.size());
        std::size_t written = 0;
        ASSERT_TRUE(fluxnet::detail::write_frame(one.data(), one.size(), fluxnet::MessageType::Data,
                                                 0, body, 1024, written));
        bytes.insert(bytes.end(), one.begin(), one.begin() + static_cast<std::ptrdiff_t>(written));
    }
    fluxnet::detail::FrameDecoder decoder(1024, 4096);
    int messages = 0;
    for (std::size_t i = 0; i < bytes.size();) {
        const std::size_t n = std::min<std::size_t>(3, bytes.size() - i);
        ASSERT_TRUE(decoder.feed(bytes.data() + i, n, [&](fluxnet::MessageView) {
            ++messages;
            return true;
        }));
        i += n;
    }
    EXPECT_EQ(messages, 30);
}
