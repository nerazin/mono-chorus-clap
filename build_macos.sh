#!/bin/sh
set -eu
cd "$(dirname "$0")"
bundle="build/Mono Chorus.clap"
mkdir -p "$bundle/Contents/MacOS"
cp Info.plist "$bundle/Contents/Info.plist"
xcrun clang -std=c11 -O2 -Wall -Wextra -Wpedantic -fvisibility=hidden \
  -mmacosx-version-min=11.0 -arch arm64 -arch x86_64 -bundle \
  -Wl,-exported_symbol,_clap_entry -Ivendor/clap/include \
  source/mono_chorus.c -o "$bundle/Contents/MacOS/MonoChorus"
codesign --force --sign - --timestamp=none "$bundle"
printf 'Built: %s\n' "$bundle"
