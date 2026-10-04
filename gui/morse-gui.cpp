// gui/morse-gui.cpp - native Win32 front end joining the encoder and the analyser.
//
//   Encoder tab : text -> Morse text + audio (play / save WAV / save text), optional
//                 hidden message carried in the gap timing.
//   Analyser tab: open or drop a WAV (or send the encoder's audio straight across),
//                 see visible Morse, hidden bits / text, envelope plot, spectrogram.
//
// Pure Win32 + the header-only morse_lib; no third-party code. Windows only.
// Build: see Makefile (`make gui` under MinGW/MSYS2) or CMakeLists.txt.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <mmsystem.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "../morse_lib/analysis.hpp"
#include "../morse_lib/encoder.hpp"

#ifdef _MSC_VER
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#endif

namespace {

enum Id : int {
    ID_TAB = 100,
    ID_ENC_TEXT = 200, ID_ENC_HIDDEN, ID_ENC_CAP, ID_ENC_WPM, ID_ENC_FREQ, ID_ENC_RATE, ID_ENC_VOL,
    ID_ENC_GEN, ID_ENC_PLAY, ID_ENC_STOP, ID_ENC_SAVEWAV, ID_ENC_SAVETXT, ID_ENC_TOANA, ID_ENC_MORSE, ID_ENC_INFO,
    ID_ANA_OPEN = 300, ID_ANA_PLAY, ID_ANA_STOP, ID_ANA_SAVE, ID_ANA_PATH, ID_ANA_REPORT, ID_ANA_ENV, ID_ANA_SPEC
};

const unsigned kRates[] = {8000, 11025, 22050, 44100, 48000};
const wchar_t* const kPlotClass = L"MorseAnalyserPlot";
const wchar_t* const kMainClass = L"MorseAnalyserMain";

struct App {
    HINSTANCE inst = nullptr;
    HWND hwnd = nullptr, tab = nullptr;
    HFONT font = nullptr, mono = nullptr;
    bool ready = false;

    // encoder page
    HWND e_lblText, e_text, e_lblHidden, e_hidden, e_cap, e_lblWpm, e_wpm, e_lblFreq, e_freq, e_lblRate, e_rate,
        e_lblVol, e_vol, e_gen, e_play, e_stop, e_saveWav, e_saveTxt, e_toAna, e_lblMorse, e_morse, e_info;
    // analyser page
    HWND a_open, a_play, a_stop, a_save, a_path, a_report, a_env, a_spec;

    mlib::EncodeResult enc;
    unsigned encRate = 44100;
    bool encValid = false;  // `enc` matches the current field values

    mlib::Audio loaded;
    bool haveAudio = false;
    mlib::AnalysisResult res;
    bool haveRes = false;
    std::string reportText;  // UTF-8, '\n' line ends
    std::vector<uint32_t> specPix;  // 0x00RRGGBB, specW * specH

    std::vector<uint8_t> playBuf;  // must outlive asynchronous PlaySound
} g;

// ---------------------------------------------------------------- helpers

std::wstring widen(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), &w[0], n);
    return w;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring get_text(HWND h) {
    int n = GetWindowTextLengthW(h);
    if (n <= 0) return L"";
    std::wstring w(size_t(n) + 1, L'\0');
    int got = GetWindowTextW(h, &w[0], n + 1);
    w.resize(size_t(got < 0 ? 0 : got));
    return w;
}

std::string get_utf8(HWND h) {
    std::string s = narrow(get_text(h));
    std::string o;  // Edit controls use CR LF; the encoder only needs whitespace.
    for (char c : s) o += (c == '\r') ? ' ' : c;
    return o;
}

// UTF-8 text with '\n' line ends -> an Edit control ("\r\n").
void set_text(HWND h, const std::string& utf8) {
    std::string crlf;
    for (char c : utf8) {
        if (c == '\n') crlf += '\r';
        crlf += c;
    }
    SetWindowTextW(h, widen(crlf).c_str());
}

std::string fmtn(const char* f, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}

void message(const std::string& text, UINT icon = MB_ICONERROR) {
    MessageBoxW(g.hwnd, widen(text).c_str(), L"Morse Analyser", MB_OK | icon);
}

void stop_sound() { PlaySoundW(nullptr, nullptr, 0); }

bool play_samples(const std::vector<double>& s, unsigned rate) {
    stop_sound();  // the previous buffer is about to be replaced
    g.playBuf = mlib::wav16_bytes(s, rate);
    return PlaySoundW(reinterpret_cast<LPCWSTR>(g.playBuf.data()), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT) != FALSE;
}

bool file_dialog(bool save, const wchar_t* filter, const wchar_t* defext, std::wstring& out) {
    std::vector<wchar_t> buf(32768, L'\0');
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = g.hwnd;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = buf.data();
    ofn.nMaxFile = DWORD(buf.size());
    ofn.lpstrDefExt = defext;
    ofn.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    BOOL ok = save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn);
    if (!ok) return false;
    out = buf.data();
    return true;
}

bool read_number(HWND h, const char* name, double& v) {
    std::wstring w = get_text(h);
    wchar_t* end = nullptr;
    v = std::wcstod(w.c_str(), &end);
    while (end && *end == L' ') ++end;
    if (w.empty() || !end || *end != L'\0') {
        message(std::string("\"") + name + "\" must be a number.");
        SetFocus(h);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- encoder page

void update_capacity() {
    if (!g.ready) return;
    std::string cover = get_utf8(g.e_text), hidden = narrow(get_text(g.e_hidden));
    size_t bits = mlib::hidden_capacity_bits(cover);
    size_t used = hidden.size();
    std::string s = fmtn("Hidden payload: %zu of %zu bytes used (the text carries %zu bits, 8 bits per byte)", used,
                         bits / 8, bits);
    if (used > bits / 8) s += "  -  TOO LONG: lengthen the message or shorten the hidden text";
    SetWindowTextW(g.e_cap, widen(s).c_str());
}

bool do_generate() {
    double wpm, freq, vol;
    if (!read_number(g.e_wpm, "Speed (WPM)", wpm) || !read_number(g.e_freq, "Pitch (Hz)", freq) ||
        !read_number(g.e_vol, "Volume (%)", vol))
        return false;
    int sel = int(SendMessageW(g.e_rate, CB_GETCURSEL, 0, 0));
    if (sel < 0 || sel >= int(sizeof kRates / sizeof kRates[0])) sel = 3;

    mlib::EncodeOptions o;
    o.wpm = wpm;
    o.frequency = freq;
    o.sample_rate = kRates[sel];
    o.amplitude = vol / 100.0;
    o.hidden = narrow(get_text(g.e_hidden));
    try {
        mlib::EncodeResult r = mlib::encode(get_utf8(g.e_text), o);
        g.enc = std::move(r);
        g.encRate = o.sample_rate;
        g.encValid = true;
    } catch (const std::exception& e) {
        message(e.what());
        return false;
    }
    set_text(g.e_morse, g.enc.morse);
    std::string info = fmtn("Generated %.2f s of audio, unit %.0f ms, %u Hz, %zu samples; hidden %zu bits embedded.", g.enc.duration,
                            g.enc.unit * 1000.0, g.encRate, g.enc.samples.size(), g.enc.hidden_bits);
    if (!g.enc.skipped.empty()) info += "  Skipped (no Morse code): " + g.enc.skipped;
    SetWindowTextW(g.e_info, widen(info).c_str());
    return true;
}

bool ensure_generated() { return g.encValid || do_generate(); }

void mark_dirty() {
    if (g.ready) g.encValid = false;
}

// ---------------------------------------------------------------- analyser page

void run_analysis(const std::wstring& label) {
    HCURSOR old = SetCursor(LoadCursor(nullptr, IDC_WAIT));
    g.haveRes = false;
    g.specPix.clear();
    try {
        g.res = mlib::analyse(g.loaded);
        g.haveRes = true;
        g.reportText = mlib::format_report(g.res);
        const mlib::Spectrogram& sp = g.res.spectrogram;
        g.specPix.resize(sp.pixels.size());
        for (size_t i = 0; i < sp.pixels.size(); ++i)
            g.specPix[i] = (uint32_t(sp.pixels[i].r) << 16) | (uint32_t(sp.pixels[i].g) << 8) | sp.pixels[i].b;
    } catch (const std::exception& e) {
        g.reportText = std::string("[-] Error: ") + e.what() + "\n";
    }
    SetCursor(old);
    SetWindowTextW(g.a_path, label.c_str());
    set_text(g.a_report, g.reportText);
    InvalidateRect(g.a_env, nullptr, FALSE);
    InvalidateRect(g.a_spec, nullptr, FALSE);
}

void select_tab(int index);

void load_file(const std::wstring& path) {
    try {
        g.loaded = mlib::read_wav(std::filesystem::path(path));
        g.haveAudio = true;
    } catch (const std::exception& e) {
        message(e.what());
        return;
    }
    run_analysis(path);
}

// ---------------------------------------------------------------- plots

void fill(HDC dc, const RECT& r, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

void text_at(HDC dc, int x, int y, const std::string& s) {
    std::wstring w = widen(s);
    TextOutW(dc, x, y, w.c_str(), int(w.size()));
}

void paint_placeholder(HDC dc, const RECT& rc, const char* msg) {
    text_at(dc, rc.left + 10, rc.top + 10, msg);
}

void paint_envelope(HDC dc, const RECT& rc) {
    if (!g.haveRes) {
        paint_placeholder(dc, rc, "Envelope plot: open a WAV file or send audio from the Encoder tab.");
        return;
    }
    const mlib::AnalysisResult& r = g.res;
    const int L = 40, R = 10, T = 22, B = 22;
    RECT pr = {rc.left + L, rc.top + T, rc.right - R, rc.bottom - B};
    int pw = int(pr.right - pr.left), ph = int(pr.bottom - pr.top);
    if (pw < 20 || ph < 20 || r.envelope.empty()) return;
    const double dur = double(r.envelope.size()) / r.sample_rate;

    text_at(dc, rc.left + 4, rc.top + 3, "Envelope and detected tones");
    for (const auto& t : r.tones) {
        int x0 = pr.left + int(std::get<0>(t) / dur * pw);
        int x1 = pr.left + int(std::get<1>(t) / dur * pw);
        RECT tr = {x0, pr.top, std::max(x1, x0 + 1), pr.bottom};
        fill(dc, tr, RGB(200, 220, 245));
    }
    // threshold
    HPEN thr = CreatePen(PS_DOT, 1, RGB(200, 40, 40));
    HGDIOBJ old = SelectObject(dc, thr);
    int ty = pr.bottom - int(r.threshold * ph);
    MoveToEx(dc, pr.left, ty, nullptr);
    LineTo(dc, pr.right, ty);
    SelectObject(dc, old);
    DeleteObject(thr);
    // envelope: one point per pixel column (peak of its sample range)
    std::vector<POINT> pts;
    const size_t n = r.envelope.size();
    for (int px = 0; px < pw; ++px) {
        size_t i0 = size_t(px) * n / size_t(pw), i1 = std::max(i0 + 1, size_t(px + 1) * n / size_t(pw));
        double m = 0;
        for (size_t i = i0; i < i1 && i < n; ++i) m = std::max(m, r.envelope[i]);
        POINT p;
        p.x = pr.left + px;
        p.y = pr.bottom - int(m * ph);
        pts.push_back(p);
    }
    HPEN line = CreatePen(PS_SOLID, 1, RGB(230, 120, 20));
    old = SelectObject(dc, line);
    if (pts.size() > 1) Polyline(dc, pts.data(), int(pts.size()));
    SelectObject(dc, old);
    DeleteObject(line);
    // frame + labels
    HPEN fr = CreatePen(PS_SOLID, 1, RGB(90, 90, 90));
    old = SelectObject(dc, fr);
    HGDIOBJ oldb = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, pr.left, pr.top, pr.right, pr.bottom);
    SelectObject(dc, oldb);
    SelectObject(dc, old);
    DeleteObject(fr);
    text_at(dc, rc.left + 14, pr.top - 2, "1");
    text_at(dc, rc.left + 14, pr.bottom - 14, "0");
    text_at(dc, pr.left, pr.bottom + 4, "0 s");
    text_at(dc, pr.right - 60, pr.bottom + 4, fmtn("%.2f s", dur));
}

void paint_spectrogram(HDC dc, const RECT& rc) {
    if (!g.haveRes || !g.res.has_spectrogram || g.specPix.empty()) {
        paint_placeholder(dc, rc, "Spectrogram: appears after an analysis.");
        return;
    }
    const mlib::Spectrogram& sp = g.res.spectrogram;
    const int L = 40, R = 10, T = 22, B = 18;
    RECT pr = {rc.left + L, rc.top + T, rc.right - R, rc.bottom - B};
    int pw = int(pr.right - pr.left), ph = int(pr.bottom - pr.top);
    if (pw < 20 || ph < 20) return;
    text_at(dc, rc.left + 4, rc.top + 3,
            fmtn("Spectrogram (0 - %.0f Hz, %.0f..%.0f dB)", sp.fmax, sp.lo, sp.hi));
    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = sp.width;
    bi.bmiHeader.biHeight = -sp.height;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    SetStretchBltMode(dc, HALFTONE);
    SetBrushOrgEx(dc, 0, 0, nullptr);
    StretchDIBits(dc, pr.left, pr.top, pw, ph, 0, 0, sp.width, sp.height, g.specPix.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
    text_at(dc, rc.left + 2, pr.top - 2, fmtn("%.0f", sp.fmax));
    text_at(dc, rc.left + 14, pr.bottom - 14, "0");
    text_at(dc, pr.left, pr.bottom + 2, "0 s");
    text_at(dc, pr.right - 60, pr.bottom + 2, fmtn("%.2f s", g.res.duration));
}

LRESULT CALLBACK PlotProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            int w = int(rc.right - rc.left), h = int(rc.bottom - rc.top);
            if (w > 0 && h > 0) {
                HDC mem = CreateCompatibleDC(hdc);
                HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
                HGDIOBJ oldBmp = SelectObject(mem, bmp);
                fill(mem, rc, RGB(255, 255, 255));
                SetBkMode(mem, TRANSPARENT);
                HGDIOBJ oldFont = SelectObject(mem, g.font);
                if (GetDlgCtrlID(hwnd) == ID_ANA_ENV) paint_envelope(mem, rc);
                else paint_spectrogram(mem, rc);
                SelectObject(mem, oldFont);
                BitBlt(hdc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
                SelectObject(mem, oldBmp);
                DeleteObject(bmp);
                DeleteDC(mem);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------- layout / creation

HWND make(const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD ex = 0) {
    HWND h = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, g.hwnd,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g.inst, nullptr);
    SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(g.font), TRUE);
    return h;
}

void place(HWND h, int x, int y, int w, int ht) { MoveWindow(h, x, y, w, ht, TRUE); }

void select_tab(int index) {
    (void)TabCtrl_SetCurSel(g.tab, index);
    const HWND enc[] = {g.e_lblText, g.e_text, g.e_lblHidden, g.e_hidden, g.e_cap, g.e_lblWpm, g.e_wpm, g.e_lblFreq,
                        g.e_freq, g.e_lblRate, g.e_rate, g.e_lblVol, g.e_vol, g.e_gen, g.e_play, g.e_stop,
                        g.e_saveWav, g.e_saveTxt, g.e_toAna, g.e_lblMorse, g.e_morse, g.e_info};
    const HWND ana[] = {g.a_open, g.a_play, g.a_stop, g.a_save, g.a_path, g.a_report, g.a_env, g.a_spec};
    for (HWND h : enc) ShowWindow(h, index == 0 ? SW_SHOW : SW_HIDE);
    for (HWND h : ana) ShowWindow(h, index == 1 ? SW_SHOW : SW_HIDE);
}

void layout() {
    if (!g.ready) return;
    RECT cr;
    GetClientRect(g.hwnd, &cr);
    MoveWindow(g.tab, 0, 0, int(cr.right), int(cr.bottom), TRUE);
    RECT rc = {0, 0, cr.right, cr.bottom};
    TabCtrl_AdjustRect(g.tab, FALSE, &rc);
    const int M = 10;
    int x = int(rc.left) + M, y = int(rc.top) + M, w = int(rc.right - rc.left) - 2 * M, h = int(rc.bottom - rc.top) - 2 * M;
    if (w < 100 || h < 100) return;

    // --- encoder page
    place(g.e_lblText, x, y, w, 18);
    place(g.e_text, x, y + 20, w, 90);
    y += 118;
    place(g.e_lblHidden, x, y, w, 18);
    place(g.e_hidden, x, y + 20, w, 24);
    place(g.e_cap, x, y + 48, w, 18);
    y += 76;
    int cx = x;
    place(g.e_lblWpm, cx, y + 4, 86, 18);   cx += 88;
    place(g.e_wpm, cx, y, 50, 24);          cx += 70;
    place(g.e_lblFreq, cx, y + 4, 70, 18);  cx += 72;
    place(g.e_freq, cx, y, 60, 24);         cx += 80;
    place(g.e_lblRate, cx, y + 4, 80, 18);  cx += 82;
    place(g.e_rate, cx, y, 90, 200);        cx += 110;
    place(g.e_lblVol, cx, y + 4, 70, 18);   cx += 72;
    place(g.e_vol, cx, y, 50, 24);
    y += 34;
    const int bw = 112, bg = 6;
    HWND btns[] = {g.e_gen, g.e_play, g.e_stop, g.e_saveWav, g.e_saveTxt, g.e_toAna};
    for (int i = 0; i < 6; ++i) place(btns[i], x + i * (bw + bg), y, bw, 28);
    y += 36;
    place(g.e_lblMorse, x, y, w, 18);
    int morseH = std::max(40, int(rc.top) + M + h - (y + 20) - 26);
    place(g.e_morse, x, y + 20, w, morseH);
    place(g.e_info, x, y + 20 + morseH + 4, w, 20);

    // --- analyser page
    int ax = x, ay = int(rc.top) + M;
    place(g.a_open, ax, ay, 110, 28);
    place(g.a_play, ax + 116, ay, 80, 28);
    place(g.a_stop, ax + 202, ay, 80, 28);
    place(g.a_save, ax + 288, ay, 120, 28);
    place(g.a_path, ax + 418, ay + 6, std::max(50, w - 418), 20);
    ay += 36;
    int ah = int(rc.top) + M + h - ay;
    int leftW = std::max(260, w * 42 / 100);
    place(g.a_report, ax, ay, leftW, ah);
    int rx = ax + leftW + 8, rw = w - leftW - 8, half = (ah - 8) / 2;
    place(g.a_env, rx, ay, rw, half);
    place(g.a_spec, rx, ay + half + 8, rw, ah - half - 8);
}

bool create_controls() {
    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof icc;
    icc.dwICC = ICC_TAB_CLASSES;
    InitCommonControlsEx(&icc);

    NONCLIENTMETRICSW ncm;
    ZeroMemory(&ncm, sizeof ncm);
    ncm.cbSize = sizeof ncm;
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0)) g.font = CreateFontIndirectW(&ncm.lfMessageFont);
    if (!g.font) g.font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    g.mono = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                         CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    if (!g.mono) g.mono = static_cast<HFONT>(GetStockObject(ANSI_FIXED_FONT));

    g.tab = make(WC_TABCONTROLW, L"", WS_CLIPSIBLINGS, ID_TAB);
    TCITEMW ti;
    ZeroMemory(&ti, sizeof ti);
    ti.mask = TCIF_TEXT;
    ti.pszText = const_cast<wchar_t*>(L"Encoder  (text to Morse)");
    (void)TabCtrl_InsertItem(g.tab, 0, &ti);
    ti.pszText = const_cast<wchar_t*>(L"Analyser  (audio to Morse)");
    (void)TabCtrl_InsertItem(g.tab, 1, &ti);

    const DWORD edit = ES_AUTOHSCROLL, multi = WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN;
    g.e_lblText = make(L"STATIC", L"Message to encode:", 0, -1);
    g.e_text = make(L"EDIT", L"HELLO WORLD", multi, ID_ENC_TEXT, WS_EX_CLIENTEDGE);
    g.e_lblHidden = make(L"STATIC", L"Hidden message (optional, hidden in the gap timing; plain ASCII is shown correctly by the analyser):", 0, -1);
    g.e_hidden = make(L"EDIT", L"", edit, ID_ENC_HIDDEN, WS_EX_CLIENTEDGE);
    g.e_cap = make(L"STATIC", L"", 0, ID_ENC_CAP);
    g.e_lblWpm = make(L"STATIC", L"Speed (WPM):", 0, -1);
    g.e_wpm = make(L"EDIT", L"20", edit, ID_ENC_WPM, WS_EX_CLIENTEDGE);
    g.e_lblFreq = make(L"STATIC", L"Pitch (Hz):", 0, -1);
    g.e_freq = make(L"EDIT", L"600", edit, ID_ENC_FREQ, WS_EX_CLIENTEDGE);
    g.e_lblRate = make(L"STATIC", L"Sample rate:", 0, -1);
    g.e_rate = make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, ID_ENC_RATE);
    for (unsigned r : kRates) SendMessageW(g.e_rate, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(std::to_wstring(r).c_str()));
    SendMessageW(g.e_rate, CB_SETCURSEL, 3, 0);
    g.e_lblVol = make(L"STATIC", L"Volume (%):", 0, -1);
    g.e_vol = make(L"EDIT", L"80", edit, ID_ENC_VOL, WS_EX_CLIENTEDGE);
    g.e_gen = make(L"BUTTON", L"Generate", BS_PUSHBUTTON, ID_ENC_GEN);
    g.e_play = make(L"BUTTON", L"Play", BS_PUSHBUTTON, ID_ENC_PLAY);
    g.e_stop = make(L"BUTTON", L"Stop", BS_PUSHBUTTON, ID_ENC_STOP);
    g.e_saveWav = make(L"BUTTON", L"Save WAV...", BS_PUSHBUTTON, ID_ENC_SAVEWAV);
    g.e_saveTxt = make(L"BUTTON", L"Save text...", BS_PUSHBUTTON, ID_ENC_SAVETXT);
    g.e_toAna = make(L"BUTTON", L"Analyse >>", BS_PUSHBUTTON, ID_ENC_TOANA);
    g.e_lblMorse = make(L"STATIC", L"Morse text (letters separated by a space, words by /):", 0, -1);
    g.e_morse = make(L"EDIT", L"", multi | ES_READONLY, ID_ENC_MORSE, WS_EX_CLIENTEDGE);
    g.e_info = make(L"STATIC", L"", 0, ID_ENC_INFO);

    g.a_open = make(L"BUTTON", L"Open WAV...", BS_PUSHBUTTON, ID_ANA_OPEN);
    g.a_play = make(L"BUTTON", L"Play", BS_PUSHBUTTON, ID_ANA_PLAY);
    g.a_stop = make(L"BUTTON", L"Stop", BS_PUSHBUTTON, ID_ANA_STOP);
    g.a_save = make(L"BUTTON", L"Save report...", BS_PUSHBUTTON, ID_ANA_SAVE);
    g.a_path = make(L"STATIC", L"No audio loaded - open a .wav file, drop one on this window, or use \"Analyse this\" in the Encoder tab.", SS_LEFTNOWORDWRAP, ID_ANA_PATH);
    g.a_report = make(L"EDIT", L"", multi | ES_READONLY | WS_HSCROLL | ES_AUTOHSCROLL, ID_ANA_REPORT, WS_EX_CLIENTEDGE);
    SendMessageW(g.a_report, WM_SETFONT, reinterpret_cast<WPARAM>(g.mono), TRUE);
    SendMessageW(g.e_morse, WM_SETFONT, reinterpret_cast<WPARAM>(g.mono), TRUE);
    g.a_env = make(kPlotClass, L"", WS_BORDER, ID_ANA_ENV);
    g.a_spec = make(kPlotClass, L"", WS_BORDER, ID_ANA_SPEC);

    // Remove the 30,000-character default limit of multi-line edit controls.
    SendMessageW(g.e_text, EM_SETLIMITTEXT, 0, 0);
    SendMessageW(g.e_morse, EM_SETLIMITTEXT, 0, 0);
    SendMessageW(g.a_report, EM_SETLIMITTEXT, 0, 0);
    return g.tab && g.e_text && g.a_report && g.a_env && g.a_spec;
}

// ---------------------------------------------------------------- window procedure

void on_command(int id, int code) {
    switch (id) {
        case ID_ENC_TEXT:
        case ID_ENC_HIDDEN:
            if (code == EN_CHANGE) { mark_dirty(); update_capacity(); }
            break;
        case ID_ENC_WPM:
        case ID_ENC_FREQ:
        case ID_ENC_VOL:
            if (code == EN_CHANGE) mark_dirty();
            break;
        case ID_ENC_RATE:
            if (code == CBN_SELCHANGE) mark_dirty();
            break;
        case ID_ENC_GEN:
            do_generate();
            break;
        case ID_ENC_PLAY:
            if (ensure_generated() && !play_samples(g.enc.samples, g.encRate)) message("Windows could not play this audio.");
            break;
        case ID_ENC_STOP:
        case ID_ANA_STOP:
            stop_sound();
            break;
        case ID_ENC_SAVEWAV: {
            if (!ensure_generated()) break;
            std::wstring path;
            if (!file_dialog(true, L"WAV audio (*.wav)\0*.wav\0All files (*.*)\0*.*\0", L"wav", path)) break;
            try {
                mlib::write_wav16(std::filesystem::path(path), g.enc.samples, g.encRate);
            } catch (const std::exception& e) {
                message(e.what());
            }
            break;
        }
        case ID_ENC_SAVETXT: {
            if (!ensure_generated()) break;
            std::wstring path;
            if (!file_dialog(true, L"Text file (*.txt)\0*.txt\0All files (*.*)\0*.*\0", L"txt", path)) break;
            std::ofstream f(std::filesystem::path(path), std::ios::binary);
            f << g.enc.morse << "\r\n";
            f.flush();
            if (!f) message("Could not write the file.");
            break;
        }
        case ID_ENC_TOANA: {
            if (!ensure_generated()) break;
            g.loaded.samples = g.enc.samples;
            g.loaded.sample_rate = g.encRate;
            g.haveAudio = true;
            select_tab(1);
            layout();
            run_analysis(L"(audio generated in the Encoder tab)");
            break;
        }
        case ID_ANA_OPEN: {
            std::wstring path;
            if (file_dialog(false, L"WAV audio (*.wav)\0*.wav\0All files (*.*)\0*.*\0", L"wav", path)) load_file(path);
            break;
        }
        case ID_ANA_PLAY:
            if (!g.haveAudio) message("Nothing to play yet - open a WAV file first.", MB_ICONINFORMATION);
            else if (!play_samples(g.loaded.samples, g.loaded.sample_rate)) message("Windows could not play this audio.");
            break;
        case ID_ANA_SAVE: {
            if (g.reportText.empty()) { message("Nothing to save yet - analyse some audio first.", MB_ICONINFORMATION); break; }
            std::wstring path;
            if (!file_dialog(true, L"Text file (*.txt)\0*.txt\0All files (*.*)\0*.*\0", L"txt", path)) break;
            std::ofstream f(std::filesystem::path(path), std::ios::binary);
            for (char c : g.reportText) {
                if (c == '\n') f.put('\r');
                f.put(c);
            }
            f.flush();
            if (!f) message("Could not write the file.");
            break;
        }
    }
}

LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE:
            g.hwnd = hwnd;
            if (!create_controls()) return -1;
            g.ready = true;
            select_tab(0);
            layout();
            update_capacity();
            DragAcceptFiles(hwnd, TRUE);
            return 0;
        case WM_SIZE:
            layout();
            return 0;
        case WM_GETMINMAXINFO: {
            MINMAXINFO* mm = reinterpret_cast<MINMAXINFO*>(lp);
            mm->ptMinTrackSize.x = 800;
            mm->ptMinTrackSize.y = 620;
            return 0;
        }
        case WM_CTLCOLORSTATIC: {
            // Labels and read-only edits blend into the (white) themed tab page.
            HDC sdc = reinterpret_cast<HDC>(wp);
            SetBkColor(sdc, GetSysColor(COLOR_WINDOW));
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
        }
        case WM_COMMAND:
            on_command(int(LOWORD(wp)), int(HIWORD(wp)));
            return 0;
        case WM_NOTIFY: {
            NMHDR* nm = reinterpret_cast<NMHDR*>(lp);
            if (nm->idFrom == ID_TAB && nm->code == UINT(TCN_SELCHANGE)) {
                select_tab(TabCtrl_GetCurSel(g.tab));
                layout();
            }
            return 0;
        }
        case WM_DROPFILES: {
            HDROP drop = reinterpret_cast<HDROP>(wp);
            UINT n = DragQueryFileW(drop, 0, nullptr, 0);
            std::vector<wchar_t> buf(size_t(n) + 1, L'\0');
            DragQueryFileW(drop, 0, buf.data(), n + 1);
            DragFinish(drop);
            select_tab(1);
            layout();
            load_file(buf.data());
            return 0;
        }
        case WM_DESTROY:
            stop_sound();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int show) {
    g.inst = inst;
    WNDCLASSEXW pc;
    ZeroMemory(&pc, sizeof pc);
    pc.cbSize = sizeof pc;
    pc.style = CS_HREDRAW | CS_VREDRAW;
    pc.lpfnWndProc = PlotProc;
    pc.hInstance = inst;
    pc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    pc.lpszClassName = kPlotClass;
    WNDCLASSEXW mc;
    ZeroMemory(&mc, sizeof mc);
    mc.cbSize = sizeof mc;
    mc.style = CS_HREDRAW | CS_VREDRAW;
    mc.lpfnWndProc = MainProc;
    mc.hInstance = inst;
    mc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    mc.hbrBackground = reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(COLOR_WINDOW + 1));
    mc.lpszClassName = kMainClass;
    if (!RegisterClassExW(&pc) || !RegisterClassExW(&mc)) return 1;

    HWND w = CreateWindowExW(0, kMainClass, L"Morse Analyser", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT,
                             CW_USEDEFAULT, 1100, 740, nullptr, nullptr, inst, nullptr);
    if (!w) return 1;
    ShowWindow(w, show);
    UpdateWindow(w);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return int(m.wParam);
}
