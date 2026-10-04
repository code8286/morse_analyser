// morse_lib/image.hpp - dependency-free output: PNG writer (stored deflate) and
// SVG envelope plot. Replaces matplotlib in the original Python tool.
#pragma once
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace mlib {

struct RGB { uint8_t r, g, b; };

// Viridis-like colour map, t in [0,1].
inline RGB colormap(double t) {
    static const double stops[5][3] = {{68, 1, 84}, {59, 82, 139}, {33, 145, 140}, {94, 201, 98}, {253, 231, 37}};
    t = std::min(1.0, std::max(0.0, t)) * 4.0;
    int i = std::min(3, int(t));
    double f = t - i;
    auto mix = [&](int c) { return uint8_t(stops[i][c] + (stops[i + 1][c] - stops[i][c]) * f + 0.5); };
    return {mix(0), mix(1), mix(2)};
}

namespace detail {
inline uint32_t crc32(const uint8_t* d, size_t n, uint32_t crc = 0) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    crc = ~crc;
    for (size_t i = 0; i < n; ++i) crc = table[(crc ^ d[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}
inline void put32be(std::vector<uint8_t>& v, uint32_t x) {
    for (int s = 24; s >= 0; s -= 8) v.push_back(uint8_t(x >> s));
}
inline void chunk(std::ofstream& f, const char* type, const std::vector<uint8_t>& data) {
    std::vector<uint8_t> out;
    put32be(out, uint32_t(data.size()));
    std::vector<uint8_t> td(type, type + 4);
    td.insert(td.end(), data.begin(), data.end());
    out.insert(out.end(), td.begin(), td.end());
    put32be(out, crc32(td.data(), td.size()));
    f.write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()));
}
}  // namespace detail

// pixels: row-major RGB, width*height entries.
inline void write_png(const std::string& path, int w, int h, const std::vector<RGB>& px) {
    using namespace detail;
    std::ofstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot write " + path);
    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    f.write(reinterpret_cast<const char*>(sig), 8);

    std::vector<uint8_t> ihdr;
    put32be(ihdr, uint32_t(w));
    put32be(ihdr, uint32_t(h));
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
    chunk(f, "IHDR", ihdr);

    std::vector<uint8_t> raw;
    raw.reserve(size_t(h) * (size_t(w) * 3 + 1));
    for (int y = 0; y < h; ++y) {
        raw.push_back(0);
        for (int x = 0; x < w; ++x) {
            const RGB& p = px[size_t(y) * w + x];
            raw.push_back(p.r); raw.push_back(p.g); raw.push_back(p.b);
        }
    }
    std::vector<uint8_t> z{0x78, 0x01};
    size_t pos = 0;
    do {
        size_t n = std::min<size_t>(65535, raw.size() - pos);
        bool last = pos + n >= raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(uint8_t(n & 0xFF)); z.push_back(uint8_t(n >> 8));
        z.push_back(uint8_t(~n & 0xFF)); z.push_back(uint8_t((~n >> 8) & 0xFF));
        z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
        pos += n;
    } while (pos < raw.size());
    uint32_t a = 1, b = 0;
    for (uint8_t c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
    put32be(z, (b << 16) | a);
    chunk(f, "IDAT", z);
    chunk(f, "IEND", {});
}

// Envelope + detected tone bursts as an SVG (the "Tone Detection" debug view).
inline void write_envelope_svg(const std::string& path, const std::vector<double>& env, double sr,
                               const std::vector<std::tuple<double, double, double>>& tones,
                               const std::string& title = "Tone Detection") {
    std::ofstream f(path);
    if (!f) throw std::runtime_error("cannot write " + path);
    const int W = 1400, H = 500, L = 60, R = 20, T = 40, B = 50;
    const double dur = double(env.size()) / sr, pw = W - L - R, ph = H - T - B;
    auto X = [&](double t) { return L + t / dur * pw; };
    auto Y = [&](double v) { return T + (1.0 - v) * ph; };
    f << "<svg xmlns='http://www.w3.org/2000/svg' width='" << W << "' height='" << H
      << "' font-family='sans-serif' font-size='12'>\n<rect width='100%' height='100%' fill='white'/>\n";
    f << "<text x='" << W / 2 << "' y='24' text-anchor='middle' font-size='16'>" << title << "</text>\n";
    for (auto& [s, e, d] : tones) {
        (void)d;
        f << "<rect x='" << X(s) << "' y='" << T << "' width='" << std::max(1.0, X(e) - X(s)) << "' height='" << ph
          << "' fill='#1f77b4' fill-opacity='0.3'/>\n";
    }
    // decimate to <= ~3000 points (peak per bucket)
    const size_t target = 3000, step = std::max<size_t>(1, env.size() / target);
    f << "<polyline fill='none' stroke='#ff7f0e' stroke-width='1' points='";
    for (size_t i = 0; i < env.size(); i += step) {
        double m = env[i];
        for (size_t j = i; j < std::min(env.size(), i + step); ++j) m = std::max(m, env[j]);
        f << X(double(i) / sr) << "," << Y(m) << " ";
    }
    f << "'/>\n";
    f << "<rect x='" << L << "' y='" << T << "' width='" << pw << "' height='" << ph << "' fill='none' stroke='#444'/>\n";
    for (int i = 0; i <= 10; ++i) {
        double t = dur * i / 10;
        f << "<text x='" << X(t) << "' y='" << H - 28 << "' text-anchor='middle'>" << int(t * 100 + 0.5) / 100.0 << "</text>\n";
    }
    for (int i = 0; i <= 4; ++i)
        f << "<text x='" << L - 6 << "' y='" << Y(i / 4.0) + 4 << "' text-anchor='end'>" << i / 4.0 << "</text>\n";
    f << "<text x='" << W / 2 << "' y='" << H - 6 << "' text-anchor='middle'>Seconds</text>\n";
    f << "<rect x='" << W - 160 << "' y='" << T + 6 << "' width='12' height='12' fill='#ff7f0e'/><text x='" << W - 142
      << "' y='" << T + 16 << "'>Envelope</text>\n";
    f << "</svg>\n";
}

}  // namespace mlib
