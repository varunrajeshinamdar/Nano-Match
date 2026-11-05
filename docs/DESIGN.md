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

## M2: Baseline book and test suite (2026-11-04)

**Summary:** Implemented the full specification in `src/baseline/baseline_book.hpp` using standard containers, added
log file reading and writing, the `nm_replay` tool, and a behavioural test suite that any future book will also run.

**Decisions:**
- `std::map` of `std::list` with `std::unordered_map` for lookup: the obvious design on purpose. It is a behavioural
  reference for differential testing and a cost reference for benchmarks, so clarity beats speed here.
- Allocator as a template parameter: the same containers can run on a pool later, which separates the cost of
  allocation from the cost of the data structures in comparisons.
- `MatchingBook` and `EventSink` concepts rather than virtual interfaces: event delivery can be inlined, and tests,
  tools and benchmarks are written once against all implementations.
- Tests are typed over the book type: the optimized book inherits the entire suite by being added to one list.
- Randomized testing against an independent model: example tests only cover cases someone thought of. The model
  tracks every queue and checks each command's events, so priority errors are caught at the command that caused them.
- Seeded `std::mt19937_64` with modulo in the test stream: the engine's output is specified by the standard, while
  `std::` distributions are not, so a failing seed reproduces on any toolchain.
- Independent Python encoding of the specification's worked example: it checks encoding, matching and hashing against
  a reading of the specification that shares no code with the engine.

**Specification changes:** ID 0 is reserved, since events use it to mean "no order". The `side` and `leaves` fields of
every event type are now fully defined. The hash is defined to cover the whole file.

**Verification:** Tests pass on GCC 13 and Clang 18, in debug, release, ASan with UBSan, and TSan. Line coverage was
reviewed, and the only unexecuted line left is a failed read after a successful open. To check that the tests can
fail, 14 bugs were injected one at a time, including LIFO queues, wrong trade prices, off-by-one capacity and wrong
modify priority. The unit tests caught all 14, and the randomized model independently caught 12. It missed a
validation bug in code it shares with the book, which the unit tests cover, and a reject field it didn't check, which
it now does.

**Results:** None. Measurement starts at M3.

**Known limitations:** Slow by design. A failed allocation terminates the process, which the specification leaves
undefined.

## M3: Workload generator and benchmark harness (2026-11-05)

**Summary:** Added seeded workload generation (`src/workload/`, `nm_gen`), the measurement harness (`bench/`), machine
setup and orchestration (`scripts/`), and the method in `BENCHMARKS.md`, then took the first baseline measurements.

**Decisions:**
- Workloads committed before any measurement: git history shows they weren't chosen after seeing results.
- Integer-only generation with its own PRNG: `std::` distributions and floating point can differ between toolchains,
  and a workload has to be identical everywhere for results and hashes to be comparable.
- A baseline book inside the generator: flow has to react to the real book, so that crossing orders cross and cancels
  name resting orders. Keeping a separate approximation of the book would drift.
- Prices anchored to a wandering reference rather than the best price: inspecting the first draft's output showed
  that orders joined the best price faster than aggression removed them, which pinned the price. A static price keeps
  every access in a small window and would have flattered cache behaviour, so this makes the benchmark harder.
- Rebalanced `sweep`: the first draft removed liquidity about eight times faster than it arrived, and the book
  collapsed to a few hundred orders. That measured a thin book rather than sustained aggression.
- Separate untimed and timed runs: per-command timers inflate the average, so it comes from a run without them, and
  the distribution comes from a run that reports the timer's own cost beside it.
- Counters read in-process with `perf_event_open`: counting can start and stop around the measured loop, and it
  excludes setup, which `perf stat` on the whole process would include.
- The harness refuses to write results if runs of one book on one log disagree on the event count or hash, because
  that would mean they didn't do the same work.
- Citability recorded in each file: an experiment can't be mistaken for a result, and only citable files reach the
  tables.

**Verification:** Tests cover the generators against published reference values, percentile rank and rounding,
counters, and workload properties such as determinism, depth control and flow validity. A pipeline test runs the
generator, replay and harness together. Results below come from a clean tree on GCC, with counters unmultiplexed.

**Results:** [turbo off](../results/20261104T141813_3b0acefa_baseline_turbo_off.json),
[turbo on](../results/20261105T153214_3b0acefa_baseline_turbo_on.json). Tables are in `BENCHMARKS.md`.

**Known limitations:** Workloads are synthetic and not calibrated to any venue. Measurement is on a laptop without
isolated cores or interrupt steering. Timed runs have their histogram update between samples, which can disturb
the cache slightly.

