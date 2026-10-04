# Morse Analyser

A self-contained C++17 toolkit (no third-party dependencies) for Morse code in both
directions:

* **Encoder**: turns text into Morse text and Morse audio (WAV), and can *hide a secret
  message* in the timing of the gaps inside letters.
* **Analyser**: decomposes a Morse audio file. It decodes the visible dots and dashes,
  and it reads hidden (steganographic) data carried in the gap timing.
* **GUI** (Windows): one window joining both. Type a message, hear it, send it straight
  to the analyser, or open any WAV file and inspect it.

The analyser is a C++ port of the original Python script `morse_stego_analyzer.py`
(kept in `origin/`). All numerical code (WAV I/O, FFT, Hilbert transform, Butterworth
filter, STFT, k-means, PNG/SVG output) is implemented in this repository under
`morse_lib/`; the GUI is plain Win32.

## Programs

| Program | What it is | Platforms |
|---|---|---|
| `morse-gui` | GUI with Encoder and Analyser tabs | Windows |
| `morse-encoder` | command-line text to Morse text + WAV | any |
| `morse-analyser` | command-line audio analysis | any |

## Requirements

* A C++17 compiler (g++ 9+, clang++ 10+, or MSVC 2019+). The GUI needs MinGW-w64 /
  MSYS2 or MSVC on Windows.
* Optional: GNU make or CMake 3.10+.
* Analyser input: uncompressed **`.wav`** only (8/16/24/32-bit PCM, 32/64-bit float;
  multi-channel is mixed down to mono). MP3/FLAC are not supported.

## Build

```sh
# Makefile (Linux, macOS, MSYS2/MinGW). On Windows this also builds the GUI.
make              # builds into build/
make gui          # GUI only (MinGW-w64; add NO_MANIFEST=1 if windres is unavailable)
make test         # builds and runs all tests
sudo make install # optional (PREFIX=... to change)

# CMake (any platform, incl. Visual Studio; the GUI target exists on Windows only)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release

# Plain compiler, no build system
g++ -O2 -std=c++17 morse-analyser.cpp -o morse-analyser
g++ -O2 -std=c++17 morse-encoder.cpp  -o morse-encoder
g++ -O2 -std=c++17 -mwindows -static gui/morse-gui.cpp -o morse-gui \
    -lcomctl32 -lcomdlg32 -lwinmm -lshell32 -lgdi32 -luser32     # Windows only
```

## The GUI

Start `morse-gui.exe`.

**Encoder tab.** Type a message; set speed (WPM), pitch (Hz), sample rate and volume;
optionally type a hidden message. The line under the hidden-message box shows how many
bytes the text can carry. *Generate* produces the Morse text and the audio; *Play* /
*Stop* use the sound card; *Save WAV...* and *Save text...* write files; *Analyse >>*
sends the audio straight to the Analyser tab. Changing any field regenerates the audio
automatically the next time you play, save or analyse.

**Analyser tab.** *Open WAV...* (or drop a file on the window) analyses a file. The left
pane is the text report (visible Morse and decoded text, gap clusters, hidden bits,
hidden carrier text); on the right are the envelope plot with detected tones and the
spectrogram (0 to 20 kHz, for spotting out-of-band content). *Play* plays the loaded
audio and *Save report...* writes the report to a text file.

## Command-line encoder

```sh
morse-encoder "HELLO WORLD" -o hello.wav --wpm 18 --freq 700
morse-encoder "THE QUICK BROWN FOX" -o secret.wav --hidden "Hi!!"
morse-encoder "SOS" --text-only
```

| Option | Default | Meaning |
|---|---|---|
| `TEXT` | (required) | message; letters, digits and punctuation with a Morse code; case-insensitive; other characters are skipped and reported |
| `-o`, `--out FILE` | `morse.wav` | output WAV (16-bit mono PCM) |
| `--text-only` | off | print the Morse text, write no audio |
| `--wpm N` | `20` | speed, 1 to 100 (unit time = 1.2 / N seconds) |
| `--freq HZ` | `600` | tone pitch, 20 Hz to 45% of the sample rate |
| `--rate HZ` | `44100` | sample rate, 8000 to 192000 |
| `--ramp MS` | `5` | raised-cosine attack/release (avoids key clicks) |
| `--amp A` | `0.8` | amplitude, above 0 up to 1 |
| `--lead S`, `--tail S` | `0.3` | silence before and after |
| `--hidden MSG` | none | hide MSG in the gap timing (see below) |

Errors (bad numbers, a hidden message that does not fit) go to stderr with exit code 1.

### How the hidden message is stored

Every gap between two elements of the *same letter* is a carrier. A normal gap is 1
unit long and means bit `0`; a stretched gap of 1.6 units means bit `1`. Letter gaps
(3 units) and word gaps (7 units) are never touched, and 1.6 is below the 2-unit
threshold at which a gap counts as a letter break, so the visible Morse still decodes.
Bytes are sent MSB first, one bit per carrier gap, and unused carriers stay `0`. A
message therefore needs 8 carrier gaps per byte: "THE QUICK BROWN FOX" carries 4 bytes.
The GUI shows the capacity as you type.

## Command-line analyser

```sh
morse-analyser [audio.wav] [options]
```

| Option | Default | Meaning |
|---|---|---|
| `audio.wav` | `morse_stego.wav` | input file |
| `--csv FILE` | `morse_timings.csv` | per-gap timings and decoded bits |
| `--outdir DIR` | `.` | directory for CSV / plot output (must exist) |
| `--cutoff HZ` | `20` | envelope low-pass cutoff |
| `--min-tone SEC` | `0.005` | ignore bursts shorter than this |
| `--no-plots` | off | skip SVG / PNG generation |
| `-h`, `--help` | | show usage |

Exit code is `0` on success, `1` on error (message on stderr).

### Outputs

* **stdout**: sample rate, duration, threshold, tone count, visible Morse text, gap
  clusters, unit time, the original "hidden bits" and ASCII candidates, and the
  *hidden carrier* section (bits and decoded text, or a note that none was found).
* `morse_timings.csv`: columns `gap_seconds,bit`.
* `tone_detection.svg`: envelope with detected tone bursts shaded.
* `spectrogram.png`: spectrogram, 0 to min(20 kHz, Nyquist).

## How the analysis works

```
WAV -> normalise -> Hilbert envelope -> 20 Hz Butterworth low-pass (zero-phase)
    -> threshold (20th/95th percentile) -> tone bursts -> gaps
    -> visible Morse (2-means dot/dash split, gap classes)
    -> 3-means gap clustering -> unit time -> gap-modulation bits -> ASCII
    -> carrier-gap decoder -> CSV + envelope plot + spectrogram
```

Two hidden-data decoders run side by side:

* **Original heuristic** (from the Python script): with `unit` the lowest gap cluster
  centre, a gap longer than `unit * 1.25` is bit `1`, anything else `0`. It treats every
  gap as a carrier, so ordinary letter and word gaps show up as `1` bits.
* **Carrier-gap decoder** (matches the encoder): only gaps classified as inside a
  letter are examined. The threshold is the midpoint of the two 2-means centres, or
  1.2 dots when only one kind is present. It reports text only when at least one gap was
  stretched.

## Repository layout

```
morse-analyser.cpp     command-line analyser (front end)
morse-encoder.cpp      command-line encoder (front end)
gui/
  morse-gui.cpp        Win32 GUI
  morse-gui.rc, .manifest   embeds the visual-styles manifest
morse_lib/
  wav.hpp              WAV reader/writer (also in-memory WAV for playback)
  dsp.hpp              FFT, Hilbert, Butterworth, filtfilt, STFT
  stats.hpp            percentile, 1-D k-means
  image.hpp            PNG writer, SVG envelope plot
  morse.hpp            Morse table, text<->Morse, timing decoder
  encoder.hpp          text -> Morse text + audio, hidden-bit embedding
  analysis.hpp         the whole analysis pipeline as a library (CLI and GUI share it)
tests/
  test_roundtrip.cpp   encoder unit tests + encoder-to-analyser round trips
  gen_test_wav.cpp     synthesises a clean "SOS HI" test WAV
origin/                original Python script
Makefile, CMakeLists.txt
```

## Testing

`make test` (or `ctest`) runs:

* `test_roundtrip`: encoder unit tests (PARIS timing, capacity, option validation, only
  carrier gaps change) and encoder-to-analyser round trips over speeds 10 to 40 WPM,
  pitches 400 and 1000 Hz, sample rates 8000, 22050 and 48000 Hz, light noise, dash-only
  and dot-only text, and no-hidden-data files. Each checks the visible text and the
  recovered hidden bits.
* a command-line round trip (`morse-encoder --hidden` into `morse-analyser`);
* the original synthesised "SOS HI" smoke test.

The DSP routines were also cross-checked against scipy: filter coefficients agree to
~1e-9, STFT values to ~1e-8, and the envelope to ~4e-6 away from the file edges. The
analyser's original output is unchanged by the library refactor; the carrier section is
only added.

## Known limitations

* **GUI status:** the GUI was written against the Win32 API and syntax-checked against
  stand-in headers on Linux. It has not yet been run on a Windows machine, so expect to
  fix small things on first use and please report them. All the logic behind it
  (encoder, analyser, WAV, plots data) is covered by the tests above.
* The GUI analyses on the UI thread, so a very long recording briefly freezes the window.
  The window is not DPI-aware (Windows scales it on high-DPI screens).
* The original gap-bit heuristic assumes hidden data is carried by lengthened gaps;
  ordinary letter/word gaps are also longer than the unit, so on normal Morse its
  "hidden bits" are noise. The carrier-gap decoder only understands this project's
  encoder scheme.
* Visible-Morse decoding splits dots from dashes by tone length. If every tone has the
  same length and no 1-unit gap exists (for example the text "TT"), dashes cannot be
  told from dots and are read as dots.
* The Hilbert transform is zero-padded to a power-of-two length, so the envelope can
  differ slightly from scipy within the first/last samples.
* The whole file is held in memory (about 8 bytes per sample, several copies during
  analysis); very long recordings need proportionally more RAM.
* Characters without a Morse code (and non-ASCII text) are skipped by the encoder.

## Changelog

Newest first. Update this section with every major change.

* **2026-10-02**: Added the **encoder** (`morse_lib/encoder.hpp`, `morse-encoder`) with
  hidden-message embedding in letter-internal gap timing; the **Win32 GUI**
  (`gui/morse-gui.cpp`: Encoder and Analyser tabs, playback, WAV/text/report saving,
  envelope plot, spectrogram, drag-and-drop); the **carrier-gap decoder** in the
  analyser; the analysis pipeline moved into `morse_lib/analysis.hpp` (CLI output
  unchanged apart from the new section); in-memory WAV output and Unicode file paths in
  `wav.hpp`; dash-only text now decodes in the visible-Morse step; round-trip tests;
  Makefile and CMake updated.
* **2026-10-02**: Added MIT `LICENSE` (copyright code8286).
* **2026-10-02**: Added README, Makefile, CMake build, `.gitignore`, and a smoke test
  with a test-WAV generator. Fixed a spurious GCC warning in `filtfilt` (now throws on
  inputs shorter than 2 samples).
* **2026-10-02**: Initial C++ port of `morse_stego_analyzer.py` with the self-written
  `morse_lib`; added visible-Morse text decoding, SVG/PNG plot output, and robustness
  for files that start mid-tone.

## License

Released under the [MIT License](LICENSE).
