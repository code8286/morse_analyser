// morse-analyser.cpp
// C++17 port of morse_stego_analyser.py - decomposes an audio file, extracts the
// Morse tone bursts and analyses the *gaps* between them for hidden (steganographic)
// bits, exactly like the original Python pipeline:
//
//   load -> Hilbert envelope -> 20 Hz Butterworth low-pass (zero phase)
//        -> percentile threshold -> tone bursts -> gaps -> 3-means unit estimate
//        -> gap-modulation bits -> ASCII candidates (normal + reversed)
//        -> CSV + envelope plot (SVG) + spectrogram (PNG)
//
// On top of the port it also decodes the *visible* Morse text from tone timings and
// the hidden payload written by morse-encoder (carrier-gap scheme, see encoder.hpp).
//
// The pipeline itself lives in morse_lib/analysis.hpp (shared with the GUI); this file
// is only the command-line front end:
//
//     g++ -O2 -std=c++17 morse-analyser.cpp -o morse-analyser
//     ./morse-analyser morse_stego.wav [--outdir DIR] [--csv FILE] [--no-plots]

#include <cstdio>
#include <fstream>
#include <iomanip>
#include <string>

#include "morse_lib/analysis.hpp"

struct Config {
    std::string audio_file = "morse_stego.wav";
    mlib::AnalysisConfig analysis;
    std::string output_csv = "morse_timings.csv";
    std::string outdir = ".";
    bool plots = true;
};

static std::string join(const std::string& dir, const std::string& f) {
    if (dir.empty() || dir == "." || f.find('/') != std::string::npos || f.find('\\') != std::string::npos) return f;
    return dir + "/" + f;
}

static void usage(const char* prog) {
    std::printf(
        "Usage: %s [audio.wav] [options]\n"
        "  --csv FILE        output CSV for gap timings   (default morse_timings.csv)\n"
        "  --outdir DIR      directory for CSV/plot output (default .)\n"
        "  --cutoff HZ       envelope low-pass cutoff      (default 20)\n"
        "  --min-tone SEC    minimum tone length           (default 0.005)\n"
        "  --no-plots        skip SVG/PNG plot generation\n",
        prog);
}

int main(int argc, char** argv) {
    Config cfg;
    try {
        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i];
            auto need = [&](const char* name) -> std::string {
                if (i + 1 >= argc) throw std::runtime_error(std::string("missing value for ") + name);
                return argv[++i];
            };
            if (a == "-h" || a == "--help") { usage(argv[0]); return 0; }
            else if (a == "--csv") cfg.output_csv = need("--csv");
            else if (a == "--outdir") cfg.outdir = need("--outdir");
            else if (a == "--cutoff") cfg.analysis.envelope_cutoff = std::stod(need("--cutoff"));
            else if (a == "--min-tone") cfg.analysis.min_tone_length = std::stod(need("--min-tone"));
            else if (a == "--no-plots") cfg.plots = false;
            else if (!a.empty() && a[0] == '-') throw std::runtime_error("unknown option " + a);
            else cfg.audio_file = a;
        }
        cfg.analysis.spectrogram = cfg.plots;

        mlib::Audio audio = mlib::read_wav(cfg.audio_file);
        mlib::AnalysisResult r = mlib::analyse(audio, cfg.analysis);

        std::fputs(mlib::format_header(r).c_str(), stdout);
        if (!r.has_gaps) throw std::runtime_error("fewer than two tone bursts found - nothing to analyse");
        std::fputs(mlib::format_body(r).c_str(), stdout);

        // Save measurements
        std::string csv_path = join(cfg.outdir, cfg.output_csv);
        {
            std::ofstream csv(csv_path);
            if (!csv) throw std::runtime_error("cannot write " + csv_path);
            csv << "gap_seconds,bit\n" << std::setprecision(17);
            for (size_t i = 0; i < r.gaps.size(); ++i) csv << r.gaps[i] << "," << r.bits[i] << "\n";
        }
        std::printf("\n[+] Saved %s\n", csv_path.c_str());

        if (cfg.plots) {
            std::string svg = join(cfg.outdir, "tone_detection.svg");
            mlib::write_envelope_svg(svg, r.envelope, r.sample_rate, r.tones);
            std::printf("[+] Tone detection plot saved: %s\n", svg.c_str());
            const mlib::Spectrogram& sp = r.spectrogram;
            std::string png = join(cfg.outdir, "spectrogram.png");
            mlib::write_png(png, sp.width, sp.height, sp.pixels);
            std::printf("[+] Spectrogram (0-%.0f Hz, %.1f..%.1f dB, %zu frames) saved: %s\n", sp.fmax, sp.lo, sp.hi,
                        sp.frames, png.c_str());
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[-] Error: %s\n", e.what());
        return 1;
    }
    return 0;
}
