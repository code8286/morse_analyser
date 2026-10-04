// morse_lib/stats.hpp - percentile and 1-D k-means (replacement for numpy/sklearn)
#pragma once
#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
#include <vector>

namespace mlib {

// numpy.percentile (linear interpolation), q in [0,100].
inline double percentile(std::vector<double> v, double q) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    double pos = q / 100.0 * double(v.size() - 1);
    size_t lo = size_t(std::floor(pos)), hi = size_t(std::ceil(pos));
    return v[lo] + (v[hi] - v[lo]) * (pos - double(lo));
}

// 1-D k-means: k-means++ seeding, Lloyd iterations, best of n_init runs.
// Returns the cluster centres sorted ascending. If there are fewer distinct
// values than k, fewer centres are returned.
inline std::vector<double> kmeans_1d(const std::vector<double>& x, int k, int n_init = 10, unsigned seed = 0,
                                     int max_iter = 300) {
    std::vector<double> uniq(x);
    std::sort(uniq.begin(), uniq.end());
    uniq.erase(std::unique(uniq.begin(), uniq.end()), uniq.end());
    if (x.empty()) return {};
    if (int(uniq.size()) <= k) return uniq;

    std::mt19937 rng(seed);
    std::vector<double> best;
    double best_inertia = std::numeric_limits<double>::infinity();

    for (int run = 0; run < n_init; ++run) {
        // k-means++ seeding
        std::vector<double> c;
        c.push_back(x[rng() % x.size()]);
        while (int(c.size()) < k) {
            std::vector<double> d2(x.size());
            double tot = 0;
            for (size_t i = 0; i < x.size(); ++i) {
                double m = std::numeric_limits<double>::infinity();
                for (double cc : c) m = std::min(m, (x[i] - cc) * (x[i] - cc));
                d2[i] = m;
                tot += m;
            }
            double r = std::uniform_real_distribution<double>(0, tot)(rng), acc = 0;
            size_t pick = x.size() - 1;
            for (size_t i = 0; i < x.size(); ++i) {
                acc += d2[i];
                if (acc >= r && d2[i] > 0) { pick = i; break; }
            }
            c.push_back(x[pick]);
        }
        // Lloyd
        std::vector<int> lab(x.size(), -1);
        for (int it = 0; it < max_iter; ++it) {
            bool changed = false;
            std::vector<double> sum(k, 0.0);
            std::vector<int> cnt(k, 0);
            for (size_t i = 0; i < x.size(); ++i) {
                int bi = 0;
                for (int j = 1; j < k; ++j)
                    if (std::fabs(x[i] - c[j]) < std::fabs(x[i] - c[bi])) bi = j;
                if (lab[i] != bi) { lab[i] = bi; changed = true; }
                sum[bi] += x[i];
                cnt[bi]++;
            }
            for (int j = 0; j < k; ++j)
                if (cnt[j]) c[j] = sum[j] / cnt[j];
            if (!changed) break;
        }
        double inertia = 0;
        for (size_t i = 0; i < x.size(); ++i) inertia += (x[i] - c[lab[i]]) * (x[i] - c[lab[i]]);
        if (inertia < best_inertia) { best_inertia = inertia; best = c; }
    }
    std::sort(best.begin(), best.end());
    return best;
}

}  // namespace mlib
