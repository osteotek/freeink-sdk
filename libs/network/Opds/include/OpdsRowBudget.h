#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "OpdsEntry.h"

// Heap a feed parser may spend on what it keeps. A feed is parsed while the
// rest of it still arrives over the network, and an allocation that fails then
// aborts the firmware. Free heap swings by several KB as network buffers come
// and go, so a parser does not test it per row: it takes a budget from the free
// heap when it starts and charges the rows and strings it keeps.
class OpdsRowBudget {
 public:
  // Heap left out of the budget: network receive buffers, expat's own growth and
  // the row being built, which is charged only once complete. These grow with
  // the heap that is free: on the X4, with the TLS record buffer already lent
  // (see OpdsTransport::get), they took up to 52% of the free heap at parser
  // start (21.7 KB of 43 KB) beyond what the budget charged.
  static constexpr size_t RESERVE_PERCENT = 60;
  static constexpr size_t MIN_RESERVE = 12 * 1024;
  // Last resort for a network burst larger than the reserve: rows stop when
  // free heap falls below this, which a measured parse never came near.
  static constexpr size_t MIN_FREE_HEAP = 4 * 1024;
  // Smallest first row storage worth parsing a feed into.
  static constexpr size_t MIN_FIRST_ROWS = 8;
  // Facet rows are optional and come before the entries in an Atom feed, so
  // they leave this much for the entries: a few typical catalog entries.
  static constexpr size_t FACET_HEADROOM = 4 * 1024;

  // Heap kept out of the budget when `freeHeap` is free at parser start.
  static size_t reserveFor(size_t freeHeap);
  // Takes the budget from the free heap now; false when nothing is left.
  bool start();
  // True while free heap is above MIN_FREE_HEAP.
  static bool heapAboveFloor();
  size_t remaining() const { return used < limit ? limit - used : 0; }
  // Charges a string the parse keeps besides its rows (feed links, titles).
  // It is needed, so it is charged even past the budget.
  void charge(const std::string& s);
  // Charges what `s` grew by since it held `heldBefore` (see heldBy()).
  void chargeGrowth(const std::string& s, size_t heldBefore);
  static size_t heldBy(const std::string& s);
  // Charges an optional string; false, with nothing charged, if it does not fit.
  bool tryCharge(const std::string& s);

  // Reserves the first row storage: a share of the budget, between `minRows`
  // and `maxRows`. Storage is sized here because growing it later needs one
  // large free block, which is rare while the feed streams.
  bool reserveFirst(std::vector<OpdsEntry>& rows, size_t minRows, size_t maxRows);

  // Appends `row` unless `rows` already holds `maxRows`, or its strings and any
  // storage growth would leave less than `keepLeft` of the budget. The first
  // refusal latches in `stopped`, so the kept rows stay a prefix of the feed.
  bool append(std::vector<OpdsEntry>& rows, size_t maxRows, bool& stopped, OpdsEntry&& row, size_t keepLeft = 0);

 private:
  bool grow(std::vector<OpdsEntry>& rows, size_t maxRows, size_t needAfter);

  size_t limit = 0;
  size_t used = 0;
};
