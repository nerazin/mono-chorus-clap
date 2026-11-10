# Mono Chorus CLAP

A small stereo CLAP chorus effect inspired by Elektron Monomachine's FX-CHORS. This repository contains version **0.3.0** of the floating-point reconstruction. The plug-in and DSP live in one C source file, `source/mono_chorus.c`. It does not contain or load firmware and does not run an emulator.

It exposes eight automatable and modulatable parameters in the host's own interface: DEL, DEP, SPD, MIX, FB, WID, LP and INP. It has no separate graphical editor.

## Build on macOS

### Requirements

- macOS 11 or newer on Apple Silicon or Intel.
- Apple's Xcode Command Line Tools, including `clang`, `xcrun` and `codesign`.
- The CLAP headers in `vendor/clap/include` (included in this repository).

Install the Command Line Tools if you don't already have them:

```sh
xcode-select --install
```

Then clone the repository and build:

```sh
git clone https://github.com/nerazin/mono-chorus-clap.git
cd mono-chorus-clap
./build_macos.sh
```

The script builds both arm64 and x86_64 slices, creates a CLAP bundle, and ad-hoc signs it. The result is:

```text
build/Mono Chorus.clap
```

To install, quit Bitwig Studio, create `~/Library/Audio/Plug-Ins/CLAP/` if needed, and copy the **whole** `Mono Chorus.clap` bundle into that folder. Move any previous bundle with the same name out of the folder first. Reopen Bitwig and rescan plug-ins if necessary.

```sh
mkdir -p "$HOME/Library/Audio/Plug-Ins/CLAP"
ditto "build/Mono Chorus.clap" "$HOME/Library/Audio/Plug-Ins/CLAP/Mono Chorus.clap"
```

An ad-hoc signature is not an Apple notarization. If macOS specifically blocks a downloaded bundle, the quarantine attribute can be removed for that bundle with:

```sh
xattr -dr com.apple.quarantine "$HOME/Library/Audio/Plug-Ins/CLAP/Mono Chorus.clap"
```

The build script has not been run on macOS in this development environment; it uses standard Xcode Command Line Tools. A Linux CLAP build and the original host tests were run to check the source.

## Controls

| Control | Function |
| --- | --- |
| DEL | Centre delay time |
| DEP | Delay modulation depth |
| SPD | Modulation speed |
| MIX | Dry/wet balance |
| FB | Negative feedback amount |
| WID | Relative right-channel modulation phase |
| LP | Low-pass filter in the feedback loop |
| INP | Input gain and saturation |

All controls use a 0–127 range. Integer values correspond to the original device's 7-bit controls; fractional values may be used for host automation.

For a metallic tail after a short click or snare, try `DEL 55, DEP 80, SPD 0, MIX 127, FB 127, WID 80, LP 127, INP 64`. Start at a comfortable listening level: high feedback may ring for a long time.

## Signal flow and accuracy

Three modulated, linearly interpolated delay taps per channel are combined and fed through a one-pole low-pass and a negative-feedback loop. Width rotates the right-channel modulation. High feedback strengthens short-delay resonances, creating metallic, beating tails.

This is a floating-point reconstruction with some fixed-point-style rounding, not a bit-perfect emulation. The 0.3.0 algorithm was compared at 44.1 kHz against the isolated chorus output of the original firmware running in a separate offline test harness. Twenty-five fixed-setting stereo noise/tail tests scored 99.82%–99.99% using `100 × (1 − RMS(reference − output) / RMS(reference))`. These are waveform-error scores for the tested material, not a universal perceptual accuracy guarantee. A four-second decay test scored 99.83%.

The reference was not recorded from a physical Monomachine. Other sample rates, every parameter combination, automated parameter changes, track processing, converters and analogue outputs have not been established by those scores. See [Comparison/fixed_settings_results.csv](Comparison/fixed_settings_results.csv) for the 25 test settings.

## Project layout

- `source/mono_chorus.c` — plug-in and DSP implementation.
- `vendor/clap/include` — CLAP headers.
- `vendor/clap/LICENSE` — licence for the included CLAP headers.
- `build_macos.sh`, `Info.plist` — macOS build and bundle metadata.
- `Comparison/fixed_settings_results.csv` — fixed-setting comparison results.

This project is independent and is not affiliated with or endorsed by Elektron.
