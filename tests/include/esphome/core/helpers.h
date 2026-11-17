#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace esphome {

class StringRef {
 public:
  StringRef() = default;
  StringRef(const char *value) : value_(value != nullptr ? value : "") {}
  StringRef(const std::string &value) : value_(value) {}

  const std::string &str() const { return this->value_; }
  operator const std::string &() const { return this->value_; }

 private:
  std::string value_;
};

uint32_t fnv1_hash(const char *str);

/// Host stand-in for ESPHome's random_uint32(): returns test_random::set()'s value (0 by default),
/// so schedules that draw jitter are deterministic in tests.
uint32_t random_uint32();

/// Host stand-in for ESPHome's HighFrequencyLoopRequester: while any requester is started,
/// is_high_frequency() is true (the real Application then runs the loop without waiting for its
/// interval). start() and stop() are idempotent per requester, as in ESPHome.
class HighFrequencyLoopRequester {
 public:
  void start() {
    if (this->started_)
      return;
    num_requests++;
    this->started_ = true;
  }
  void stop() {
    if (!this->started_)
      return;
    num_requests--;
    this->started_ = false;
  }
  static bool is_high_frequency() { return num_requests > 0; }
  /// Test helper (not in ESPHome): forget every request, e.g. those a destroyed component left behind.
  static void test_reset() { num_requests = 0; }

 protected:
  bool started_{false};
  static inline uint8_t num_requests{0};
};

/// Buffer size format_hex_pretty_to() needs for @p byte_count bytes ("XX:XX:...:XX\0").
constexpr size_t format_hex_pretty_size(size_t byte_count) { return byte_count * 3; }

/// Same output as ESPHome's: uppercase hex pairs joined by @p separator, cut to fit @p buffer.
template<size_t N>
inline char *format_hex_pretty_to(char (&buffer)[N], const uint8_t *data, size_t length, char separator = ':') {
  static constexpr char DIGITS[] = "0123456789ABCDEF";
  size_t pos = 0;
  for (size_t i = 0; i < length && pos + (i > 0 ? 3 : 2) < N; i++) {
    if (i > 0)
      buffer[pos++] = separator;
    buffer[pos++] = DIGITS[data[i] >> 4];
    buffer[pos++] = DIGITS[data[i] & 0x0F];
  }
  buffer[pos] = '\0';
  return buffer;
}

namespace test_random {
/// Make every following random_uint32() call return @p value.
void set(uint32_t value);
}  // namespace test_random

}  // namespace esphome