// morse_lib/wav.hpp - minimal RIFF/WAVE reader & writer (no external deps)
// Reads PCM 8/16/24/32-bit and IEEE float 32/64-bit (incl. WAVE_FORMAT_EXTENSIBLE).
// Multi-channel audio is mixed down to mono by averaging (like librosa mono=True).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace mlib {

struct Audio {
    std::vector<double> samples;  // mono, range roughly [-1, 1]
    uint32_t sample_rate = 0;
    double duration() const { return sample_rate ? double(samples.size()) / sample_rate : 0.0; }
};

namespace detail {
inline uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
inline uint32_t rd32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
}  // namespace detail

// `path` may be a narrow string (implicit conversion) or a std::filesystem::path
// (use this for Unicode paths on Windows).
inline Audio read_wav(const std::filesystem::path& path) {
    using namespace detail;
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open file: " + path.u8string());
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (buf.size() < 12 || std::memcmp(buf.data(), "RIFF", 4) != 0 || std::memcmp(buf.data() + 8, "WAVE", 4) != 0)
        throw std::runtime_error("not a RIFF/WAVE file (only .wav is supported): " + path.u8string());

    uint16_t fmt_tag = 0, channels = 0, bits = 0;
    uint32_t rate = 0;
    const uint8_t* data = nullptr;
    size_t data_len = 0;

    size_t pos = 12;
    while (pos + 8 <= buf.size()) {
        const uint8_t* ck = buf.data() + pos;
        uint32_t len = rd32(ck + 4);
        size_t body = pos + 8;
        if (std::memcmp(ck, "fmt ", 4) == 0 && body + 16 <= buf.size()) {
            fmt_tag = rd16(buf.data() + body);
            channels = rd16(buf.data() + body + 2);
            rate = rd32(buf.data() + body + 4);
            bits = rd16(buf.data() + body + 14);
            if (fmt_tag == 0xFFFE && len >= 26 && body + 26 <= buf.size())
                fmt_tag = rd16(buf.data() + body + 24);  // sub-format GUID's first 2 bytes
        } else if (std::memcmp(ck, "data", 4) == 0) {
            data = buf.data() + body;
            data_len = std::min<size_t>(len, buf.size() - body);
            break;
        }
        pos = body + len + (len & 1);
    }
    if (!data || !channels || !rate) throw std::runtime_error("malformed WAV (missing fmt/data chunk)");
    if (!((fmt_tag == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32)) ||
          (fmt_tag == 3 && (bits == 32 || bits == 64))))
        throw std::runtime_error("unsupported WAV encoding (format " + std::to_string(fmt_tag) + ", " +
                                 std::to_string(bits) + " bit)");

    const size_t bps = bits / 8;
    const size_t frames = data_len / (bps * channels);
    Audio a;
    a.sample_rate = rate;
    a.samples.resize(frames);
    for (size_t i = 0; i < frames; ++i) {
        double acc = 0;
        for (unsigned c = 0; c < channels; ++c) {
            const uint8_t* p = data + (i * channels + c) * bps;
            double v;
            if (fmt_tag == 3) {
                if (bits == 32) { float x; std::memcpy(&x, p, 4); v = x; }
                else { double x; std::memcpy(&x, p, 8); v = x; }
            } else if (bits == 8) {
                v = (int(p[0]) - 128) / 128.0;
            } else if (bits == 16) {
                v = int16_t(rd16(p)) / 32768.0;
            } else if (bits == 24) {
                int32_t x = int32_t((uint32_t(p[0]) << 8) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 24)) >> 8;
                v = x / 8388608.0;
            } else {
                v = int32_t(rd32(p)) / 2147483648.0;
            }
            acc += v;
        }
        a.samples[i] = acc / channels;
    }
    return a;
}

// Serialises mono samples ([-1,1], clipped) as a complete 16-bit PCM WAV image
// (little-endian regardless of host). Used for files and for in-memory playback.
inline std::vector<uint8_t> wav16_bytes(const std::vector<double>& s, uint32_t rate) {
    std::vector<uint8_t> b;
    b.reserve(44 + s.size() * 2);
    auto p32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i))); };
    auto p16 = [&](uint16_t v) { b.push_back(uint8_t(v & 0xFF)); b.push_back(uint8_t(v >> 8)); };
    auto tag = [&](const char* t) { for (int i = 0; i < 4; ++i) b.push_back(uint8_t(t[i])); };
    const uint32_t bytes = uint32_t(s.size() * 2);
    tag("RIFF"); p32(36 + bytes); tag("WAVE");
    tag("fmt "); p32(16); p16(1); p16(1); p32(rate); p32(rate * 2); p16(2); p16(16);
    tag("data"); p32(bytes);
    for (double v : s) {
        if (!(v == v)) v = 0;  // NaN
        if (v > 1) v = 1;
        if (v < -1) v = -1;
        p16(uint16_t(int16_t(std::lround(v * 32767.0))));
    }
    return b;
}

// 16-bit mono PCM writer (used by tests / by the encoder app).
inline void write_wav16(const std::filesystem::path& path, const std::vector<double>& s, uint32_t rate) {
    std::ofstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot write file: " + path.u8string());
    auto b = wav16_bytes(s, rate);
    f.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
    f.flush();
    if (!f) throw std::runtime_error("error while writing file: " + path.u8string());
}

}  // namespace mlib
