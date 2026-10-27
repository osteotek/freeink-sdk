#pragma once

#include <cstddef>
#include <cstdint>

// Host stand-in for Arduino's Print: the parsers derive from it and use nothing else.
class Print {
 public:
  virtual ~Print() = default;
  virtual size_t write(uint8_t) = 0;
  virtual size_t write(const uint8_t*, size_t) = 0;
  virtual void flush() {}
};
