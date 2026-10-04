// morse-encoder.cpp - command-line text -> Morse text + Morse audio (WAV) encoder.
//
//   morse-encoder "HELLO WORLD" [-o out.wav] [--wpm N] [--freq HZ] [--rate HZ]
//                 [--hidden "secret"] [--ramp MS] [--amp A] [--lead S] [--tail S]
//                 [--text-only]
//
// Prints the Morse text; unless --text-only is given also writes the WAV (default
// morse.wav). --hidden embeds a payload in the timing of the gaps inside letters,
// which `morse-analyser` reports under "Hidden carrier bits".
#include <cstdio>
#include <iostream>
#include <string>

#include "morse_lib/encoder.hpp"
#include "morse_lib/wav.hpp"

static void usage(const char* prog) {
    std::printf(
        "Usage: %s TEXT [options]\n"
        "  -o, --out FILE    output WAV (default morse.wav)\n"
        "  --text-only       print Morse text only, write no audio\n"
        "  --wpm N           speed in words per minute      (default 20)\n"
        "  --freq HZ         tone frequency                 (default 600)\n"
        "  --rate HZ         sample rate                    (default 44100)\n"
        "  --ramp MS         attack/release ramp            (default 5)\n"
        "  --amp A           amplitude 0..1                 (default 0.8)\n"
        "  --lead S, --tail S  silence before/after         (default 0.3)\n"
        "  --hidden MSG      hide MSG in the gap timing (needs enough letters; 8 bits per character)\n"
        "  -h, --help        this help\n",
        prog);
}

int main(int argc, char** argv) {
    try {
        mlib::EncodeOptions o;
        std::string text, out = "morse.wav";
        bool have_text = false, text_only = false;
        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i];
            auto need = [&]() -> std::string {
                if (i + 1 >= argc) throw std::runtime_error("missing value for " + a);
                return argv[++i];
            };
            if (a == "-h" || a == "--help") { usage(argv[0]); return 0; }
            else if (a == "-o" || a == "--out") out = need();
            else if (a == "--text-only") text_only = true;
            else if (a == "--wpm") o.wpm = std::stod(need());
            else if (a == "--freq") o.frequency = std::stod(need());
            else if (a == "--rate") o.sample_rate = uint32_t(std::stoul(need()));
            else if (a == "--ramp") o.ramp_ms = std::stod(need());
            else if (a == "--amp") o.amplitude = std::stod(need());
            else if (a == "--lead") o.lead_s = std::stod(need());
            else if (a == "--tail") o.tail_s = std::stod(need());
            else if (a == "--hidden") o.hidden = need();
            else if (a.size() > 1 && a[0] == '-') throw std::runtime_error("unknown option " + a);
            else if (!have_text) { text = a; have_text = true; }
            else throw std::runtime_error("unexpected argument " + a + " (quote the text)");
        }
        if (!have_text) { usage(argv[0]); return 1; }

        mlib::EncodeResult r = mlib::encode(text, o);
        std::printf("%s\n", r.morse.c_str());
        if (!r.skipped.empty()) std::fprintf(stderr, "[!] skipped characters without Morse code: %s\n", r.skipped.c_str());
        if (!text_only) {
            mlib::write_wav16(out, r.samples, o.sample_rate);
            std::fprintf(stderr, "[+] wrote %s (%.2fs, unit %.4fs, hidden %zu/%zu bits)\n", out.c_str(), r.duration,
                         r.unit, r.hidden_bits, r.capacity_bits);
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[-] Error: %s\n", e.what());
        return 1;
    }
    return 0;
}
