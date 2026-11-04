# NanoMatch

A deterministic limit order book and matching engine in C++20, focused on low-latency design and
reproducible measurement.

**Status:** under active development. Core types and the build are in place; the matching engine is
not implemented yet. No performance results have been published.

## Documentation

- [Specification](docs/SPEC.md): order types, matching rules, binary formats, determinism guarantees
- [Design log](docs/DESIGN.md): what was decided at each milestone and why

## Building

Requires GCC 13+ or Clang 18+, CMake 3.25+, and Ninja.

```sh
make build test            # release build and tests
make asan                  # tests under AddressSanitizer and UndefinedBehaviorSanitizer
make tsan                  # tests under ThreadSanitizer
make check                 # everything CI runs
```

On recent Linux kernels, ThreadSanitizer may need `sudo sysctl -w vm.mmap_rnd_bits=28`.

## License

[MIT](LICENSE)
