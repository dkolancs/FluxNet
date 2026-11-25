#include "protocol/codec.hpp"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    fluxnet::detail::FrameDecoder decoder(1u << 20, 2u << 20);
    decoder.feed(reinterpret_cast<const std::byte*>(data), size,
                 [](fluxnet::MessageView) { return true; });
    return 0;
}
