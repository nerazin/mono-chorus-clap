#!/bin/sh
set -eu
cd "$(dirname "$0")"

bundle="build/Mono Chorus.clap"
objects="build/objects"
mkdir -p "$bundle/Contents/MacOS" "$objects"
cp Info.plist "$bundle/Contents/Info.plist"

common="-O2 -fvisibility=hidden -mmacosx-version-min=11.0 -arch arm64 -arch x86_64"
xcrun clang -std=c11 $common -Wall -Wextra -Wpedantic -Ivendor/clap/include \
  -c source/mono_chorus.c -o "$objects/mono_chorus.o"
xcrun clang++ -std=c++17 $common -Wall -Wextra -Wpedantic -fno-exceptions -fno-rtti \
  -Isource -Isource/friend -c source/chorus_engine.cpp -o "$objects/chorus_engine.o"
xcrun clang++ -std=c++17 $common -Wall -Wextra -Wpedantic -fno-exceptions -fno-rtti \
  -Isource/friend -c source/friend/core/Chorus.cpp -o "$objects/native_chorus.o"
xcrun clang++ -bundle $common -Wl,-exported_symbol,_clap_entry \
  "$objects/mono_chorus.o" "$objects/chorus_engine.o" "$objects/native_chorus.o" \
  -o "$bundle/Contents/MacOS/MonoChorus"
codesign --force --sign - --timestamp=none "$bundle"
printf 'Built: %s\n' "$bundle"
