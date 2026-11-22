#include "session/discovery.hpp"
#include "state/value.hpp"
#include <algorithm>
#include <random>

namespace netcode {
namespace {
bool text_ok(const std::string& value) {
    return !value.empty() && valid_value({1, FieldType::String, 80}, Value{value}) &&
           std::none_of(value.begin(), value.end(), [](unsigned char c) { return c < 32 || c == 127; });
}
void write_string(Writer& w, const std::string& s) {
    w.u8(uint8_t(s.size()));
    w.append(Bytes(s.begin(), s.end()));
}
bool read_string(Reader& r, std::string& s) {
    uint8_t n;
    Bytes b;
    if (!r.u8(n) || n > 80 || !r.read(n, b))
        return false;
    s.assign(b.begin(), b.end());
    return text_ok(s);
}
}
bool RoomFilter::matches(const RoomListing& r) const {
    if ((!region.empty() && r.region != region) || (!build.empty() && r.build != build) ||
        (require_space && (!r.joinable || r.occupied >= r.capacity)))
        return false;
    for (const auto& [key, value] : properties) {
        auto it = r.properties.find(key);
        if (it == r.properties.end() || it->second != value)
            return false;
    }
    return true;
}
bool RoomDirectory::join_random(const RoomFilter& filter, Joined complete) {
    if (!complete) return false;
    auto candidates = search(filter);
    if (candidates.empty()) return false;
    thread_local std::mt19937 engine{std::random_device{}()};
    std::uniform_int_distribution<size_t> index(0,candidates.size()-1);
    return join(candidates[index(engine)].room,std::move(complete));
}
bool valid_listing(const RoomListing& r) {
    if (!r.room || !r.capacity || !r.occupied || r.occupied > r.capacity || !text_ok(r.name) ||
        !text_ok(r.region) || !text_ok(r.build) || r.properties.size() > 8)
        return false;
    size_t bytes = 32 + r.name.size() + r.region.size() + r.build.size();
    for (const auto& [k, v] : r.properties) {
        if (!text_ok(k) || !text_ok(v))
            return false;
        bytes += 2 + k.size() + v.size();
    }
    return bytes <= 1000;
}
Bytes encode_listing(const RoomListing& r) {
    if (!valid_listing(r))
        return {};
    Writer w;
    w.u32(0x3144524e);
    w.u64(r.room);
    w.u16(r.port);
    w.u8(r.occupied);
    w.u8(r.capacity);
    w.u8(r.joinable);
    write_string(w, r.name);
    write_string(w, r.region);
    write_string(w, r.build);
    w.u8(uint8_t(r.properties.size()));
    for (const auto& [k, v] : r.properties) {
        write_string(w, k);
        write_string(w, v);
    }
    return w.take();
}
bool decode_listing(const Bytes& data, RoomListing& result) {
    Reader r(data);
    RoomListing out;
    uint32_t magic;
    uint8_t joinable, n;
    if (!r.u32(magic) || magic != 0x3144524e || !r.u64(out.room) || !r.u16(out.port) || !r.u8(out.occupied) ||
        !r.u8(out.capacity) || !r.u8(joinable) || joinable > 1 || !read_string(r, out.name) ||
        !read_string(r, out.region) || !read_string(r, out.build) || !r.u8(n) || n > 8)
        return false;
    out.joinable = joinable != 0;
    for (unsigned i = 0; i < n; ++i) {
        std::string k, v;
        if (!read_string(r, k) || !read_string(r, v) || !out.properties.emplace(k, v).second)
            return false;
    }
    if (!r.done() || !valid_listing(out))
        return false;
    result = std::move(out);
    return true;
}
}
