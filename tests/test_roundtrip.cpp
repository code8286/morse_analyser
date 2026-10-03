// tests/test_roundtrip.cpp - encoder unit tests and encoder -> analyser round trips.
// Exit code 0 = all passed. No third-party test framework.
#include <cmath>
#include <cstdio>
#include <random>
#include <string>

#include "../morse_lib/analysis.hpp"
#include "../morse_lib/encoder.hpp"

static int g_fail = 0, g_checks = 0;
#define CHECK(cond)                                                                      \
    do {                                                                                 \
        ++g_checks;                                                                      \
        if (!(cond)) { ++g_fail; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)

static bool throws_invalid(const std::string& text, const mlib::EncodeOptions& o) {
    try { mlib::encode(text, o); } catch (const std::invalid_argument&) { return true; } catch (...) {}
    return false;
}

static std::string expected_bits(const std::string& hidden, size_t carriers) {
    std::string b;
    for (unsigned char c : hidden)
        for (int i = 7; i >= 0; --i) b += char('0' + ((c >> i) & 1));
    b.resize(carriers, '0');
    return b;
}

static void unit_tests() {
    using namespace mlib;
    EncodeOptions o;
    o.lead_s = o.tail_s = 0;
    o.wpm = 20;

    EncodeResult r = encode("PARIS", o);
    CHECK(r.morse == ".--. .- .-. .. ...");
    CHECK(std::fabs(r.duration - 43 * 0.06) < 1e-9);  // PARIS = 43 units without the trailing word gap
    CHECK(std::fabs(r.unit - 0.06) < 1e-12);
    CHECK(r.samples.size() == size_t(std::llround(43 * 0.06 * 44100)));

    r = encode("  sos   hi ", o);  // case-insensitive, whitespace collapsed
    CHECK(r.morse == "... --- ... / .... ..");
    CHECK(r.capacity_bits == 10);
    CHECK(hidden_capacity_bits("SOS HI") == 10 && hidden_capacity_bytes("SOS HI") == 1);

    r = encode("A#B\xC3\xA9", o);  // '#' and UTF-8 bytes are skipped, and reported
    CHECK(r.morse == ".- -...");
    CHECK(r.skipped.find('#') != std::string::npos);

    CHECK(throws_invalid("", o));
    CHECK(throws_invalid("###", o));
    EncodeOptions h = o;
    h.hidden = "toolong";
    CHECK(throws_invalid("SOS", h));  // capacity 4 bits
    h.hidden = "";
    h.wpm = 0; CHECK(throws_invalid("SOS", h));
    h = o; h.frequency = 30000; CHECK(throws_invalid("SOS", h));
    h = o; h.sample_rate = 100; CHECK(throws_invalid("SOS", h));
    h = o; h.amplitude = 1.5; CHECK(throws_invalid("SOS", h));
    h = o; h.hidden_bit1_ratio = 2.0; CHECK(throws_invalid("SOS", h));

    // Carrier gaps: bit 1 lengthens exactly the intra-letter gap, nothing else.
    h = o; h.hidden = "\x80";  // 10000000 -> first carrier gap only
    EncodeResult a = encode("SOS HI", o), b = encode("SOS HI", h);
    CHECK(a.timeline.size() == b.timeline.size());
    size_t changed = 0;
    for (size_t i = 0; i < a.timeline.size(); ++i)
        if (std::fabs(a.timeline[i].seconds - b.timeline[i].seconds) > 1e-12) ++changed;
    CHECK(changed == 1);

    // Audio sanity: peak within amplitude, silent at the very start (ramped).
    EncodeOptions d; r = encode("E", d);
    double pk = 0; for (double v : r.samples) pk = std::max(pk, std::fabs(v));
    CHECK(pk <= d.amplitude + 1e-9 && pk > 0.5 * d.amplitude);
}

struct Case { double wpm, freq; unsigned rate; const char* text; const char* hidden; double noise; };

static void roundtrip(const Case& c) {
    using namespace mlib;
    EncodeOptions o;
    o.wpm = c.wpm; o.frequency = c.freq; o.sample_rate = c.rate; o.hidden = c.hidden;
    EncodeResult e = encode(c.text, o);
    if (c.noise > 0) {
        std::mt19937 rng(1234);
        std::normal_distribution<double> nd(0.0, c.noise);
        for (double& v : e.samples) v += nd(rng);
    }
    // Through a real 16-bit WAV image, as the GUI/CLI do.
    auto bytes = wav16_bytes(e.samples, c.rate);
    std::string path = "rt_tmp.wav";
    {
        FILE* f = std::fopen(path.c_str(), "wb");
        std::fwrite(bytes.data(), 1, bytes.size(), f);
        std::fclose(f);
    }
    Audio a = read_wav(path);
    std::remove(path.c_str());
    AnalysisConfig cfg; cfg.spectrogram = false;
    AnalysisResult r = analyse(a, cfg);

    std::string up = c.text;
    for (char& ch : up) ch = char(std::toupper((unsigned char)ch));
    bool vis_ok = r.has_gaps && r.visible.text == up;
    std::string bits; for (int b : r.carrier.bits) bits += char('0' + b);
    bool bits_ok = r.has_gaps && bits == expected_bits(c.hidden, e.capacity_bits);
    bool det_ok = r.carrier.detected == (std::string(c.hidden).size() > 0);
    bool txt_ok = r.carrier.text == c.hidden;
    CHECK(vis_ok); CHECK(bits_ok); CHECK(det_ok); CHECK(txt_ok);
    if (!(vis_ok && bits_ok && det_ok && txt_ok))
        std::printf("   case wpm=%g f=%g sr=%u noise=%g text='%s' hidden='%s'\n   visible='%s' carrier='%s'\n", c.wpm,
                    c.freq, c.rate, c.noise, c.text, c.hidden, r.visible.text.c_str(), r.carrier.text.c_str());
}

int main() {
    unit_tests();
    const char* cover = "THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG";
    const double wpms[] = {10, 15, 25, 40};
    const double freqs[] = {400, 1000};
    const unsigned rates[] = {8000, 22050, 48000};
    for (double w : wpms)
        for (double f : freqs)
            for (unsigned sr : rates) roundtrip({w, f, sr, cover, "Hi!", 0});
    roundtrip({20, 600, 44100, cover, "", 0});                   // no hidden data
    roundtrip({20, 600, 44100, cover, "~~~~~~~~", 0});           // many 1-bits
    roundtrip({20, 600, 44100, cover, "        ", 0});           // spaces
    roundtrip({20, 600, 44100, cover, "Secret k", 0.02});       // light noise
    roundtrip({15, 700, 22050, "SOS HI", "A", 0});                // minimal capacity
    roundtrip({20, 600, 44100, "The Quick Brown Fox", "Hi!!", 0});
    roundtrip({20, 600, 44100, "TM OT", "", 0});              // dashes only
    roundtrip({20, 600, 44100, "EE I S", "", 0});             // dots only
    roundtrip({20, 600, 44100, "OOOO MMMM", "A", 0});         // dashes only, with hidden data
    std::printf("%d checks, %d failed\n", g_checks, g_fail);
    std::printf(g_fail ? "TESTS FAILED\n" : "TESTS PASSED\n");
    return g_fail ? 1 : 0;
}
