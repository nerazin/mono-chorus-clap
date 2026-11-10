#!/bin/sh
set -eu
cd "$(dirname "$0")/.."

test_tmp=$(mktemp -d "${TMPDIR:-/tmp}/mono-chorus-test.XXXXXXXX")
trap 'rm -rf -- "$test_tmp"' EXIT HUP INT TERM

cc -std=c11 -O2 -Wall -Wextra -Wpedantic -fPIC -fvisibility=hidden \
  -I vendor/clap/include -c source/mono_chorus.c -o "$test_tmp/wrapper.o"
c++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -fPIC -fvisibility=hidden \
  -I source -I source/friend -c source/chorus_engine.cpp -o "$test_tmp/engine.o"
c++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -fPIC -fvisibility=hidden \
  -I source/friend -c source/friend/core/Chorus.cpp -o "$test_tmp/native.o"
c++ -shared "$test_tmp/wrapper.o" "$test_tmp/engine.o" "$test_tmp/native.o" \
  -lm -o "$test_tmp/MonoChorus.clap"

cc -std=c11 -O2 -Wall -Wextra -Wpedantic -I vendor/clap/include \
  tests/clap_contract.c -ldl -lm -o "$test_tmp/clap_contract"
c++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -I source tests/stream_stress.cpp \
  "$test_tmp/engine.o" "$test_tmp/native.o" -lm -o "$test_tmp/stream_stress"

old_plugin=${1:-"$test_tmp/MonoChorus.clap"}
"$test_tmp/clap_contract" "$old_plugin" "$test_tmp/MonoChorus.clap"
"$test_tmp/stream_stress"
