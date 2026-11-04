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
