// morse_lib/analysis.hpp - the complete analysis pipeline as a reusable library
// (shared by the command-line analyser and the GUI).
//
//   normalise -> Hilbert envelope -> low-pass (zero phase) -> threshold -> tone bursts
//   -> gaps -> visible Morse text -> 3-means gap clusters -> gap-modulation bits
//   -> ASCII candidates (original heuristic) -> carrier-gap decoder (encoder.hpp scheme)
//   -> spectrogram image
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

#include "dsp.hpp"
#include "image.hpp"
#include "morse.hpp"
#include "stats.hpp"
#include "wav.hpp"

namespace mlib {

using Tone = std::tuple<double, double, double>;  // start, end, duration (seconds)

struct AnalysisConfig {
    double envelope_cutoff = 20.0;   // Hz
    double min_tone_length = 0.005;  // seconds
    bool spectrogram = true;
};

struct Spectrogram {
    int width = 0, height = 0;
    std::vector<RGB> pixels;  // row-major, low frequencies at the bottom
    double fmax = 0, lo = 0, hi = 0;  // Hz range shown; dB range of the colour map
    size_t frames = 0;
};

// Result of the carrier-gap decoder (see encoder.hpp for the scheme).
struct CarrierResult {
    size_t carriers = 0;      // intra-letter gaps examined
    std::vector<int> bits;    // one bit per carrier
    bool detected = false;    // at least one carrier was stretched
    std::string text;         // bytes (MSB first) -> printable ASCII, trailing NULs trimmed
};

struct AnalysisResult {
    uint32_t sample_rate = 0;
    double duration = 0;
    std::vector<double> envelope;  // normalised to a peak of 1
    double threshold = 0;
    std::vector<Tone> tones;
    std::vector<double> gaps;
    bool has_gaps = false;         // false if fewer than two tones were found (rest is empty)
    MorseDecode visible;
    std::vector<double> gap_clusters;
    double unit = 0;
    std::vector<int> bits;                // original heuristic, one per gap
    std::vector<std::string> ascii;       // [0] normal, [1] reversed bit order
    CarrierResult carrier;
    Spectrogram spectrogram;
    bool has_spectrogram = false;
};

namespace an_detail {

inline std::string fmt(const char* f, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}

inline std::vector<double> get_envelope(const std::vector<double>& audio, double sr, double cutoff) {
    auto analytic = hilbert(audio);
    std::vector<double> env(audio.size());
    for (size_t i = 0; i < env.size(); ++i) env[i] = std::abs(analytic[i]);
    auto ba = butter_lowpass(3, cutoff / (sr / 2.0));
    env = filtfilt(ba, env);
    double mx = *std::max_element(env.begin(), env.end());
    if (mx > 0)
        for (double& v : env) v /= mx;
    return env;
}

inline double detect_threshold(const std::vector<double>& env) {
    double noise = percentile(env, 20);
    double peak = percentile(env, 95);
    return noise + (peak - noise) * 0.35;
}

inline std::vector<Tone> detect_tones(const std::vector<double>& env, double sr, double threshold, double min_len) {
    const size_t n = env.size();
    std::vector<size_t> starts, ends;
    for (size_t i = 0; i + 1 < n; ++i) {
        bool a0 = env[i] > threshold, a1 = env[i + 1] > threshold;
        if (!a0 && a1) starts.push_back(i);
        else if (a0 && !a1) ends.push_back(i);
    }
    // If the file starts mid-tone, drop the orphan end so start/end pairs stay aligned.
    if (!ends.empty() && (starts.empty() || ends.front() < starts.front())) ends.erase(ends.begin());
    if (ends.size() < starts.size()) ends.push_back(n - 1);

    std::vector<Tone> tones;
    for (size_t k = 0; k < std::min(starts.size(), ends.size()); ++k) {
        double duration = double(ends[k] - starts[k]) / sr;
        if (duration > min_len) tones.emplace_back(double(starts[k]) / sr, double(ends[k]) / sr, duration);
    }
    return tones;
}

inline std::vector<std::string> bits_to_ascii(const std::vector<int>& bits) {
    std::vector<std::string> output;
    for (int reverse = 0; reverse < 2; ++reverse) {
        std::vector<int> data(bits);
        if (reverse) std::reverse(data.begin(), data.end());
        std::string chars;
        for (long i = 0; i < long(data.size()) - 7; i += 8) {
            int value = 0;
            for (int j = 0; j < 8; ++j) value = (value << 1) | data[i + j];
            chars += (value >= 32 && value <= 126) ? char(value) : '.';
        }
        output.push_back(chars);
    }
    return output;
}

// Carrier decoder: look only at gaps classified as intra-letter by the visible decode.
// Threshold between a "0" (~1 unit) and a "1" (~1.6 units) gap: midpoint of the two
// 2-means centres when both kinds are present, otherwise 1.2 x the dot length (the
// tone-measured dot is slightly longer than the gap-measured unit, so this stays safe).
inline CarrierResult decode_carriers(const std::vector<double>& gaps, const MorseDecode& vis) {
    CarrierResult c;
    std::vector<double> cg;
    for (size_t i = 0; i < gaps.size() && i < vis.gap_kind.size(); ++i)
        if (vis.gap_kind[i] == 0) cg.push_back(gaps[i]);
    c.carriers = cg.size();
    if (cg.empty()) return c;

    double lo = *std::min_element(cg.begin(), cg.end());
    double hi = *std::max_element(cg.begin(), cg.end());
    double thr = 1.2 * vis.dot_unit;
    if (lo > 0 && hi / lo >= 1.2) {
        auto centres = kmeans_1d(cg, 2);
        if (centres.size() == 2) thr = 0.5 * (centres[0] + centres[1]);
    }
    for (double g : cg) c.bits.push_back(g > thr ? 1 : 0);
    c.detected = std::any_of(c.bits.begin(), c.bits.end(), [](int b) { return b == 1; });

    std::string bytes;
    for (size_t i = 0; i + 8 <= c.bits.size(); i += 8) {
        int v = 0;
        for (int j = 0; j < 8; ++j) v = (v << 1) | c.bits[i + j];
        bytes += char(v);
    }
    while (!bytes.empty() && bytes.back() == 0) bytes.pop_back();
    for (unsigned char ch : bytes) c.text += (ch >= 32 && ch <= 126) ? char(ch) : '.';
    return c;
}

inline Spectrogram make_spectrogram(const std::vector<double>& audio, double sr) {
    const size_t nperseg = 4096;
    const double bin_hz = sr / double(nperseg);
    Spectrogram sp;
    sp.fmax = std::min(20000.0, sr / 2.0);
    const size_t maxbin = std::min<size_t>(nperseg / 2, size_t(sp.fmax / bin_hz));

    size_t frames = 0;
    {
        size_t ext = audio.size() + nperseg, hop = nperseg / 2, padded = ext;
        if ((padded - (nperseg - hop)) % hop) padded += hop - (padded - (nperseg - hop)) % hop;
        frames = padded < nperseg ? 1 : (padded - nperseg) / hop + 1;
    }
    sp.frames = frames;
    const int W = int(std::min<size_t>(frames, 1600));
    const int H = int(std::min<size_t>(maxbin + 1, 512));
    std::vector<double> sum(size_t(W) * H, 0.0);
    std::vector<int> cnt(size_t(W) * H, 0);

    stft_stream(audio, nperseg, [&](size_t k, const std::vector<float>& mag) {
        int x = int(k * size_t(W) / frames);
        for (size_t b = 0; b <= maxbin; ++b) {
            int y = int(b * size_t(H) / (maxbin + 1));
            double db = 20.0 * std::log10(double(mag[b]) + 1e-10);
            sum[size_t(y) * W + x] += db;
            cnt[size_t(y) * W + x]++;
        }
    });

    double lo = 1e300, hi = -1e300;
    for (size_t i = 0; i < sum.size(); ++i)
        if (cnt[i]) {
            sum[i] /= cnt[i];
            lo = std::min(lo, sum[i]);
            hi = std::max(hi, sum[i]);
        }
    sp.lo = lo;
    sp.hi = hi;
    sp.width = W;
    sp.height = H;
    sp.pixels.resize(size_t(W) * H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            size_t src = size_t(H - 1 - y) * W + x;
            sp.pixels[size_t(y) * W + x] = colormap(hi > lo ? (sum[src] - lo) / (hi - lo) : 0.0);
        }
    return sp;
}

}  // namespace an_detail

// Runs the whole pipeline. Throws std::runtime_error for empty/silent input.
// If fewer than two tones are found, `has_gaps` is false and later fields stay empty
// (callers decide how to report it).
inline AnalysisResult analyse(const Audio& audio, const AnalysisConfig& cfg = {}) {
    using namespace an_detail;
    if (audio.samples.empty() || audio.sample_rate == 0) throw std::runtime_error("audio file contains no samples");
    std::vector<double> x = audio.samples;
    double peak = 0;
    for (double v : x) peak = std::max(peak, std::fabs(v));
    if (peak == 0) throw std::runtime_error("audio is silent");
    for (double& v : x) v /= peak;

    AnalysisResult r;
    r.sample_rate = audio.sample_rate;
    r.duration = audio.duration();
    const double sr = audio.sample_rate;

    r.envelope = get_envelope(x, sr, cfg.envelope_cutoff);
    r.threshold = detect_threshold(r.envelope);
    r.tones = detect_tones(r.envelope, sr, r.threshold, cfg.min_tone_length);
    for (size_t i = 0; i + 1 < r.tones.size(); ++i)
        r.gaps.push_back(std::get<0>(r.tones[i + 1]) - std::get<1>(r.tones[i]));

    if (!r.gaps.empty()) {
        r.has_gaps = true;
        r.visible = decode_timings(r.tones);
        r.gap_clusters = kmeans_1d(r.gaps, 3, 10, 0);
        r.unit = r.gap_clusters.front();
        const double expected = r.unit;
        for (double g : r.gaps) {
            double error = g - expected;
            r.bits.push_back((std::fabs(error) > r.unit * 0.25) ? (error > 0 ? 1 : 0) : 0);
        }
        r.ascii = bits_to_ascii(r.bits);
        r.carrier = decode_carriers(r.gaps, r.visible);
    }
    if (cfg.spectrogram) {
        r.spectrogram = make_spectrogram(x, sr);
        r.has_spectrogram = true;
    }
    return r;
}

// First four lines of the report (also printed before the "nothing to analyse" error).
inline std::string format_header(const AnalysisResult& r) {
    using an_detail::fmt;
    return fmt("[+] Sample rate: %u\n", r.sample_rate) + fmt("[+] Duration: %.2fs\n", r.duration) +
           fmt("[+] Detection threshold: %.3f\n", r.threshold) + fmt("[+] Tone bursts detected: %zu\n", r.tones.size());
}

// Everything after the header. Requires r.has_gaps.
inline std::string format_body(const AnalysisResult& r) {
    using an_detail::fmt;
    std::string o;
    o += fmt("\n[+] Visible Morse (dot unit %.4fs):\n%s\n=> %s\n", r.visible.dot_unit, r.visible.symbols.c_str(),
             r.visible.text.c_str());
    o += "[+] Gap clusters: [";
    for (size_t i = 0; i < r.gap_clusters.size(); ++i) o += fmt("%s%.8f", i ? " " : "", r.gap_clusters[i]);
    o += "]\n";
    o += fmt("[+] Estimated Morse unit: %.4fs\n", r.unit);
    o += "\n[+] Hidden bits:\n";
    for (int b : r.bits) o += char('0' + b);
    o += "\n\n[+] ASCII candidates:\n";
    for (auto& t : r.ascii) o += t + "\n";

    o += fmt("\n[+] Hidden carrier bits (%zu gaps inside letters):\n", r.carrier.carriers);
    for (int b : r.carrier.bits) o += char('0' + b);
    o += "\n";
    if (r.carrier.detected) o += "[+] Hidden carrier text: \"" + r.carrier.text + "\"\n";
    else o += "[+] No hidden carrier modulation detected.\n";
    return o;
}

inline std::string format_report(const AnalysisResult& r) {
    std::string o = format_header(r);
    if (r.has_gaps) o += format_body(r);
    else o += "\n[-] Fewer than two tone bursts found - nothing to analyse.\n";
    return o;
}

}  // namespace mlib
