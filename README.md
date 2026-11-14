# Faust — Luca Spanedda's version

Fork of [grame-cncm/faust](https://github.com/grame-cncm/faust) (branch `master-dev`) with a few personal changes.
The original README is in the upstream repository.

## Changes

- **Block diagrams** (`compiler/draw/device/SVGDev.cpp`): custom SVG style for `faust -svg`.
- **Qt GUI** (`architecture/faust/gui/QTUI.h`, `Styles/Grey.qss`, `Styles/GreyDark.qss`, `Styles/Grey.qrc`),
  used by `faust2caqt` / `faust2jaqt`:
  - light and dark theme; the theme follows the macOS appearance and can be changed with the
    round button in the top right corner of the window (the choice is saved per application);
  - flat, compact widgets: more controls fit in a window, the window opens as large as its
    content (largest tab page included) and scrolls only if it is larger than the screen;
  - boxes around single controls are frameless, labels made only of `_` (e.g. `" _ "`) are hidden,
    groups from the third nesting level on are shown without frame;
  - bargraph values with 5 significant digits (6 decimals for huge ranges such as
    `ma.MIN..ma.MAX`, used with `[style:numerical]`), GUI refresh at 25 fps;
  - dB bargraphs (`[unit:dB]`) in the colors of the theme: a thin bar in the signal grey, red only
    over 0 dB, a faint line at 0 dB, the scale beside the bar with marks chosen by importance
    (0, -20, -40, ...) and drawn only where they do not overlap, so wide ranges (-120..+20) stay
    readable (the green / yellow / orange steps of the original are gone);
  - colors of hand-painted widgets: `THEME` block at the top of `QTUI.h`.
- **`faust2caqt`** (`tools/faust2appls/faust2caqt`):
  - temporary build folder in `$TMPDIR`, always removed (no more `faust.XXXXXX` leftovers);
  - signing works in folders synced by iCloud (Desktop, Documents);
  - unused Qt plugins are not bundled, `macdeployqt` rpath noise is hidden;
  - macOS 14 deployment target (same as Homebrew Qt);
  - `-preset auto` saves presets in `~/Documents/FaustPresets` instead of `/var/tmp`;
  - new option `-theme system|light|dark` (initial GUI theme).

Usual build: `faust2caqt -double -midi -resample -preset auto file.dsp`

- **`faust2caqt-meters`** (`tools/faust2appls/faust2caqt-meters`, `architecture/ca-qt-meters.cpp`,
  `architecture/faust/gui/QTMeters.h`): same as `faust2caqt` (same options), plus an analysis strip
  above the controls, in the same window, modelled on MiniMeters, drawn by the GPU at 60 fps with
  the colors of the light/dark theme:
  - the divider below the strip resizes it, closes it (drag up) and reopens it (drag down); when
    closed the analysis stops and the application costs as much as `faust2caqt`;
  - four resizable columns: waveform | levels + stereometer | oscilloscope | spectrum; drag a
    border to resize (double click: default proportions); the borders are shown only while the
    mouse is over the strip;
  - waveform: L and R (or Mid and Side) in two lanes, min/max envelope scrolling smoothly with
    the wall clock (fractional pixel steps), speed slow/normal/fast;
  - oscilloscope: full height, L and R overlaid, Follow Pitch, 1 or 4 cycles, 1:1 or Zoom on quiet
    signals, optional persistence and glow;
  - levels (peak, RMS Fast/Slow, LUFS Momentary/Short-Term/Integrated with BS.1770 gating,
    maximum peak in the tooltip);
  - stereometer with L/R/mono axes, phosphor fading and correlation, or phase view (chip
    Stereo | Phase): Takens delay embedding of one signal (L, R or Mid), x(t) against
    x(t + tau), so a periodic signal draws a closed curve and a chaotic one its attractor; tau
    automatic (first zero of the autocorrelation, or where it falls below 1/e; smoothed) or
    fixed (0.1-10 ms); same cost as the stereometer;
  - spectrum (two curves, dotted peak hold, readout of the loudest frequency with note and cents;
    FFT size follows the sample rate) or spectrogram;
  - everything shows two signals, L and R: a routing matrix (gear button, top left; the routing
    is in its tooltip) sends any input or output of the
    DSP to L and/or R; several channels on a side are averaged;
  - mode chips: moving the mouse over a module shows its main modes as small buttons; a click on
    the level bars (or their readout) selects the next level mode; right click: all the options;
  - status bar below the controls: one bar per CPU core, CPU of the whole Mac and of the
    application (same scale), DSP load (time in the DSP compute / block duration, average and
    worst) and late blocks, block size measured in the audio callback, sample rate, number of
    inputs and outputs of the DSP, output device and latency; updated once per second;
  - when the window is smaller than the interface, the controls are scaled down proportionally;
  - routing and options are saved per application.

  The audio thread only copies the buffers (lock-free ring): the DSP is unchanged.
  Usual build: `faust2caqt-meters -double -midi -resample -preset auto file.dsp`

## Install (macOS)

```bash
sudo git clone https://github.com/LucaSpanedda/faust.git /Applications/faust
sudo chown -R $(whoami) /Applications/faust
cd /Applications/faust
git submodule update --init --recursive
make
sudo make install
```

`make install` copies everything to `/usr/local` (`bin`, `include/faust`, `share/faust`, `lib`).
Edit the files here, then `make && sudo make install`: changes made directly in `/usr/local`
are overwritten by the next install.

## Update from Grame

```bash
cd /Applications/faust
git remote add upstream https://github.com/grame-cncm/faust.git   # only once
git fetch upstream
git merge upstream/master-dev
make && sudo make install
git push origin master-dev
```
