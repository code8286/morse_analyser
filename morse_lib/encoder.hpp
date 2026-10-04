// morse_lib/encoder.hpp - text -> Morse text + Morse audio, with optional hidden
// (steganographic) payload carried in the timing of the gaps *inside* letters.
//
// Hidden-data scheme (decoded by mlib::analyse() -> AnalysisResult::carrier):
//   * Every gap between two elements of the same letter ("intra-letter gap") is a
//     carrier. Nominal length is 1 unit; a hidden bit 0 keeps it at 1 unit, a hidden
//     bit 1 stretches it to `hidden_bit1_ratio` units (default 1.6, always < 2 so the
//     visible Morse still decodes: gaps >= 2 units are read as letter breaks).
//   * Letter gaps (3 units) and word gaps (7 units) are never modified.
//   * Payload bytes are sent MSB first, in order, one bit per carrier gap. Unused
//     trailing carriers stay at bit 0 (decoded as NUL bytes, which the analyser trims).
#pragma once
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "morse.hpp"

namespace mlib {

struct EncodeOptions {
    double wpm = 20.0;          // words per minute (PARIS standard): unit = 1.2 / wpm seconds
    double frequency = 600.0;   // tone pitch in Hz
    uint32_t sample_rate = 44100;
    double ramp_ms = 5.0;       // raised-cosine attack/release to avoid key clicks
    double amplitude = 0.8;     // peak, 0 < a <= 1
    double lead_s = 0.3;        // silence before the first tone
    double tail_s = 0.3;        // silence after the last tone
    double hidden_bit1_ratio = 1.6;  // length (in units) of a carrier gap that carries a 1
    std::string hidden;         // optional hidden payload (raw bytes)
};

struct Segment {
    bool tone;
    double seconds;
};

struct EncodeResult {
    std::string morse;               // ".... . .-.. / ..." (letters ' ', words " / ")
    std::string skipped;             // distinct input characters that have no Morse code
    std::vector<Segment> timeline;   // tones/gaps incl. lead and tail silence
    std::vector<double> samples;     // mono audio in [-1, 1]
    double unit = 0;                 // seconds per unit
    double duration = 0;             // seconds
    size_t capacity_bits = 0;        // hidden bits the cover text can carry
    size_t hidden_bits = 0;          // hidden bits actually embedded
};

namespace enc_detail {

struct Parsed {
    std::vector<std::vector<std::string>> words;  // words -> letters -> code ("." / "-")
    std::string skipped;
};

inline Parsed parse(const std::string& text) {
    Parsed p;
    std::vector<std::string> cur;
    std::vector<std::string> skipped;  // distinct characters (a UTF-8 sequence counts as one)
    auto end_word = [&] {
        if (!cur.empty()) p.words.push_back(std::move(cur));
        cur.clear();
    };
    for (size_t i = 0; i < text.size(); ++i) {
        unsigned char ch = (unsigned char)text[i];
        if (std::isspace(ch)) { end_word(); continue; }
        auto it = morse_table().find(char(std::toupper(ch)));
        if (it != morse_table().end()) { cur.push_back(it->second); continue; }
        size_t len = 1;
        if (ch >= 0xF0) len = 4; else if (ch >= 0xE0) len = 3; else if (ch >= 0xC0) len = 2;
        std::string c = text.substr(i, len);
        if (std::find(skipped.begin(), skipped.end(), c) == skipped.end()) skipped.push_back(c);
        i += c.size() - 1;
    }
    end_word();
    for (auto& c : skipped) p.skipped += c;
    return p;
}

inline size_t capacity(const Parsed& p) {
    size_t n = 0;
    for (auto& w : p.words)
        for (auto& l : w) n += l.size() - 1;
    return n;
}

}  // namespace enc_detail

// Number of hidden bits `text` can carry (one per intra-letter gap).
inline size_t hidden_capacity_bits(const std::string& text) {
    return enc_detail::capacity(enc_detail::parse(text));
}

// Largest hidden payload (bytes) `text` can carry.
inline size_t hidden_capacity_bytes(const std::string& text) { return hidden_capacity_bits(text) / 8; }

inline void validate(const EncodeOptions& o) {
    auto bad = [](const std::string& m) { throw std::invalid_argument(m); };
    if (!(o.wpm >= 1.0 && o.wpm <= 100.0)) bad("speed must be between 1 and 100 WPM");
    if (!(o.sample_rate >= 8000 && o.sample_rate <= 192000)) bad("sample rate must be between 8000 and 192000 Hz");
    if (!(o.frequency >= 20.0 && o.frequency <= 0.45 * o.sample_rate))
        bad("tone frequency must be between 20 Hz and 45% of the sample rate");
    if (!(o.amplitude > 0.0 && o.amplitude <= 1.0)) bad("amplitude must be in (0, 1]");
    if (!(o.ramp_ms >= 0.0 && o.ramp_ms <= 50.0)) bad("ramp must be between 0 and 50 ms");
    if (!(o.lead_s >= 0.0 && o.lead_s <= 60.0 && o.tail_s >= 0.0 && o.tail_s <= 60.0))
        bad("lead/tail silence must be between 0 and 60 s");
    if (!(o.hidden_bit1_ratio >= 1.3 && o.hidden_bit1_ratio <= 1.9))
        bad("hidden-bit gap ratio must be between 1.3 and 1.9");
}

inline EncodeResult encode(const std::string& text, const EncodeOptions& o) {
    validate(o);
    enc_detail::Parsed p = enc_detail::parse(text);
    if (p.words.empty()) throw std::invalid_argument("nothing to encode: no character of the text has a Morse code");

    EncodeResult r;
    r.skipped = p.skipped;
    r.unit = 1.2 / o.wpm;
    r.capacity_bits = enc_detail::capacity(p);

    // Hidden payload -> bit stream (MSB first).
    std::vector<int> bits;
    for (unsigned char c : o.hidden)
        for (int b = 7; b >= 0; --b) bits.push_back((c >> b) & 1);
    if (bits.size() > r.capacity_bits)
        throw std::invalid_argument("hidden message needs " + std::to_string(bits.size()) +
                                    " bits but the cover text only carries " + std::to_string(r.capacity_bits) +
                                    " (one per gap inside letters; use a longer cover text)");
    r.hidden_bits = bits.size();

    // Timeline.
    const double U = r.unit;
    if (o.lead_s > 0) r.timeline.push_back({false, o.lead_s});
    size_t bit_index = 0;
    for (size_t w = 0; w < p.words.size(); ++w) {
        if (w) { r.timeline.push_back({false, 7 * U}); r.morse += " / "; }
        for (size_t l = 0; l < p.words[w].size(); ++l) {
            if (l) { r.timeline.push_back({false, 3 * U}); r.morse += ' '; }
            const std::string& code = p.words[w][l];
            r.morse += code;
            for (size_t e = 0; e < code.size(); ++e) {
                if (e) {
                    int bit = bit_index < bits.size() ? bits[bit_index] : 0;
                    ++bit_index;
                    r.timeline.push_back({false, (bit ? o.hidden_bit1_ratio : 1.0) * U});
                }
                r.timeline.push_back({true, (code[e] == '.' ? 1 : 3) * U});
            }
        }
    }
    if (o.tail_s > 0) r.timeline.push_back({false, o.tail_s});

    // Synthesis. Boundaries are rounded from cumulative time so rounding never drifts.
    const double sr = o.sample_rate;
    const double pi = 3.14159265358979323846;
    double total = 0;
    for (auto& s : r.timeline) total += s.seconds;
    r.duration = total;
    const size_t n_total = size_t(std::llround(total * sr));
    r.samples.assign(n_total, 0.0);
    double acc = 0;
    size_t a = 0;
    const size_t ramp_full = size_t(std::llround(o.ramp_ms * 1e-3 * sr));
    for (auto& s : r.timeline) {
        acc += s.seconds;
        size_t b = std::min(n_total, size_t(std::llround(acc * sr)));
        if (s.tone && b > a) {
            const size_t len = b - a;
            const size_t ramp = std::min(ramp_full, len / 2);
            for (size_t k = 0; k < len; ++k) {
                double env = 1.0;
                size_t edge = std::min(k, len - 1 - k);
                if (ramp && edge < ramp) env = 0.5 - 0.5 * std::cos(pi * double(edge) / double(ramp));
                r.samples[a + k] = o.amplitude * env * std::sin(2.0 * pi * o.frequency * double(a + k) / sr);
            }
        }
        a = b;
    }
    return r;
}

}  // namespace mlib
