// tests/gen_test_wav.cpp - writes a clean Morse WAV for smoke-testing the analyser.
// Usage: gen_test_wav out.wav [TEXT] [wpm-unit-seconds]
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include "../morse_lib/morse.hpp"
#include "../morse_lib/wav.hpp"

int main(int argc, char** argv) {
    std::string out = argc > 1 ? argv[1] : "test.wav";
    std::string text = argc > 2 ? argv[2] : "SOS HI";
    double U = argc > 3 ? std::stod(argv[3]) : 0.06;
    const unsigned sr = 22050;
    const double freq = 600.0, pi = 3.14159265358979323846;
    std::vector<double> s(size_t(0.3 * sr), 0.0);
    auto tone = [&](double dur) {
        size_t n = size_t(dur * sr);
        for (size_t i = 0; i < n; ++i) {
            double t = double(i) / sr, ramp = std::min(1.0, std::min(t, dur - t) / 0.003);
            s.push_back(0.8 * ramp * std::sin(2 * pi * freq * t));
        }
    };
    auto gap = [&](double dur) { s.insert(s.end(), size_t(dur * sr), 0.0); };
    std::string m = mlib::text_to_morse(text);
    for (size_t i = 0; i < m.size(); ++i) {
        char c = m[i];
        if (c == '.' || c == '-') {
            tone((c == '.' ? 1 : 3) * U);
            char n = i + 1 < m.size() ? m[i + 1] : 0;
            if (n == '.' || n == '-') gap(U);
        } else if (c == ' ') {
            if (i + 1 < m.size() && m[i + 1] == '/') continue;
            gap(3 * U);
        } else if (c == '/') gap(7 * U);
    }
    gap(0.3);
    mlib::write_wav16(out, s, sr);
    std::printf("wrote %s (%s)\n", out.c_str(), m.c_str());
}
