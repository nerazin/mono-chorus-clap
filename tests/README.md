# Offline tests

On Linux, run the self-contained CLAP interface and continuous-stream checks
with a C11/C++17 compiler:

```sh
./tests/run_linux.sh
```

The script builds in a temporary directory, checks parameter IDs, state
round-tripping, audio ports and latency reporting, and processes continuous
signals at several host rates. Without an argument it checks 0.4.0 state
against itself. To also check loading state created by a Linux build of
version 0.3.0, pass that build's path:

```sh
./tests/run_linux.sh /absolute/path/to/MonoChorus-0.3.0.clap
```

`clap_render.c` is a small offline CLAP host for comparisons using custom
inputs. The firmware itself and the separate emulator used for the reference
comparisons are not included here, so the published self-contained checks do
not reproduce those firmware comparisons.
