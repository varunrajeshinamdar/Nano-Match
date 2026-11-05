# NanoMatch

A deterministic limit order book and matching engine in C++20, focused on low-latency design and
reproducible measurement.

**Status:** feature complete, with [published results](docs/BENCHMARKS.md#results). Final experiments and
documentation are in progress.

## Documentation

- [Specification](docs/SPEC.md): order types, matching rules, binary formats, determinism guarantees
- [Design log](docs/DESIGN.md): what was decided at each milestone and why
- [Benchmarks](docs/BENCHMARKS.md): measurement method, workloads and results

## Building

Requires GCC 13+ or Clang 18+, CMake 3.25+, and Ninja.

```sh
make build test            # release build and tests
make asan                  # tests under AddressSanitizer and UndefinedBehaviorSanitizer
make tsan                  # tests under ThreadSanitizer
make replay-check          # every book and threading mode reproduces the recorded event hashes
make fuzz                  # coverage-guided fuzzing of both books under sanitizers, requires Clang
make coverage              # coverage report in build/coverage/html
make check                 # format, tests, sanitizers and static analysis
make matrix                # every compiler and preset CI covers, from a clean build
```

On recent Linux kernels, ThreadSanitizer may need `sudo sysctl -w vm.mmap_rnd_bits=28`.

## Replaying a log

`nm_replay` runs a command log through a book and prints the hash of the resulting event log. Two runs
that print the same hash produced the same events.

```sh
build/release/tools/nm_replay --in tests/data/spec_example.nmc \
    --min-price 1 --max-price 1000 --capacity 16 --out /tmp/events.nme
```

## License

[MIT](LICENSE)
