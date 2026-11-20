#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace netcode {

using Bytes = std::vector<std::uint8_t>;

class Writer {
public:
    void u8(std::uint8_t value);
    void u16(std::uint16_t value);
    void u32(std::uint32_t value);
    void u64(std::uint64_t value);
    void f32(float value);
    void append(const Bytes& bytes);
    const Bytes& bytes() const { return bytes_; }
    Bytes take();

private:
    Bytes bytes_;
};

class Reader {
public:
    explicit Reader(const Bytes& bytes) : Reader(bytes.data(), bytes.size()) {}
    Reader(Bytes&&) = delete;
    Reader(const std::uint8_t* data, std::size_t size) : data_(data), remaining_(size) {}

    bool u8(std::uint8_t& value);
    bool u16(std::uint16_t& value);
    bool u32(std::uint32_t& value);
    bool u64(std::uint64_t& value);
    bool f32(float& value);
    bool read(std::size_t size, Bytes& result);
    std::size_t remaining() const { return remaining_; }
    bool done() const { return remaining_ == 0; }

private:
    bool integer(std::size_t width, std::uint64_t& value);
    const std::uint8_t* data_;
    std::size_t remaining_;
};

}
