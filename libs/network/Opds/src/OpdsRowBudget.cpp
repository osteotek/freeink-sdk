#include "OpdsRowBudget.h"

#include <Arduino.h>
#include <OpdsLog.h>

#include <algorithm>
#include <utility>

namespace {
// Per heap buffer: TLSF block header (4 B) plus light heap poisoning (8 B head,
// 4 B tail); the size itself is rounded up to 4 B.
constexpr size_t ALLOC_OVERHEAD = 16;
// Largest free block left after (re)allocating row storage, so the strings of
// the rows that follow still find room.
constexpr size_t STORAGE_BLOCK_SLACK = 4096;
// Share of the budget the first row storage may take; the rest is for strings.
// Catalog entries hold about 1 KB of strings against 228 B of storage each.
constexpr size_t FIRST_STORAGE_PERCENT = 20;
constexpr size_t FIRST_GROWTH_ROWS = 8;  // storage for a list that starts empty
constexpr size_t SMALL_GROWTH_ROWS = 4;  // growth step when no block fits a doubling

size_t heapBytes(const std::string& s) {
  static const size_t inlineCapacity = std::string().capacity();
  return s.capacity() > inlineCapacity ? ((s.capacity() + 1 + 3) & ~size_t{3}) + ALLOC_OVERHEAD : 0;
}

size_t heldBytes(const OpdsEntry& e) {
  return heapBytes(e.title) + heapBytes(e.author) + heapBytes(e.href) + heapBytes(e.id) + heapBytes(e.heading) +
         heapBytes(e.detail) + heapBytes(e.selfHref) + heapBytes(e.description) + heapBytes(e.coverHref);
}
}  // namespace

size_t OpdsRowBudget::reserveFor(const size_t freeHeap) {
  return std::max(freeHeap * RESERVE_PERCENT / 100, MIN_RESERVE);
}

bool OpdsRowBudget::start() {
  const size_t free = ESP.getFreeHeap();
  const size_t reserve = reserveFor(free);
  limit = free > reserve ? free - reserve : 0;
  used = 0;
  return limit > 0;
}

bool OpdsRowBudget::heapAboveFloor() { return ESP.getFreeHeap() >= MIN_FREE_HEAP; }

void OpdsRowBudget::charge(const std::string& s) { used += heapBytes(s); }

size_t OpdsRowBudget::heldBy(const std::string& s) { return heapBytes(s); }

void OpdsRowBudget::chargeGrowth(const std::string& s, const size_t heldBefore) {
  const size_t held = heapBytes(s);
  if (held > heldBefore) used += held - heldBefore;
}

bool OpdsRowBudget::tryCharge(const std::string& s) {
  const size_t bytes = heapBytes(s);
  if (bytes > remaining()) return false;
  used += bytes;
  return true;
}

bool OpdsRowBudget::reserveFirst(std::vector<OpdsEntry>& rows, const size_t minRows, const size_t maxRows) {
  const size_t block = ESP.getMaxAllocHeap();
  const size_t byBlock = block > STORAGE_BLOCK_SLACK ? (block - STORAGE_BLOCK_SLACK) / sizeof(OpdsEntry) : 0;
  const size_t byShare = remaining() * FIRST_STORAGE_PERCENT / 100 / sizeof(OpdsEntry);
  const size_t capacity = std::min({std::max(byShare, minRows), maxRows, byBlock});
  if (capacity < minRows || capacity * sizeof(OpdsEntry) > remaining()) return false;
  used += capacity * sizeof(OpdsEntry);
  rows.reserve(capacity);
  return true;
}

bool OpdsRowBudget::grow(std::vector<OpdsEntry>& rows, const size_t maxRows, const size_t needAfter) {
  const size_t oldCapacity = rows.capacity();
  // Double when a large enough block is free; mid-stream it rarely is, so fall
  // back to a few rows at a time.
  for (const size_t step : {std::max(oldCapacity, FIRST_GROWTH_ROWS), SMALL_GROWTH_ROWS}) {
    const size_t newCapacity = std::min(oldCapacity + step, maxRows);
    if (newCapacity <= oldCapacity) return false;
    // Growing moves the rows' strings without copying them, but holds the old
    // and new storage together for a moment: the budget must cover the new one whole.
    const size_t newBytes = newCapacity * sizeof(OpdsEntry);
    if (ESP.getMaxAllocHeap() < newBytes + STORAGE_BLOCK_SLACK || newBytes + needAfter > remaining()) continue;
    used += (newCapacity - oldCapacity) * sizeof(OpdsEntry);
    rows.reserve(newCapacity);
    return true;
  }
  return false;
}

bool OpdsRowBudget::append(std::vector<OpdsEntry>& rows, const size_t maxRows, bool& stopped, OpdsEntry&& row,
                           const size_t keepLeft) {
  if (stopped) return false;
  const size_t bytes = heldBytes(row);
  const char* refusal = nullptr;
  if (rows.size() >= maxRows) {
    refusal = "row cap";
  } else if (bytes + keepLeft > remaining()) {
    refusal = "budget";
  } else if (!heapAboveFloor()) {
    refusal = "heap floor";
  } else if (rows.size() == rows.capacity() && !grow(rows, maxRows, bytes + keepLeft)) {
    refusal = "storage";
  }
  if (refusal) {
    stopped = true;
    LOG_DBG("OPDS", "Rows stop at %u (%s); budget left %u", static_cast<unsigned>(rows.size()), refusal,
            static_cast<unsigned>(remaining()));
    return false;
  }
  used += bytes;
  rows.push_back(std::move(row));
  return true;
}
