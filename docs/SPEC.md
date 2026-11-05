# NanoMatch Specification

This document defines the externally observable behaviour of the matching engine. Every book
implementation must produce byte-identical output for the same input, including the baseline.

## 1. Scope

A book serves one instrument and processes one command at a time, in log order.

Out of scope: multiple instruments in one book, self-trade prevention, time-in-force variants
(IOC or FOK limit orders), stop and iceberg orders, auctions, networking, and crash recovery.

## 2. Numeric representation

| Field | Type | Notes |
|---|---|---|
| Price | `int64`, integer ticks | Valid range is `[min_price, max_price]`, with `min_price >= 1` |
| Quantity | `uint32`, integer lots | Must be at least 1 in any order or modify command |
| Order ID | `uint64` | See section 3 |
| Sequence | `uint64` | See section 7 |

Floating point is not used for prices or quantities. The mapping from ticks to currency belongs to
the caller. The price band and order capacity are fixed when the book is constructed. An implementation
may refuse a configuration it can't provide, which must happen at construction rather than while
processing commands.

## 3. Order IDs

- The sender assigns order IDs. The engine does not generate them.
- ID 0 is reserved, because events use it to mean "no order". A new order with ID 0 is rejected as
  `DUPLICATE_ID`, which follows from the high-water mark below starting at 0.
- Each new order's ID must be strictly greater than the ID of every earlier well-formed limit or
  market command in that session, whether or not that command was accepted. An ID is therefore never
  reused, including after the order has left the book.
- An ID that breaks this rule is rejected as `DUPLICATE_ID`. This check needs one stored value and
  takes constant time.
- A modified order keeps its ID.
- Cancel or modify of an ID that is not currently resting is rejected as `UNKNOWN_ID`. This includes
  IDs that were filled, cancelled, or never seen.

## 4. Commands

### 4.1 Limit

Matches against the opposite side while the prices cross, then rests any remainder at its limit price.

### 4.2 Market

Matches against the opposite side at any price. A market order never rests. Any remainder is
cancelled with reason `MARKET_REMAINDER`, including the whole order if the opposite side is empty.

### 4.3 Cancel

Removes a resting order in full.

### 4.4 Modify

Replaces a resting order's price and open quantity. `quantity` is the new open quantity, not a
change relative to the old one and not a new total including prior fills.

| Change | Queue priority |
|---|---|
| Same price, same quantity | Kept |
| Same price, lower quantity | Kept |
| Same price, higher quantity | Lost |
| Different price | Lost |

An order that loses priority moves to the back of the queue at its new price. If the new price
crosses the opposite side, the order matches as an aggressor first, then rests any remainder.

## 5. Matching rules

- **Price-time priority.** Better prices match first: highest bid, lowest ask. Within one price,
  orders match in the order they joined that level (FIFO).
- **Crossing.** A buy crosses when its price is greater than or equal to the best ask. A sell crosses
  when its price is less than or equal to the best bid.
- **Trade price.** Always the resting order's price.
- **Partial fills.** A partially filled resting order keeps its queue position.
- **Book invariant.** After each command completes, best bid is strictly less than best ask whenever
  both sides are non-empty.

## 6. Validation and capacity

Checks run in this order, and the first failure decides the reject reason. No book state changes on
a reject, except that a new order's ID still advances the high-water mark from section 3.

| # | Check | Applies to | Reject reason |
|---|---|---|---|
| 1 | Known command type; valid side; reserved bytes are zero | All | `MALFORMED` |
| 2 | ID is strictly increasing | Limit, market | `DUPLICATE_ID` |
| 2 | ID is currently resting | Cancel, modify | `UNKNOWN_ID` |
| 3 | Price within band | Limit, modify | `INVALID_PRICE` |
| 4 | Quantity at least 1 | Limit, market, modify | `INVALID_QUANTITY` |

An ID advances the high-water mark only if check 1 passes, because a malformed record's fields can't
be trusted.

A market command's `price` field must be 0. A cancel's `side`, `price` and `quantity` fields must be
0. A modify's `side` field must be 0. Any violation is `MALFORMED`.

**Capacity.** Capacity limits the number of simultaneously resting orders. It is checked only when a
new order is about to rest, so an order that fills completely never needs a slot. If the book is
full, fills already made stand, and the remainder is cancelled with reason `BOOK_FULL`. A modify
reuses its existing slot and can't hit this limit.

## 7. Binary formats

All integers are little-endian. Log files begin with a 16-byte header:

| Offset | Size | Field | Value |
|---|---|---|---|
| 0 | 8 | magic | `NMCMD\0\0\0` for commands, `NMEVT\0\0\0` for events |
| 8 | 2 | version | 1 |
| 10 | 2 | record_size | 32 for commands, 56 for events |
| 12 | 4 | reserved | 0 |

### 7.1 Command record (32 bytes)

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 8 | seq | Strictly increasing within a log |
| 8 | 8 | order_id | |
| 16 | 8 | price | Ticks |
| 24 | 4 | quantity | |
| 28 | 1 | type | 1 limit, 2 market, 3 cancel, 4 modify |
| 29 | 1 | side | 0 none, 1 buy, 2 sell |
| 30 | 2 | reserved | 0 |

A log whose `seq` values are not strictly increasing, or whose header is invalid, is corrupt. The
replay tool stops with an error rather than producing output.

### 7.2 Event record (56 bytes)

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 8 | event_seq | Starts at 1 and increases by 1 per event |
| 8 | 8 | command_seq | `seq` of the command that caused this event |
| 16 | 8 | order_id | The aggressor, for trades |
| 24 | 8 | counter_order_id | The resting order, for trades; otherwise 0 |
| 32 | 8 | price | See table below |
| 40 | 4 | quantity | See table below |
| 44 | 4 | leaves | Open quantity remaining on `order_id` |
| 48 | 4 | counter_leaves | Open quantity remaining on `counter_order_id`; otherwise 0 |
| 52 | 1 | type | See table below |
| 53 | 1 | side | Side of `order_id`; 0 if unknown |
| 54 | 1 | reason | See table below; otherwise 0 |
| 55 | 1 | reserved | 0 |

| Type | Name | `price` | `quantity` |
|---|---|---|---|
| 1 | `ACCEPTED` | Order price (0 for market) | Order quantity |
| 2 | `REJECTED` | As submitted | As submitted |
| 3 | `TRADE` | Resting order's price | Traded quantity |
| 4 | `RESTED` | Resting price | Open quantity |
| 5 | `CANCELED` | 0 | Quantity removed |
| 6 | `MODIFIED` | New price | New open quantity |

`leaves` is the aggressor's remaining quantity on a `TRADE`, equal to `quantity` on `ACCEPTED`, `RESTED`
and `MODIFIED`, and 0 on `REJECTED` and `CANCELED`.

`side` on a `REJECTED` event is the command's `side` field if the command is well-formed, and 0 if it
was rejected as `MALFORMED`, because a malformed record's fields can't be trusted. A rejected cancel
or modify therefore reports 0, even when the ID is resting.

| Reason | Name | Used with |
|---|---|---|
| 1 | `MALFORMED` | `REJECTED` |
| 2 | `DUPLICATE_ID` | `REJECTED` |
| 3 | `UNKNOWN_ID` | `REJECTED` |
| 4 | `INVALID_PRICE` | `REJECTED` |
| 5 | `INVALID_QUANTITY` | `REJECTED` |
| 6 | `USER_CANCEL` | `CANCELED` |
| 7 | `MARKET_REMAINDER` | `CANCELED` |
| 8 | `BOOK_FULL` | `CANCELED` |

### 7.3 Event order per command

| Command | Events |
|---|---|
| Rejected | `REJECTED` |
| Limit | `ACCEPTED`, `TRADE`\*, then `RESTED` or `CANCELED(BOOK_FULL)` if a remainder exists |
| Market | `ACCEPTED`, `TRADE`\*, then `CANCELED(MARKET_REMAINDER)` if a remainder exists |
| Cancel | `CANCELED(USER_CANCEL)` |
| Modify, priority kept | `MODIFIED` |
| Modify, priority lost | `MODIFIED`, `TRADE`\*, then `RESTED` if a remainder exists |

`TRADE`\* means zero or more trades, in matching order.

### 7.4 Example

The book holds a resting sell, ID 10, for 5 lots at 100. A limit buy, ID 11, arrives for 8 lots at 101:

| Event | order_id | counter_order_id | price | quantity | leaves | counter_leaves |
|---|---|---|---|---|---|---|
| `ACCEPTED` | 11 | 0 | 101 | 8 | 8 | 0 |
| `TRADE` | 11 | 10 | 100 | 5 | 3 | 0 |
| `RESTED` | 11 | 0 | 101 | 3 | 3 | 0 |

## 8. Determinism

**Guarantee.** For a fixed book configuration, the same command log produces a byte-identical event
log on every run, for every build type and for every book implementation.

**Verification.** Two event logs are compared by the 64-bit FNV-1a hash of the whole file, header
included, and byte by byte when the hashes differ. This hash detects accidental divergence. It offers no protection against deliberate tampering.

**Requirements on the matching path:**

- No wall clock or TSC reads that can affect output. Ordering comes only from log position.
- No unseeded randomness. Generated workloads record their seed.
- No behaviour dependent on pointer values, allocator state, or hash-table iteration order.
- No floating point.
- No uninitialized bytes in output. Reserved fields and padding are written as zero.
- Timing instrumentation sits outside the matching path, so enabling it can't change any event.
