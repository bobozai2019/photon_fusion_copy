#include "transport/wire.hpp"

#include <iostream>

int main()
{
    using namespace netcode;
    Writer writer;
    writer.u16(0x1234);
    writer.u32(0x78563412);
    writer.u64(0xfedcba9876543210);
    writer.f32(-0.5f);
    writer.f32(0.75f);

    const Bytes bytes = writer.take();
    const Bytes expected{0x34, 0x12, 0x12, 0x34, 0x56, 0x78, 0x10, 0x32, 0x54, 0x76,
                         0x98, 0xba, 0xdc, 0xfe, 0, 0, 0, 0xbf, 0, 0, 0x40, 0x3f};
    if (bytes != expected || !writer.bytes().empty()) {
        std::cerr << "Unexpected wire layout\n";
        return 1;
    }

    Reader reader(bytes);
    std::uint16_t short_value;
    std::uint32_t word;
    std::uint64_t large;
    float negative, positive;
    if (!reader.u16(short_value) || short_value != 0x1234 ||
        !reader.u32(word) || word != 0x78563412 ||
        !reader.u64(large) || large != 0xfedcba9876543210 ||
        !reader.f32(negative) || negative != -0.5f ||
        !reader.f32(positive) || positive != 0.75f || !reader.done()) {
        std::cerr << "Wire round trip failed\n";
        return 1;
    }
    Reader truncated(bytes.data(), 3);
    word = 42;
    if (truncated.u32(word) || word != 42 || truncated.remaining() != 3) {
        std::cerr << "Truncated read changed output or cursor\n";
        return 1;
    }
}
