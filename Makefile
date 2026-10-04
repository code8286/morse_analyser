# Morse Analyser - build with GNU make and any C++17 compiler (g++/clang++/MinGW).
#   make        analyser + encoder CLIs (+ the GUI when building on Windows)
#   make gui    Windows GUI only (needs MinGW-w64 / MSYS2)
#   make test   build and run all tests
CXX      ?= g++
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra
BUILD    := build
IS_WIN   := $(findstring Windows,$(OS))
EXE      := $(if $(IS_WIN),.exe,)
ANALYSER := $(BUILD)/morse-analyser$(EXE)
ENCODER  := $(BUILD)/morse-encoder$(EXE)
GUI      := $(BUILD)/morse-gui$(EXE)
GEN      := $(BUILD)/gen_test_wav$(EXE)
RT       := $(BUILD)/test_roundtrip$(EXE)
GUIRES   := $(BUILD)/morse-gui-res.o
WINDRES  ?= windres
HEADERS  := $(wildcard morse_lib/*.hpp)
PREFIX   ?= /usr/local

.PHONY: all gui test install clean
all: $(ANALYSER) $(ENCODER) $(if $(IS_WIN),$(GUI),)
gui: $(GUI)

$(BUILD):
	mkdir -p $(BUILD)

$(ANALYSER): morse-analyser.cpp $(HEADERS) | $(BUILD)
	$(CXX) $(CXXFLAGS) $< -o $@

$(ENCODER): morse-encoder.cpp $(HEADERS) | $(BUILD)
	$(CXX) $(CXXFLAGS) $< -o $@

$(GEN): tests/gen_test_wav.cpp $(HEADERS) | $(BUILD)
	$(CXX) $(CXXFLAGS) $< -o $@

$(RT): tests/test_roundtrip.cpp $(HEADERS) | $(BUILD)
	$(CXX) $(CXXFLAGS) $< -o $@

# GUI resource: embeds the manifest (visual styles). Skip with `make gui NO_MANIFEST=1`.
$(GUIRES): gui/morse-gui.rc gui/morse-gui.manifest | $(BUILD)
	cd gui && $(WINDRES) morse-gui.rc -O coff -o ../$@

$(GUI): gui/morse-gui.cpp $(HEADERS) $(if $(NO_MANIFEST),,$(GUIRES)) | $(BUILD)
	$(CXX) $(CXXFLAGS) -mwindows -static $< $(if $(NO_MANIFEST),,$(GUIRES)) -o $@ \
	  -lcomctl32 -lcomdlg32 -lwinmm -lshell32 -lgdi32 -luser32

# Tests: encoder unit tests + encoder->analyser round trips, a smoke test of the CLIs,
# and the original synthesised "SOS HI" check.
test: $(ANALYSER) $(ENCODER) $(GEN) $(RT)
	cd $(BUILD) && ./test_roundtrip$(EXE)
	$(ENCODER) "THE QUICK BROWN FOX" -o $(BUILD)/stego.wav --hidden "Hi!!"
	$(ANALYSER) $(BUILD)/stego.wav --outdir $(BUILD) | tee $(BUILD)/stego.log
	@grep -q '=> THE QUICK BROWN FOX' $(BUILD)/stego.log && grep -q 'Hidden carrier text: "Hi!!"' $(BUILD)/stego.log \
	  || (echo "CLI ROUND TRIP FAILED"; exit 1)
	$(GEN) $(BUILD)/test.wav "SOS HI"
	$(ANALYSER) $(BUILD)/test.wav --outdir $(BUILD) | tee $(BUILD)/test.log
	@grep -q "=> SOS HI" $(BUILD)/test.log && test -s $(BUILD)/morse_timings.csv \
	  && test -s $(BUILD)/spectrogram.png && echo "TEST PASSED" || (echo "TEST FAILED"; exit 1)

install: $(ANALYSER) $(ENCODER)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(ANALYSER) $(DESTDIR)$(PREFIX)/bin/morse-analyser
	install -m 755 $(ENCODER) $(DESTDIR)$(PREFIX)/bin/morse-encoder
	$(if $(IS_WIN),install -m 755 $(GUI) $(DESTDIR)$(PREFIX)/bin/morse-gui$(EXE),)

clean:
	rm -rf $(BUILD)
