#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

namespace eng {

// Little-endian binary writer. Every supported target (x86_64 Linux/Windows)
// is little-endian, so trivially-copyable values are memcpy'd as-is.
class ByteWriter {
public:
    template <typename T>
    void write(const T& value) {
        static_assert(std::is_trivially_copyable_v<T>);
        const auto* p = reinterpret_cast<const uint8_t*>(&value);
        m_data.insert(m_data.end(), p, p + sizeof(T));
    }

    void u8(uint8_t v) { write(v); }
    void u16(uint16_t v) { write(v); }
    void u32(uint32_t v) { write(v); }
    void u64(uint64_t v) { write(v); }
    void i16(int16_t v) { write(v); }
    void f32(float v) { write(v); }
    void boolean(bool v) { u8(v ? 1 : 0); }

    // LEB128-style: 1 byte for < 128, which most counts/ids are.
    void varU32(uint32_t v) {
        while (v >= 0x80) {
            u8(static_cast<uint8_t>(v | 0x80));
            v >>= 7;
        }
        u8(static_cast<uint8_t>(v));
    }

    // Fixed-point: value * scale rounded into an int16 (e.g. scale 64 gives
    // 1/64-unit precision over ±512 units — plenty for room coordinates).
    void quant16(float v, float scale) {
        float s = v * scale;
        if (s > 32767.0f) s = 32767.0f;
        if (s < -32768.0f) s = -32768.0f;
        i16(static_cast<int16_t>(s >= 0 ? s + 0.5f : s - 0.5f));
    }

    void str(const std::string& s) {
        varU32(static_cast<uint32_t>(s.size()));
        m_data.insert(m_data.end(), s.begin(), s.end());
    }

    void bytes(const void* data, size_t size) {
        const auto* p = static_cast<const uint8_t*>(data);
        m_data.insert(m_data.end(), p, p + size);
    }

    const std::vector<uint8_t>& data() const { return m_data; }
    std::vector<uint8_t>& data() { return m_data; }
    size_t size() const { return m_data.size(); }
    void clear() { m_data.clear(); }

private:
    std::vector<uint8_t> m_data;
};

// Bounds-checked reader over untrusted bytes (anything off the network).
// Reading past the end never touches out-of-range memory: it returns a
// zero value and latches ok() to false, so callers can parse a whole
// message and check once at the end.
class ByteReader {
public:
    ByteReader(const void* data, size_t size) : m_data(static_cast<const uint8_t*>(data)), m_size(size) {}
    explicit ByteReader(const std::vector<uint8_t>& v) : ByteReader(v.data(), v.size()) {}

    template <typename T>
    T read() {
        static_assert(std::is_trivially_copyable_v<T>);
        T value{};
        if (!m_ok || m_pos + sizeof(T) > m_size) {
            m_ok = false;
            return value;
        }
        std::memcpy(&value, m_data + m_pos, sizeof(T));
        m_pos += sizeof(T);
        return value;
    }

    uint8_t u8() { return read<uint8_t>(); }
    uint16_t u16() { return read<uint16_t>(); }
    uint32_t u32() { return read<uint32_t>(); }
    uint64_t u64() { return read<uint64_t>(); }
    int16_t i16() { return read<int16_t>(); }
    float f32() { return read<float>(); }
    bool boolean() { return u8() != 0; }

    uint32_t varU32() {
        uint32_t result = 0;
        for (int shift = 0; shift < 35; shift += 7) {
            uint8_t b = u8();
            if (!m_ok) return 0;
            result |= static_cast<uint32_t>(b & 0x7F) << shift;
            if (!(b & 0x80)) return result;
        }
        m_ok = false;  // more than 5 bytes: malformed
        return 0;
    }

    float quant16(float scale) { return static_cast<float>(i16()) / scale; }

    std::string str(size_t maxLen = 256) {
        uint32_t len = varU32();
        if (!m_ok || len > maxLen || m_pos + len > m_size) {
            m_ok = false;
            return {};
        }
        std::string s(reinterpret_cast<const char*>(m_data + m_pos), len);
        m_pos += len;
        return s;
    }

    bool ok() const { return m_ok; }
    bool atEnd() const { return m_pos >= m_size; }
    size_t remaining() const { return m_ok && m_pos < m_size ? m_size - m_pos : 0; }
    const uint8_t* cursor() const { return m_data + m_pos; }
    void skip(size_t n) {
        if (m_pos + n > m_size) m_ok = false;
        else m_pos += n;
    }

private:
    const uint8_t* m_data;
    size_t m_size;
    size_t m_pos = 0;
    bool m_ok = true;
};

}  // namespace eng
