# Order Book (C++)

A single-file limit order book matching engine in C++17. Prices are integers (pence), orders are matched with price-time priority, and the hot path uses flat arrays and a pre-allocated memory pool instead of node-based containers.

I wrote this as a rebuild of an order book I first made in Python in a timed event, where a type error (an integer placed in an output string without conversion) printed identically to the terminal but failed the checker, and Python never warned me. Here, prices and quantities are fixed-width integer types and order sides and types are strongly typed enums, so that class of mistake does not compile.

## Features

- Order types: **Limit**, **Market**, **IOC** (immediate-or-cancel), **FOK** (fill-or-kill), **Stop** and **Stop-Limit**
- Price-time priority: best price first, then first-in-first-out within a price level
- Trades execute at the **resting** order's price
- Cancel and modify (reducing quantity at the same price keeps queue priority; any other change loses it)
- Stop orders trigger on the last trade price, and re-enter as Market (Stop) or Limit (Stop-Limit) orders
- Prices are integers in pence (`10150` = £101.50), so there is no floating-point rounding in matching

## Design

| Piece | How it works |
|---|---|
| Price levels | Four flat arrays (bids, asks, buy stops, sell stops) indexed directly by `price - minPrice`, so finding a level is O(1) |
| Orders | A contiguous pool of `OrderNode`s with a free list; all memory for orders is allocated up front |
| Queues | Each price level holds an intrusive doubly linked list of pool indices, so cancelling an order is O(1) |
| ID lookup | A custom open-addressing hash table (linear probing, power-of-two capacity) mapping order ID to pool index |
| Best prices | The best bid, best ask and stop levels are tracked as indices, so reading the top of the book is O(1) |

## Build and run

```bash
g++ -std=c++17 -O2 -Wall -Wextra orderbook.cpp -o orderbook
./orderbook
```

The `main` function shows a small scenario: resting limit orders, a stop-limit order that is triggered by an IOC sweep, and a modify that keeps queue priority. Expected output:

```
--- EXECUTED TRADES ---
Buy #6 <-> Sell #2 | Qty: 15 @ £101.00 (10100p)
Buy #6 <-> Sell #1 | Qty: 5 @ £102.50 (10250p)
Buy #5 <-> Sell #1 | Qty: 5 @ £102.50 (10250p)
-----------------------

--- UK ORDER BOOK (GBP) ---
ASKS (Sell):
---------------------------
BIDS (Buy):
  £102.50 (10250p) | Qty: 3
  £99.50 (9950p) | Qty: 12
  £98.00 (9800p) | Qty: 10
---------------------------
```

## Testing

`orderbook_differential_test.cpp` compares the order book against a deliberately simple reference implementation built on `std::map` and `std::deque`. It runs 200 random sessions of 2,000 operations each (limit, market, IOC, FOK and cancel orders) and checks that every trade (buyer, seller, price, quantity) and the total quantity at every price level are identical.

```bash
g++ -std=c++17 -O1 -g -fsanitize=address,undefined orderbook_differential_test.cpp -o test
./test
```

The test builds with AddressSanitizer and UndefinedBehaviorSanitizer enabled. It reaches into the private members using a `#define private public` hack, which is acceptable for a test but not something to copy into production code.

**Not covered by the tests:** stop and stop-limit orders, and `modifyOrder`. These are exercised only by the example in `main`.

## Known limitations

This is a learning project, not a production system. I know of the following issues, roughly from most to least important:

1. **Stop trigger ordering.** Triggered stop orders are re-injected from inside `matchOrder`, before the incoming order's own unfilled remainder has been placed on the book. Re-injection also recurses through `addOrder`. A cleaner design would queue triggered orders and process them once the incoming order is fully handled.
2. **Modify is not atomic.** Changing price or increasing size cancels the order and re-adds it. If the re-add fails (for example, the new price is outside the price band), the original order is already gone.
3. **Partial failure when the pool is full.** If the order pool is exhausted after an incoming order has already traded, `addOrder` returns `false` even though trades were recorded.
4. **Hash table robustness.** Deleted entries are never cleaned up, `insert` has no guard against a full table, and the constructor does not check that the capacity is a power of two (the index mask depends on it). It works because the default capacity is larger than the order pool, which is an unchecked assumption.
5. **Best-price scanning.** When the best bid or ask level empties, the next best price is found by a linear scan over empty levels, so a single operation can cost O(price range). A bitset or hierarchical bitmap over the levels would fix this.
6. **Memory cost scales with price range.** The dense price arrays are fast but use memory proportional to the number of price levels, which would be impractical for a very wide price range or very fine tick size.
7. **Heap allocation remains.** The fixed-capacity pool and hash table avoid per-order allocation, but the trades vector grows without bound and `triggerStops` uses a temporary vector.
8. **Fixed tick size and band.** Every price is a whole number of pence within a configured band (default 1 to 50,000 pence). Prices outside the band are rejected.
9. **Single-threaded, no self-trade prevention, no timestamps, no market data feed.** Order priority is arrival order only.
10. **No benchmarks.** I have not measured throughput or latency, so I make no performance claims.

## Possible next steps

- Fix limitations 1 to 4 and extend the differential test to cover stops and modifies
- Add a benchmark (a million random orders) and report results with hardware details
- Replace the linear best-price scan with a bitset
- Try an unordered-map or sparse-level version to compare memory use and speed
