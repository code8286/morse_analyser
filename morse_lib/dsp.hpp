// morse_lib/dsp.hpp - FFT, Hilbert transform, Butterworth low-pass, zero-phase
// filtering and STFT. Drop-in equivalents of the scipy routines used by the
// original Python analyser (hilbert, butter, filtfilt, stft).
#pragma once
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <stdexcept>
#include <vector>

namespace mlib {

using cd = std::complex<double>;
constexpr double kPi = 3.14159265358979323846;

inline size_t next_pow2(size_t n) {
    size_t p = 1;
    while (p < n) p <<= 1;
    return p;
}

// In-place iterative radix-2 FFT. a.size() must be a power of two.
inline void fft_pow2(std::vector<cd>& a, bool inverse = false) {
    const size_t n = a.size();
    if (n & (n - 1)) throw std::invalid_argument("fft_pow2: size must be a power of 2");
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = 2 * kPi / double(len) * (inverse ? 1 : -1);
        const size_t half = len / 2;
        std::vector<cd> tw(half);
        for (size_t k = 0; k < half; ++k) tw[k] = std::polar(1.0, ang * double(k));
        for (size_t i = 0; i < n; i += len)
            for (size_t k = 0; k < half; ++k) {
                cd u = a[i + k], v = a[i + k + half] * tw[k];
                a[i + k] = u + v;
                a[i + k + half] = u - v;
            }
    }
    if (inverse)
        for (auto& x : a) x /= double(n);
}

// Analytic signal via FFT (scipy.signal.hilbert). The input is zero-padded to a
// power of two for speed; only the original length is returned.
inline std::vector<cd> hilbert(const std::vector<double>& x) {
    const size_t N = x.size(), M = next_pow2(std::max<size_t>(N, 2));
    std::vector<cd> X(M, cd(0, 0));
    for (size_t i = 0; i < N; ++i) X[i] = x[i];
    fft_pow2(X);
    for (size_t k = 1; k < M / 2; ++k) X[k] *= 2.0;
    // X[M/2] kept (x1), negative frequencies zeroed
    for (size_t k = M / 2 + 1; k < M; ++k) X[k] = 0;
    fft_pow2(X, true);
    X.resize(N);
    return X;
}

// ---------------------------------------------------------------- filtering

struct IirBA {
    std::vector<double> b, a;  // a[0] == 1
};

// Digital Butterworth low-pass, cutoff Wn normalised to Nyquist (0..1).
// Same design path as scipy.signal.butter(N, Wn, 'low') (bilinear transform).
inline IirBA butter_lowpass(int order, double Wn) {
    if (Wn <= 0 || Wn >= 1) throw std::invalid_argument("butter: Wn must be in (0,1)");
    const double fs = 2.0, fs2 = 2.0 * fs;
    const double warped = 2.0 * fs * std::tan(kPi * Wn / fs);
    std::vector<cd> pa, pz;
    for (int k = 0; k < order; ++k) {
        cd p = std::polar(1.0, kPi * double(2 * k + order + 1) / double(2 * order));
        p *= warped;
        pa.push_back(p);
        pz.push_back((fs2 + p) / (fs2 - p));
    }
    cd prod(1, 0);
    for (auto& p : pa) prod *= (fs2 - p);
    double k = std::pow(warped, order) / prod.real();

    auto poly = [](const std::vector<cd>& roots) {
        std::vector<cd> c{cd(1, 0)};
        for (auto& r : roots) {
            std::vector<cd> n(c.size() + 1, cd(0, 0));
            for (size_t i = 0; i < c.size(); ++i) {
                n[i] += c[i];
                n[i + 1] -= c[i] * r;
            }
            c = n;
        }
        return c;
    };
    std::vector<cd> zeros(order, cd(-1, 0));
    auto bc = poly(zeros), ac = poly(pz);
    IirBA f;
    for (auto& v : bc) f.b.push_back(k * v.real());
    for (auto& v : ac) f.a.push_back(v.real());
    return f;
}

// Direct-form II transposed IIR filter. zi (size n-1) is updated in place.
inline std::vector<double> lfilter(const IirBA& f, const std::vector<double>& x, std::vector<double> zi) {
    const size_t n = f.a.size();
    std::vector<double> y(x.size());
    for (size_t i = 0; i < x.size(); ++i) {
        double yi = f.b[0] * x[i] + (n > 1 ? zi[0] : 0.0);
        for (size_t j = 1; j < n; ++j) {
            double nxt = (j < n - 1) ? zi[j] : 0.0;
            zi[j - 1] = nxt + f.b[j] * x[i] - f.a[j] * yi;
        }
        y[i] = yi;
    }
    return y;
}

// Steady-state initial conditions for a unit step (scipy.signal.lfilter_zi).
inline std::vector<double> lfilter_zi(const IirBA& f) {
    const int m = int(f.a.size()) - 1;
    std::vector<std::vector<double>> A(m, std::vector<double>(m + 1, 0.0));
    // (I - C^T) zi = B - a[1:]*b[0]; C = companion(a)
    for (int i = 0; i < m; ++i) {
        for (int j = 0; j < m; ++j) {
            // companion(a): C[0][c] = -a[c+1], C[r][r-1] = 1;  entry of C^T is C[j][i]
            double c = (j == 0 ? -f.a[i + 1] : 0.0) + (j == i + 1 ? 1.0 : 0.0);
            A[i][j] = (i == j ? 1.0 : 0.0) - c;
        }
        A[i][m] = f.b[i + 1] - f.a[i + 1] * f.b[0];
    }
    for (int c = 0; c < m; ++c) {  // Gaussian elimination with partial pivoting
        int piv = c;
        for (int r = c + 1; r < m; ++r)
            if (std::fabs(A[r][c]) > std::fabs(A[piv][c])) piv = r;
        std::swap(A[c], A[piv]);
        for (int r = 0; r < m; ++r) {
            if (r == c) continue;
            double fct = A[r][c] / A[c][c];
            for (int k = c; k <= m; ++k) A[r][k] -= fct * A[c][k];
        }
    }
    std::vector<double> zi(m);
    for (int i = 0; i < m; ++i) zi[i] = A[i][m] / A[i][i];
    return zi;
}

// Zero-phase forward/backward filter with odd-extension padding
// (scipy.signal.filtfilt defaults).
inline std::vector<double> filtfilt(const IirBA& f, const std::vector<double>& x) {
    const size_t n = x.size();
    if (n < 2) throw std::invalid_argument("filtfilt: need at least 2 samples");
    size_t padlen = 3 * std::max(f.a.size(), f.b.size());
    if (padlen > n - 1) padlen = n - 1;
    std::vector<double> ext;
    ext.reserve(n + 2 * padlen);
    for (size_t i = padlen; i >= 1; --i) ext.push_back(2 * x[0] - x[i]);
    ext.insert(ext.end(), x.begin(), x.end());
    for (size_t i = 1; i <= padlen; ++i) ext.push_back(2 * x[n - 1] - x[n - 1 - i]);

    auto zi = lfilter_zi(f);
    std::vector<double> z(zi.size());
    for (size_t i = 0; i < z.size(); ++i) z[i] = zi[i] * ext.front();
    auto y = lfilter(f, ext, z);
    std::reverse(y.begin(), y.end());
    for (size_t i = 0; i < z.size(); ++i) z[i] = zi[i] * y.front();
    y = lfilter(f, y, z);
    std::reverse(y.begin(), y.end());
    return std::vector<double>(y.begin() + padlen, y.begin() + padlen + n);
}

// --------------------------------------------------------------------- STFT

// Streaming STFT: calls cb(frame_index, magnitudes) for each frame so very long
// recordings never need the whole spectrogram in memory. Matches scipy defaults:
// periodic Hann window, 50 % overlap, zero-padded boundary + tail.
template <class Callback>
inline size_t stft_stream(const std::vector<double>& x, size_t nperseg, Callback cb) {
    const size_t hop = nperseg / 2, bins = nperseg / 2 + 1;
    std::vector<double> win(nperseg);
    double wsum = 0;
    for (size_t i = 0; i < nperseg; ++i) {
        win[i] = 0.5 - 0.5 * std::cos(2 * kPi * double(i) / double(nperseg));
        wsum += win[i];
    }
    const size_t ext_len = x.size() + 2 * (nperseg / 2);
    size_t padded = ext_len;
    if ((padded - (nperseg - hop)) % hop) padded += hop - (padded - (nperseg - hop)) % hop;
    const size_t frames = padded < nperseg ? 1 : (padded - nperseg) / hop + 1;

    std::vector<cd> buf(nperseg);
    std::vector<float> mag(bins);
    for (size_t k = 0; k < frames; ++k) {
        for (size_t i = 0; i < nperseg; ++i) {
            long idx = long(k * hop + i) - long(nperseg / 2);
            double v = (idx >= 0 && size_t(idx) < x.size()) ? x[size_t(idx)] : 0.0;
            buf[i] = v * win[i];
        }
        fft_pow2(buf);
        for (size_t b = 0; b < bins; ++b) mag[b] = float(std::abs(buf[b]) / wsum);
        cb(k, mag);
    }
    return frames;
}

}  // namespace mlib
