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

## M4: Optimized book (2026-11-05)

**Summary:** Added `OrderBook` in `include/nanomatch/`, built from a fixed order pool, a price ladder, a level bitmap
and an open-addressing ID index. It behaves identically to the baseline, checked by differential testing and fuzzing.

**Decisions:**
- One dense ladder for both sides: the book is never crossed between commands, so a price holds orders for only one
  side at a time, and each level needs only a head, tail and running total. This costs memory proportional to the band,
  and in return gives O(1) level access with no pointer chasing.
- Hierarchical bitmap per side rather than scanning for the next occupied level: a sparse ladder could mean a long scan
  on exactly the commands that clear a level. Each layer summarizes the one below, so the search stays bounded.
- Pool nodes linked by 32-bit index and aligned to 32 bytes: two fit in a cache line and none straddles a boundary. The
  free list is a stack, so the most recently freed node, which is the most likely to be cached, is reused first.
- Open addressing with backward-shift deletion at load 0.5 or less: neighbouring slots share cache lines, and there are
  no tombstones to slow lookups as a long-running book churns.
- Fibonacci hashing: order IDs are nearly sequential, so masking their low bits would cluster them.
- `Side` as a template parameter in matching: each side compiles to its own fixed comparisons, leaving a single side
  branch per command.
- A modified order that loses priority keeps its pool slot and index entry while matching, which is what guarantees
  that its remainder always has room to rest.
- Page policy as a template parameter: huge pages become a measured option rather than an assumption.
- Baseline allocator variant (`PoolAllocator`): it gives a three-way comparison, separating what comes from avoiding
  the general-purpose allocator from what comes from the data structures.
- "Static" here means statically sized and allocated once at startup, not static storage duration, because capacity is
  a runtime setting.

**Bug found:** With 379 tests passing, the fuzzer failed after 26 inputs. `IdIndex::find` compared keys before checking
for an empty slot, and 0 is both the empty marker and a value a command can carry, so a cancel or modify of ID 0
matched an empty slot and acted on pool slot 0. No test or generated stream had sent ID 0 to those commands. The fix
checks for empty first. Regression tests were added at the unit and book levels, the randomized stream now sends ID 0,
and the failing input is kept in `tests/fuzz/regressions`, which runs on every fuzz session.

**Verification:** The fast book passes the same typed behavioural suite and randomized model as the baseline. The
differential test compares every event byte for byte on random streams, all main workloads, scarce capacity and a
narrow band. Components are checked against `std::set` and `std::unordered_map`. Fuzzing in CI runs both books under
ASan and UBSan. 16 bugs were injected one at a time into the fast book, and the differential test caught all 16.

**Results:** Measured with M5.

**Known limitations:** Memory is proportional to band width whether used or not, and bands wider than 2^26 ticks are
refused at construction. The ID scheme assumes one sequencing sender.

## M5: Zero-allocation proof and measurement (2026-11-05)

**Summary:** Proved that the optimized book never touches the heap, measured all three designs on identical workloads,
and made changes guided by profiling.

**Decisions:**
- Allocation proof by replacing the C allocator for the whole test process: everything that reaches the heap ends up
  there, including `operator new` and standard containers. Controls show that the hook sees direct calls, containers
  and the baseline book, so a zero count means something. There's no warmup, so early orders count too.
- Three-way comparison on identical logs: `baseline`, `baseline_pool` and `fast`. The results show that avoiding the
  general-purpose allocator alone barely changes L1 misses per command, while removing the tree, list and node-based map
  accounts for most of the drop. Miss rate stays about the same at 10,000 resting orders, because the fast book reduces
  loads as much as misses. Comparing all three is what makes this visible.
- 8-byte index slots holding the low 32 bits of the ID, confirmed against the pool node: this halves the table with no
  change to average latency. A smaller table at higher load was also measured, and it lowered tail latency but raised
  average latency and branch misses, because longer probe runs mispredict more often. The lower load was kept, since it
  gave the better average without any correctness trade-off.
- Copying each command before the timer starts in timed runs: all kinds of command had about the same p99.9 whatever
  their work, which pointed at the harness rather than the engine. Samples were including the fetch from a log of about
  a gigabyte. The cost of receiving a command is measured properly by the pipeline benchmark.
- Latency reported by kind of command, because a percentile over all commands mixes very different amounts of work.

**Bugs found:**
- Shrinking the index first gave a capacity-1 book a one-slot table with no empty slot, which made erase loop forever. A
  test hung and exposed this. Tables now always keep an empty slot, and test timeouts make a hang fail quickly.
- Applying benchmark settings twice with a CPU offline failed partway, leaving turbo unchanged, so a "turbo on" run was
  really turbo off. It was noticed from a warning, stopped, and discarded before writing any file. Setup now verifies
  itself, and result labels are read from the machine.

**Verification:** The no-allocation test passes on all main workloads and on reject and full-book paths. Page faults and
context switches during measured loops are recorded in each result. Counters were not multiplexed in any run.

**Results:** [books, turbo off](../results/20261105T174430_10c22830_book_turbo_off.json),
[books, turbo on](../results/20261105T180024_e966fcf2_book_turbo_on.json). Tables are in `BENCHMARKS.md`.

**Known limitations:** Tail latency varies noticeably between runs on this machine, with some p99.9 runs differing by
several times, which appears to come from activity outside the measured process. The median across runs is reported
along with the range in the raw files.

## M6: SPSC ring and pipeline (2026-11-05)

**Summary:** Added `SpscRing` and a pipeline carrying commands from a gateway thread to a book on another core, with
benchmarks that isolate each layout decision.

**Decisions:**
- Free-running 64-bit indices masked only to find a slot: `tail - head` is exact for both full and empty, so no slot is
  wasted, and the counters can't wrap in practice.
- Acquire and release only, justified at each use: each side's own index can be read relaxed because only that thread
  writes it. On x86 these orderings need no fence instructions, and they still constrain the compiler.
- Operations are wait-free: no locks, no retry loops and no compare-and-swap. A full or empty ring returns false, and
  the caller decides how to wait.
- Layout as a template parameter: index alignment and index caching can each be switched off, so that the benchmark
  shows what each contributes. Assumptions were tested rather than repeated. Caching with padding helped the 32-byte
  payload, while results for the 8-byte payload were noisier and didn't show a clear benefit from caching.
- Pipeline throughput and latency measured in separate modes: a saturated queue mostly measures time spent waiting, so
  latency sends one command at a time.

**Verification:** Tests cover capacity, order across wraps, and a two-thread test checking every item, run under TSan for
all layouts. The pipeline test requires the same event hash as a single-threaded run. The harness refuses to write
results if runs of a book disagree about the work done.

**Results:** [ring](../results/20261105T180500_e966fcf2_ring_turbo_on.json),
[pipeline](../results/20261105T180704_e966fcf2_pipeline_turbo_on.json). Tables are in `BENCHMARKS.md`.

**Known limitations:** Throughput of some layouts varied widely between runs, so medians matter more than peaks. Pipeline
latency relies on this CPU's synchronized TSC.

## M7: Deterministic replay and timing repeatability (2026-11-05)

**Summary:** Added golden event log hashes checked on every book and threading mode, and measurements of the timer's
resolution and of timing repeatability.

**Decisions:**
- Determinism verified by hashes, separately from timing: timing can't show that output is identical, and identical
  output says nothing about timing. These are different properties, measured by different tools.
- Golden hashes run as ordinary tests: CI's compiler and build type matrix then covers GCC and Clang, debug and release,
  and sanitizer builds, without a separate pipeline.
- Three repeatability figures rather than one: the timer measuring nothing, the run-to-run spread of a whole replay's
  average, and the spread of a single command across replays. They differ greatly, and averaging over a million commands
  cancels most noise, so the run-to-run figure is much smaller than the per-command one. A single "jitter" number would
  be meaningless without saying which it is.

**Verification:** `make replay-check` reproduces the recorded hashes for four book variants on one and two threads, on three
workloads. The jitter benchmark fails if any replay produces different events.

**Results:** [timing repeatability](../results/20261105T180253_e966fcf2_jitter_turbo_on.json). Tables are in `BENCHMARKS.md`.

**Known limitations:** Timing can't resolve differences below the serialized timer's own spread, even though one TSC tick
is shorter than that.

