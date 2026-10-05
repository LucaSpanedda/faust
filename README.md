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
  - colors of hand-painted widgets: `THEME` block at the top of `QTUI.h`.
- **`faust2caqt`** (`tools/faust2appls/faust2caqt`):
  - temporary build folder in `$TMPDIR`, always removed (no more `faust.XXXXXX` leftovers);
  - signing works in folders synced by iCloud (Desktop, Documents);
  - unused Qt plugins are not bundled, `macdeployqt` rpath noise is hidden;
  - macOS 14 deployment target (same as Homebrew Qt);
  - `-preset auto` saves presets in `~/Documents/FaustPresets` instead of `/var/tmp`;
  - new option `-theme system|light|dark` (initial GUI theme).

Usual build: `faust2caqt -double -midi -resample -preset auto file.dsp`

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
