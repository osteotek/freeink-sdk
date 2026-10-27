// Host tests for OPDS feed parsing under a bounded heap. operator new below
// draws from a budget and throws std::bad_alloc past it, which on the device
// (no exceptions) aborts the firmware. The parsers must stop taking entries
// before that and report the feed as truncated.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <string>
#include <vector>

#if defined(__APPLE__)
#include <malloc/malloc.h>
static size_t blockSize(void* p) { return malloc_size(p); }
#else
#include <malloc.h>
static size_t blockSize(void* p) { return malloc_usable_size(p); }
#endif

#include "Arduino.h"
#include "OpdsFeedParser.h"

namespace {

constexpr size_t UNLIMITED = SIZE_MAX;
size_t liveBytes = 0;
size_t heapBudget = UNLIMITED;
size_t lowestFree = UNLIMITED;

size_t freeBytes() {
  if (heapBudget == UNLIMITED) return 1u << 20;
  return heapBudget > liveBytes ? heapBudget - liveBytes : 0;
}

void* allocate(size_t n) {
  if (heapBudget != UNLIMITED && liveBytes + n > heapBudget) throw std::bad_alloc();
  void* p = std::malloc(n ? n : 1);
  if (!p) throw std::bad_alloc();
  liveBytes += blockSize(p);
  if (freeBytes() < lowestFree) lowestFree = freeBytes();
  return p;
}

void release(void* p) noexcept {
  if (!p) return;
  liveBytes -= blockSize(p);
  std::free(p);
}

}  // namespace

void* operator new(size_t n) { return allocate(n); }
void* operator new[](size_t n) { return allocate(n); }
void* operator new(size_t n, const std::nothrow_t&) noexcept {
  try {
    return allocate(n);
  } catch (const std::bad_alloc&) {
    return nullptr;
  }
}
void* operator new[](size_t n, const std::nothrow_t& tag) noexcept { return operator new(n, tag); }
void operator delete(void* p) noexcept { release(p); }
void operator delete[](void* p) noexcept { release(p); }
void operator delete(void* p, size_t) noexcept { release(p); }
void operator delete[](void* p, size_t) noexcept { release(p); }

EspClass ESP;
uint32_t EspClass::getFreeHeap() const { return static_cast<uint32_t>(freeBytes()); }
uint32_t EspClass::getMaxAllocHeap() const { return static_cast<uint32_t>(freeBytes()); }

namespace {

int checksRun = 0;
int checksFailed = 0;

#define CHECK(condition)                                               \
  do {                                                                 \
    ++checksRun;                                                       \
    if (!(condition)) {                                                \
      ++checksFailed;                                                  \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #condition); \
    }                                                                  \
  } while (0)

// Catalog entries with long tokenised links and a description longer
// than the cap; `small(i)` entries carry only a title and a short link.
std::string longText(const char* stem, size_t count) {
  std::string text;
  while (text.size() < count) text += stem;
  return text.substr(0, count);
}

bool allLarge(size_t) { return false; }
bool allSmall(size_t) { return true; }
bool everyOtherSmall(size_t i) { return i % 2 == 1; }

std::string atomFeed(size_t entries, bool (*small)(size_t) = allLarge, size_t facets = 0) {
  std::string feed = "<?xml version=\"1.0\"?><feed xmlns=\"http://www.w3.org/2005/Atom\"><title>Recent</title>";
  for (size_t i = 0; i < facets; i++) {
    feed += "<link rel=\"http://opds-spec.org/facet\" title=\"Facet " + std::to_string(i) +
            "\" href=\"https://example.org/facet/" + std::to_string(i) + "\"/>";
  }
  for (size_t i = 0; i < entries; i++) {
    const std::string n = std::to_string(i);
    if (small(i)) {
      feed += "<entry><title>Book " + n + "</title><link rel=\"http://opds-spec.org/acquisition\" " +
              "type=\"application/epub+zip\" href=\"https://example.org/" + n + ".epub\"/></entry>";
      continue;
    }
    feed += "<entry><title>Book " + n + "</title><author><name>Author " + n + "</name></author><id>urn:book:" + n +
            "</id><summary>" + longText("Lorem ipsum dolor sit amet. ", 600) + "</summary>" +
            "<link rel=\"http://opds-spec.org/acquisition\" type=\"application/epub+zip\" " +
            "href=\"https://example.org/" + n + ".epub?token=" + longText("Zx9", 500) + "\"/>" +
            "<link rel=\"http://opds-spec.org/image\" type=\"image/jpeg\" " +
            "href=\"https://example.org/cover/" + n + "?token=" + longText("Qy7", 200) + "\"/></entry>";
  }
  return feed + "</feed>";
}

std::string jsonFeed(size_t entries, bool (*small)(size_t) = allLarge) {
  std::string feed = "{\"metadata\":{\"title\":\"Recent\"},\"publications\":[";
  for (size_t i = 0; i < entries; i++) {
    const std::string n = std::to_string(i);
    if (i) feed += ",";
    if (small(i)) {
      feed += "{\"metadata\":{\"title\":\"Book " + n + "\"},\"links\":[{\"rel\":\"http://opds-spec.org/acquisition\"," +
              "\"type\":\"application/epub+zip\",\"href\":\"https://example.org/" + n + ".epub\"}]}";
      continue;
    }
    feed += "{\"metadata\":{\"title\":\"Book " + n + "\",\"author\":\"Author " + n +
            "\",\"identifier\":\"urn:book:" + n +
            "\",\"description\":\"" + longText("Lorem ipsum dolor sit amet. ", 600) + "\"},\"links\":[{\"rel\":" +
            "\"http://opds-spec.org/acquisition\",\"type\":\"application/epub+zip\"," +
            "\"href\":\"https://example.org/" + n +
            ".epub?token=" + longText("Zx9", 500) + "\"}],\"images\":[{\"href\":\"https://example.org/cover/" + n +
            "?token=" + longText("Qy7", 200) + "\"}]}";
  }
  return feed + "]}";
}

struct ParseResult {
  bool threw = false;
  bool error = false;
  bool truncated = false;
  std::vector<OpdsEntry> entries;
  size_t lowestFree = 0;
};

// Streams the feed in network-sized chunks with `heapLeft` bytes of budget
// above what is live once the feed text exists. `networkBytes` stand in for
// receive buffers: held while each chunk after the first is parsed, so the
// parser takes its budget before they come and go.
ParseResult parse(const std::string& feed, const size_t heapLeft, const size_t networkBytes = 0) {
  ParseResult result;
  heapBudget = heapLeft == UNLIMITED ? UNLIMITED : liveBytes + heapLeft;
  lowestFree = UNLIMITED;
  try {
    OpdsFeedParser parser;
    for (size_t at = 0; at < feed.size(); at += 1024) {
      const size_t len = feed.size() - at < 1024 ? feed.size() - at : 1024;
      if (at == 0 || networkBytes == 0) {
        parser.write(reinterpret_cast<const uint8_t*>(feed.data() + at), len);
        continue;
      }
      std::unique_ptr<uint8_t[]> receiveBuffers(new (std::nothrow) uint8_t[networkBytes]);
      parser.write(reinterpret_cast<const uint8_t*>(feed.data() + at), len);
    }
    parser.flush();
    result.error = parser.error();
    result.truncated = parser.truncated();
    result.entries = parser.takeEntries();
  } catch (const std::bad_alloc&) {
    result.threw = true;
  }
  result.lowestFree = lowestFree;
  heapBudget = UNLIMITED;
  return result;
}

bool keptAPrefix(const std::vector<OpdsEntry>& entries) {
  for (size_t i = 0; i < entries.size(); i++) {
    if (entries[i].title != "Book " + std::to_string(i)) return false;
  }
  return true;
}

void checkTightHeap(const char* label, const std::string& feed) {
  // About the free heap an X4 has once TLS is up and the feed starts.
  const ParseResult r = parse(feed, 48 * 1024);
  CHECK(!r.threw);
  CHECK(!r.error);
  CHECK(r.truncated);
  CHECK(!r.entries.empty());
  CHECK(r.entries.size() < 200);
  CHECK(keptAPrefix(r.entries));
  // Only the entry being built, charged once complete, may reach into the
  // reserve: at most its strings at the field caps. The budget is taken after
  // the parser's own setup, from somewhat less than the 48 KB given.
  CHECK(r.lowestFree + 4096 >= OpdsRowBudget::reserveFor(40 * 1024));
  std::printf("%s, 48 KB heap: %zu entries kept, lowest free %zu bytes\n", label, r.entries.size(), r.lowestFree);
}

void testAtomFeedStopsBeforeTheHeapRunsOut() { checkTightHeap("Atom", atomFeed(200)); }

void testJsonFeedStopsBeforeTheHeapRunsOut() { checkTightHeap("JSON", jsonFeed(200)); }

// A small entry may fit after a large one did not; the kept list must still
// end at the first entry that did not fit.
void testMixedEntriesKeepAPrefix() {
  checkTightHeap("Atom mixed", atomFeed(200, everyOtherSmall));
  checkTightHeap("JSON mixed", jsonFeed(200, everyOtherSmall));
}

// Facet links precede the entries in an Atom feed; they must leave budget for
// the entries that follow.
void testFacetsDoNotCrowdOutEntries() {
  constexpr size_t heapLeft = 28 * 1024;  // about 11 KB of budget
  const ParseResult r = parse(atomFeed(20, allLarge, OpdsLimits::MAX_FACET_ENTRIES), heapLeft);
  CHECK(!r.threw);
  CHECK(!r.error);
  CHECK(!r.entries.empty());
  CHECK(keptAPrefix(r.entries));
}

// Receive buffers come and go while the feed streams. The parser budgets from
// the heap it saw at start, so they change neither what it keeps nor its safety.
void testNetworkBuffersDoNotChangeTheResult() {
  for (const std::string& feed : {atomFeed(200, everyOtherSmall), jsonFeed(200, everyOtherSmall)}) {
    const ParseResult quiet = parse(feed, 48 * 1024);
    const ParseResult busy = parse(feed, 48 * 1024, 13 * 1024);
    CHECK(!busy.threw);
    CHECK(busy.entries.size() == quiet.entries.size());
    CHECK(keptAPrefix(busy.entries));
  }
}

// Small entries are limited by the budget, not by the storage reserved up front.
void testSmallEntriesGoPastTheFirstStorage() {
  for (const std::string& feed : {atomFeed(60, allSmall), jsonFeed(60, allSmall)}) {
    const ParseResult r = parse(feed, 48 * 1024);
    CHECK(!r.threw);
    CHECK(r.entries.size() > 32);
    CHECK(keptAPrefix(r.entries));
    std::printf("60 small entries, 48 KB heap: %zu kept\n", r.entries.size());
  }
}

// A network burst beyond the reserve pushes free heap under MIN_FREE_HEAP; the
// parsers stop keeping rows there instead of allocating into it.
void testABurstBelowTheFloorStopsTheRows() {
  for (const std::string& feed : {atomFeed(200), jsonFeed(200)}) {
    const ParseResult r = parse(feed, 48 * 1024, 25 * 1024);
    CHECK(!r.threw);
    CHECK(r.truncated);
    CHECK(keptAPrefix(r.entries));
    std::printf("25 KB burst, 48 KB heap: %zu kept, lowest free %zu bytes\n", r.entries.size(), r.lowestFree);
  }
}

void testHeapBelowTheFloorIsAnError() {
  for (const std::string& feed : {atomFeed(10), jsonFeed(10)}) {
    const ParseResult r = parse(feed, OpdsRowBudget::MIN_RESERVE - 1024);
    CHECK(!r.threw);
    CHECK(r.error);
  }
}

void testAmpleHeapKeepsEveryEntry() {
  for (const std::string& feed : {atomFeed(10), jsonFeed(10)}) {
    const ParseResult r = parse(feed, UNLIMITED);
    CHECK(!r.threw);
    CHECK(!r.truncated);
    CHECK(r.entries.size() == 10);
    if (r.entries.size() != 10) continue;
    CHECK(r.entries[3].title == "Book 3");
    CHECK(r.entries[3].author == "Author 3");
    CHECK(r.entries[3].href.rfind("https://example.org/3.epub?token=", 0) == 0);
    CHECK(r.entries[3].description.size() <= OpdsLimits::MAX_DESCRIPTION_CHARS);
    CHECK(!r.entries[3].description.empty());
  }
}

void testAmpleHeapStillStopsAtTheEntryCap() {
  for (const std::string& feed : {atomFeed(100), jsonFeed(100)}) {
    const ParseResult r = parse(feed, UNLIMITED);
    CHECK(!r.threw);
    CHECK(r.truncated);
    CHECK(r.entries.size() == OpdsLimits::MAX_ENTRIES);
  }
}

}  // namespace

int main() {
  testAtomFeedStopsBeforeTheHeapRunsOut();
  testJsonFeedStopsBeforeTheHeapRunsOut();
  testMixedEntriesKeepAPrefix();
  testFacetsDoNotCrowdOutEntries();
  testNetworkBuffersDoNotChangeTheResult();
  testSmallEntriesGoPastTheFirstStorage();
  testABurstBelowTheFloorStopsTheRows();
  testHeapBelowTheFloorIsAnError();
  testAmpleHeapKeepsEveryEntry();
  testAmpleHeapStillStopsAtTheEntryCap();
  std::printf("%d checks, %d failed\n", checksRun, checksFailed);
  return checksFailed == 0 ? 0 : 1;
}
