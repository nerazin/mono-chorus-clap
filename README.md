# Mono Chorus CLAP

Mono Chorus is a stereo CLAP effect inspired by the Elektron Monomachine's
FX-CHORS. Version **0.4.0** combines the 0.3.0 plug-in interface with a native
fixed-point chorus core supplied by a friend of the project owner. It does not
load firmware or run an emulator.

The plug-in exposes eight automatable and modulatable controls in Bitwig's
parameter interface: DEL, DEP, SPD, MIX, FB, WID, LP and INP. It has no
separate graphical editor.

## Build on macOS

You need macOS 11 or later on Apple Silicon or Intel and Apple's Xcode Command
Line Tools. Install the tools if needed:

```sh
xcode-select --install
```

Clone the repository and build the plug-in:

```sh
git clone https://github.com/nerazin/mono-chorus-clap.git
cd mono-chorus-clap
./build_macos.sh
```

The script compiles the C and C++ sources for both arm64 and x86_64, creates
a CLAP bundle and ad-hoc signs it. The resulting bundle is:

```text
build/Mono Chorus.clap
```

The CLAP headers and DSP source are included in this repository. Once the
Apple tools are installed and the repository is cloned, the build does not
need firmware, an emulator, JUCE or any additional downloads. The build script
has not been run on macOS in the development environment; the included source
and wrapper were tested on Linux.

## Install in Bitwig

Quit Bitwig before installing. If you want to keep 0.3.0, move its existing
bundle somewhere **outside** the plug-in folders that Bitwig scans. Then copy
the whole new bundle into your CLAP folder:

```sh
mkdir -p "$HOME/Library/Audio/Plug-Ins/CLAP"
ditto "build/Mono Chorus.clap" "$HOME/Library/Audio/Plug-Ins/CLAP/Mono Chorus.clap"
```

Reopen Bitwig, rescan plug-ins in **Settings → Locations** if necessary, and
add **Mono Chorus** after an audio source. Version 0.4.0 keeps the plug-in ID,
parameter IDs and saved-state format of 0.3.0, so existing projects should
load their parameter settings. The DSP behaviour and latency have changed;
back up important projects before replacing the previous version. Avoid
placing two versions with the same ID in scanned folders.

The build is ad-hoc signed, not Apple-notarised. If macOS specifically blocks
the bundle, remove its quarantine attribute:

```sh
xattr -dr com.apple.quarantine "$HOME/Library/Audio/Plug-Ins/CLAP/Mono Chorus.clap"
```

If it still does not load, run `Diagnose_Mono_Chorus.sh` in Terminal. It reads
system and plug-in loading information; it does not modify files or send
data.

## Controls

| Control | Function |
| --- | --- |
| DEL | Centre delay time |
| DEP | Delay modulation depth |
| SPD | Modulation speed |
| MIX | Dry/wet balance |
| FB | Negative feedback amount |
| WID | Left/right modulation phase difference |
| LP | Low-pass filter in the feedback path |
| INP | Input gain before the effect |

All controls range from 0 to 127. Changes are processed at the native DSP's
16-frame boundaries with a model of the original host's control smoothing.
As in the measured firmware, `MIX 127` still leaves a small dry component.

For a metallic tail on a short snare or click, try:

```text
DEL 55   DEP 80   SPD 0     MIX 127
FB 127   WID 80   LP 127    INP 64
```

Start at a comfortable listening level: high feedback can ring for a long
time. Reduce FB to shorten the tail, lower LP to darken it, or raise SPD to
make its resonances move.

## Sample rate, latency and accuracy

The fixed-point core runs at 44.1 kHz. At other host rates the plug-in uses
band-limited sample-rate conversion in both directions; supported host rates
are 8–384 kHz. At host rates above 44.1 kHz, conversion also limits the dry
path's bandwidth. The plug-in reports its latency to the host:

| Host rate | Reported latency |
| --- | ---: |
| 44.1 kHz | 15 samples, about 0.34 ms |
| 48 kHz | 107 samples, about 2.23 ms |
| 96 kHz | 212 samples, about 2.21 ms |
| 192 kHz | 421 samples, about 2.19 ms |

The effect runs a short period of silence at startup and reset, so it is
ready to process audio rather than exposing the firmware routine's initial
wet-output gate. Modulation phase can therefore differ from an instrument
that has been running for a different length of time.

In offline 44.1 kHz tests, after latency alignment and matching initial state,
the CLAP build matched the isolated firmware chorus output sample for sample
in 68 fixed/long-tail cases and nine control-change cases. The reference was
Monomachine OS 1.32B running in a DSP emulator, not a recording from physical
hardware. These tests do not prove a match for every setting, sample rate or
complete hardware signal chain. At other rates, conversion was checked for
continuity, level and buffer-size invariance.

The files in [`Comparison/`](Comparison/) record earlier **0.3.0** results;
they are retained for historical comparison and do not measure 0.4.0.

## Source and tests

- `source/mono_chorus.c` — CLAP wrapper and controls, derived from 0.3.0.
- `source/chorus_engine.cpp` and `.h` — native-core adapter, control
  smoothing, buffering and sample-rate conversion.
- `source/friend/` — the supplied fixed-point chorus core and lookup tables,
  included unchanged. Its optional tuned-delay and harmonicity extensions
  are disabled in this plug-in.
- `vendor/clap/` — CLAP headers and their licence.
- `tests/` — offline CLAP and streaming regression tests; see
  [`tests/README.md`](tests/README.md).

This independent project is not affiliated with or endorsed by Elektron or
Bitwig.
