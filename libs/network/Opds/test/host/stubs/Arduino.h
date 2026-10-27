#pragma once

#include <cstdint>

#include "Print.h"

// Host stand-in for the ESP heap queries the parsers make. The test drives the
// values from a budgeted operator new (see test_opds_parsers.cpp).
class EspClass {
 public:
  uint32_t getFreeHeap() const;
  uint32_t getMaxAllocHeap() const;
};
extern EspClass ESP;
