#include "transport/wire.hpp"

#include <cstring>
#include <limits>
#include <utility>

namespace netcode {
namespace {

static_assert(sizeof(float) == sizeof(std::uint32_t) && std::numeric_limits<float>::is_iec559);

void append_integer(Bytes& bytes, std::size_t width, std::uint64_t value)
{
    for (std::size_t n = 0; n < width; ++n) {
        bytes.push_back(static_cast<std::uint8_t>(value));
        value >>= 8;
    }
}

}

void Writer::u8(std::uint8_t value) { bytes_.push_back(value); }
void Writer::u16(std::uint16_t value) { append_integer(bytes_, 2, value); }
void Writer::u32(std::uint32_t value) { append_integer(bytes_, 4, value); }
void Writer::u64(std::uint64_t value) { append_integer(bytes_, 8, value); }

void Writer::f32(float value)
{
    std::uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    u32(bits);
}

void Writer::append(const Bytes& bytes)
{
    bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
}

Bytes Writer::take() { return std::exchange(bytes_, {}); }

bool Reader::integer(std::size_t width, std::uint64_t& value)
{
    if (remaining_ < width) {
        return false;
    }
    value = 0;
    for (std::size_t n = 0; n < width; ++n) {
        value |= static_cast<std::uint64_t>(data_[n]) << (8 * n);
    }
    data_ += width;
    remaining_ -= width;
    return true;
}

bool Reader::u8(std::uint8_t& value)
{
    std::uint64_t number;
    if (!integer(1, number)) return false;
    value = static_cast<std::uint8_t>(number);
    return true;
}

bool Reader::u16(std::uint16_t& value)
{
    std::uint64_t number;
    if (!integer(2, number)) return false;
    value = static_cast<std::uint16_t>(number);
    return true;
}

bool Reader::u32(std::uint32_t& value)
{
    std::uint64_t number;
    if (!integer(4, number)) return false;
    value = static_cast<std::uint32_t>(number);
    return true;
}

bool Reader::u64(std::uint64_t& value) { return integer(8, value); }

bool Reader::f32(float& value)
{
    std::uint32_t bits;
    if (!u32(bits)) return false;
    std::memcpy(&value, &bits, sizeof(bits));
    return true;
}

bool Reader::read(std::size_t size, Bytes& result)
{
    if (size > remaining_) return false;
    if (size == 0) {
        result.clear();
        return true;
    }
    result.assign(data_, data_ + size);
    data_ += size;
    remaining_ -= size;
    return true;
}

}
