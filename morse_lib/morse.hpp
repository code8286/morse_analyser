// morse_lib/morse.hpp - International Morse table, text <-> symbol conversion and
// timing-based decoding of tone/gap measurements.
#pragma once
#include <algorithm>
#include <cctype>
#include <map>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include "stats.hpp"

namespace mlib {

inline const std::map<char, std::string>& morse_table() {
    static const std::map<char, std::string> t = {
        {'A', ".-"},   {'B', "-..."}, {'C', "-.-."}, {'D', "-.."},   {'E', "."},     {'F', "..-."},  {'G', "--."},
        {'H', "...."}, {'I', ".."},   {'J', ".---"}, {'K', "-.-"},   {'L', ".-.."},  {'M', "--"},    {'N', "-."},
        {'O', "---"},  {'P', ".--."}, {'Q', "--.-"}, {'R', ".-."},   {'S', "..."},   {'T', "-"},     {'U', "..-"},
        {'V', "...-"}, {'W', ".--"},  {'X', "-..-"}, {'Y', "-.--"},  {'Z', "--.."},  {'0', "-----"}, {'1', ".----"},
        {'2', "..---"}, {'3', "...--"}, {'4', "....-"}, {'5', "....."}, {'6', "-...."}, {'7', "--..."},
        {'8', "---.."}, {'9', "----."}, {'.', ".-.-.-"}, {',', "--..--"}, {'?', "..--.."}, {'\'', ".----."},
        {'!', "-.-.--"}, {'/', "-..-."}, {'(', "-.--."}, {')', "-.--.-"}, {'&', ".-..."}, {':', "---..."},
        {';', "-.-.-."}, {'=', "-...-"}, {'+', ".-.-."}, {'-', "-....-"}, {'_', "..--.-"}, {'"', ".-..-."},
        {'$', "...-..-"}, {'@', ".--.-."}};
    return t;
}

// "HELLO WORLD" -> ".... . .-.. .-.. --- / .-- --- .-. .-.. -.."
inline std::string text_to_morse(const std::string& text) {
    std::ostringstream o;
    bool first = true;
    for (char ch : text) {
        if (ch == ' ') { o << " /"; first = false; continue; }
        auto it = morse_table().find(char(std::toupper((unsigned char)ch)));
        if (it == morse_table().end()) continue;
        if (!first) o << ' ';
        o << it->second;
        first = false;
    }
    return o.str();
}

// ".... .. / ..." -> "HI S"; unknown groups become '?'.
inline std::string morse_to_text(const std::string& morse) {
    static std::map<std::string, char> rev;
    if (rev.empty())
        for (auto& [c, s] : morse_table()) rev[s] = c;
    std::string out, cur;
    auto flush = [&] {
        if (cur.empty()) return;
        auto it = rev.find(cur);
        out += it == rev.end() ? '?' : it->second;
        cur.clear();
    };
    for (char c : morse) {
        if (c == '.' || c == '-') cur += c;
        else if (c == '/') { flush(); out += ' '; }
        else flush();
    }
    flush();
    return out;
}

struct MorseDecode {
    std::string symbols;  // e.g. ".... . .-.. / ..."
    std::string text;
    double dot_unit = 0;  // seconds
    // One entry per gap between consecutive tones: 0 = intra-letter, 1 = letter break, 2 = word break.
    std::vector<int> gap_kind;
};

// Decode visible Morse from tone durations and the gaps between them.
// tones[i] = (start, end, duration). Dot/dash split by 2-means over durations;
// gaps: < 2 units = intra-letter, < 5 units = letter break, otherwise word break.
inline MorseDecode decode_timings(const std::vector<std::tuple<double, double, double>>& tones) {
    MorseDecode r;
    if (tones.empty()) return r;
    std::vector<double> dur;
    for (auto& t : tones) dur.push_back(std::get<2>(t));
    auto c = kmeans_1d(dur, 2);
    double dot = c.front(), split = 0;
    if (c.size() == 2 && c[1] > 1.8 * c[0]) split = 0.5 * (c[0] + c[1]);
    else {
        // Only one tone length. Dots or dashes? The shortest gap is (at least) one unit, so
        // tones >= 2.2x longer than it are dashes. (Ambiguous when no gap is a plain
        // 1-unit gap, e.g. "TT": then it is read as dots.)
        double gmin = 1e18;
        for (size_t i = 0; i + 1 < tones.size(); ++i)
            gmin = std::min(gmin, std::get<0>(tones[i + 1]) - std::get<1>(tones[i]));
        double mean = 0;
        for (double d : dur) mean += d;
        mean /= double(dur.size());
        if (gmin < 1e17 && gmin > 0 && mean >= 2.2 * gmin) {
            dot = gmin;
            split = 0;  // every tone is a dash
        } else {
            split = 1e18;  // all dots
        }
    }
    r.dot_unit = dot;
    for (size_t i = 0; i < tones.size(); ++i) {
        r.symbols += dur[i] > split ? '-' : '.';
        if (i + 1 < tones.size()) {
            double gap = std::get<0>(tones[i + 1]) - std::get<1>(tones[i]);
            int kind = gap >= 5 * dot ? 2 : (gap >= 2 * dot ? 1 : 0);
            r.gap_kind.push_back(kind);
            if (kind == 2) r.symbols += " / ";
            else if (kind == 1) r.symbols += ' ';
        }
    }
    r.text = morse_to_text(r.symbols);
    return r;
}

}  // namespace mlib
