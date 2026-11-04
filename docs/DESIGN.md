# Design Log

Append-only record of what was decided at each milestone and why.

<!--
Entry format:

## M<n>: <title> (YYYY-MM-DD)

**Summary:** what was built.

**Decisions:**
- <decision>: <why>; <alternatives rejected>.

**Verification:** tests and checks that demonstrate it works.

**Results:** links to files in results/. No figures without a results file.

**Known limitations:** what this does not handle yet.
-->

## M0: Scaffold and specification (2026-11-04)

**Summary:** Created the repository layout and wrote the behaviour specification in `SPEC.md`. No engine code yet.

**Decisions:**
- Integer ticks and lots: exact comparison and identical results on every run; floating point rejected.
- Strictly increasing order IDs: duplicate detection needs one stored value and takes constant time; a set of all
  seen IDs was rejected because it grows without bound. This assumes a single sequencing sender, and real venues
  don't always offer that guarantee.
- Fixed-size little-endian binary records: replay involves no parsing and output can be compared byte for byte;
  text formats were rejected for their size and parsing cost.
- Quantity reductions keep queue priority, while quantity increases and price changes lose it: this follows common
  exchange practice, since a reduction doesn't disadvantage other orders in the queue.
- Market orders never rest: with no limit price there is no level to rest at.
- Capacity is checked only at the point of resting: an order that fills completely doesn't need a slot.

**Verification:** None applicable; no code yet. The specification is enforced from M2 onwards.

**Results:** None.

**Known limitations:** One instrument per book. No self-trade prevention, IOC/FOK limits, stops, icebergs, auctions,
networking or recovery.

## M1: Build system and core types (2026-11-04)

**Summary:** Added the CMake build with presets, a Makefile wrapper, CI, static analysis configuration, strong
integer types, and the binary record definitions from section 7 of the specification.

**Decisions:**
- `StrongInt<Rep, Tag>` wrappers for prices, quantities, IDs and sequences: mixing them up becomes a compile error
  at no runtime cost; bare integers and `enum class` integers were rejected because they give up either safety or
  arithmetic.
- Arithmetic only where it has meaning: quantities add and subtract, while prices and IDs don't. Price arithmetic
  will be added only if a real need appears.
- Compile-time layout assertions, including `has_unique_object_representations`: records are logged as raw bytes
  and compared by hash, so padding bytes would break determinism. The build now fails if layout drifts from the
  specification.
- Enums with fixed underlying types: any byte read from a log is a valid enum value, so bad input reaches validation
  and is rejected as malformed rather than being undefined behaviour.
- GoogleTest fetched at a pinned version with a checksum: builds are reproducible and need no system package.
- Warnings attached to an interface target: strict warnings apply to project code without failing on dependencies.
- `-march=native` only in the `release` and `profile` presets: those target this machine, while debug and sanitizer
  builds stay portable.

**Verification:** 13 tests pass on GCC 13 and Clang 18 in debug, release, ASan with UBSan, and TSan. Layout is
checked at compile time. `clang-format` and `clang-tidy` are clean. CI runs all of this on every push.

**Results:** None. CI never produces performance numbers.

**Known limitations:** No matching logic yet. TSan needs `vm.mmap_rnd_bits=28` on recent kernels.
