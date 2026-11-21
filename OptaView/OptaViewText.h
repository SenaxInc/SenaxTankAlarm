/**
 * OptaViewText.h
 *
 * Text output and number formatting for OptaView. No Arduino dependency.
 *
 * Numbers with a fraction are formatted here, digit by digit, instead of with printf("%f"):
 * the result is then the same on the Opta and in the host tests, whatever printf the
 * toolchain links.
 */

#ifndef OPTAVIEW_TEXT_H
#define OPTAVIEW_TEXT_H

#include <ctype.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

namespace optaview {

// Copies src into dst (capacity cap, always NUL-terminated). Returns the length written.
inline size_t ovCopy(char *dst, size_t cap, const char *src) {
  if (cap == 0) {
    return 0;
  }
  size_t i = 0;
  if (src != nullptr) {
    for (; src[i] != '\0' && i + 1 < cap; ++i) {
      dst[i] = src[i];
    }
  }
  dst[i] = '\0';
  return i;
}

inline bool ovEqualsIgnoreCase(const char *a, const char *b) {
  if (a == nullptr || b == nullptr) {
    return false;
  }
  for (;; ++a, ++b) {
    const int ca = tolower((unsigned char)*a);
    const int cb = tolower((unsigned char)*b);
    if (ca != cb) {
      return false;
    }
    if (ca == 0) {
      return true;
    }
  }
}

// Strict unsigned parse: decimal, or hex with a 0x prefix. The whole string must be a number.
inline bool ovParseU32(const char *text, uint32_t *out) {
  if (text == nullptr || *text == '\0') {
    return false;
  }
  int base = 10;
  const char *p = text;
  if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
    base = 16;
    p += 2;
    if (*p == '\0') {
      return false;
    }
  }
  uint32_t value = 0;
  for (; *p != '\0'; ++p) {
    uint32_t digit;
    const char c = *p;
    if (c >= '0' && c <= '9') {
      digit = (uint32_t)(c - '0');
    } else if (base == 16 && c >= 'a' && c <= 'f') {
      digit = (uint32_t)(c - 'a') + 10;
    } else if (base == 16 && c >= 'A' && c <= 'F') {
      digit = (uint32_t)(c - 'A') + 10;
    } else {
      return false;
    }
    if (value > (0xFFFFFFFFUL - digit) / (uint32_t)base) {
      return false;  // overflow
    }
    value = value * (uint32_t)base + digit;
  }
  *out = value;
  return true;
}

// Strict decimal parse ("13.10", "-5", ".5"). No exponent, no hex, no trailing text, at most 15
// digits. The result is the double nearest to the decimal as written: the digits are collected
// as a whole number and divided once by a power of ten, and both are exact in a double. So the
// same text always gives the same number, typed at the console or read from a profile.
inline bool ovParseDecimal(const char *text, double *out) {
  if (text == nullptr || *text == '\0') {
    return false;
  }
  const char *p = text;
  bool negative = false;
  if (*p == '-' || *p == '+') {
    negative = (*p == '-');
    ++p;
  }
  unsigned long long mantissa = 0ULL;
  int digits = 0;
  int decimals = 0;
  for (; *p >= '0' && *p <= '9'; ++p, ++digits) {
    if (digits < 15) {
      mantissa = mantissa * 10ULL + (unsigned long long)(*p - '0');
    }
  }
  if (*p == '.') {
    ++p;
    for (; *p >= '0' && *p <= '9'; ++p, ++digits, ++decimals) {
      if (digits < 15) {
        mantissa = mantissa * 10ULL + (unsigned long long)(*p - '0');
      }
    }
  }
  if (*p != '\0' || digits == 0 || digits > 15) {
    return false;
  }
  double divisor = 1.0;
  for (int i = 0; i < decimals; ++i) {
    divisor *= 10.0;
  }
  const double value = (double)mantissa / divisor;
  *out = negative ? -value : value;
  return true;
}

inline size_t ovFormatU64(unsigned long long value, char *out, size_t cap) {
  char digits[24];
  size_t n = 0;
  do {
    digits[n++] = (char)('0' + (int)(value % 10ULL));
    value /= 10ULL;
  } while (value != 0ULL && n < sizeof(digits));
  size_t written = 0;
  if (cap == 0) {
    return 0;
  }
  while (n > 0 && written + 1 < cap) {
    out[written++] = digits[--n];
  }
  out[written] = '\0';
  return written;
}

inline size_t ovFormatHex(uint32_t value, uint8_t digits, char *out, size_t cap) {
  static const char kHex[] = "0123456789ABCDEF";
  if (cap == 0) {
    return 0;
  }
  if (digits < 1) {
    digits = 1;
  }
  if (digits > 8) {
    digits = 8;
  }
  size_t written = 0;
  for (int shift = (int)(digits - 1) * 4; shift >= 0 && written + 1 < cap; shift -= 4) {
    out[written++] = kHex[(value >> shift) & 0x0F];
  }
  out[written] = '\0';
  return written;
}

// value rounded to `decimals` places (0..6), half away from zero. Never prints "-0".
inline size_t ovFormatFixed(double value, uint8_t decimals, char *out, size_t cap) {
  if (cap == 0) {
    return 0;
  }
  if (value != value) {
    return ovCopy(out, cap, "nan");
  }
  bool negative = value < 0.0;
  if (negative) {
    value = -value;
  }
  if (decimals > 6) {
    decimals = 6;
  }
  unsigned long long divisor = 1ULL;
  for (uint8_t i = 0; i < decimals; ++i) {
    divisor *= 10ULL;
  }
  const double scaled = value * (double)divisor + 0.5;
  if (scaled >= 1.0e18) {
    return ovCopy(out, cap, negative ? "-big" : "big");
  }
  const unsigned long long whole = (unsigned long long)scaled;
  if (whole == 0ULL) {
    negative = false;
  }
  size_t written = 0;
  if (negative && written + 1 < cap) {
    out[written++] = '-';
  }
  written += ovFormatU64(whole / divisor, out + written, cap - written);
  if (decimals > 0 && written + 1 < cap) {
    out[written++] = '.';
    unsigned long long fraction = whole % divisor;
    unsigned long long place = divisor / 10ULL;
    for (uint8_t i = 0; i < decimals && written + 1 < cap; ++i) {
      out[written++] = (char)('0' + (int)(fraction / place));
      fraction %= place;
      place = place > 1ULL ? place / 10ULL : 1ULL;
    }
  }
  out[written] = '\0';
  return written;
}

// A bounded string builder. Text that does not fit is dropped and overflowed() turns true.
class OvText {
 public:
  OvText(char *buffer, size_t cap) : buffer_(buffer), cap_(cap), len_(0), overflowed_(false) {
    if (cap_ > 0) {
      buffer_[0] = '\0';
    }
  }
  void add(const char *text) {
    if (text == nullptr) {
      return;
    }
    for (; *text != '\0'; ++text) {
      addChar(*text);
    }
  }
  void addChar(char c) {
    if (len_ + 1 < cap_) {
      buffer_[len_++] = c;
      buffer_[len_] = '\0';
    } else {
      overflowed_ = true;
    }
  }
  void addU(uint32_t value) {
    char tmp[24];
    ovFormatU64(value, tmp, sizeof(tmp));
    add(tmp);
  }
  void addI(int32_t value) {
    if (value < 0) {
      addChar('-');
      addU((uint32_t)(-(int64_t)value));
    } else {
      addU((uint32_t)value);
    }
  }
  void addHex(uint32_t value, uint8_t digits) {
    char tmp[12];
    ovFormatHex(value, digits, tmp, sizeof(tmp));
    add(tmp);
  }
  void addFixed(double value, uint8_t decimals) {
    char tmp[40];
    ovFormatFixed(value, decimals, tmp, sizeof(tmp));
    add(tmp);
  }
  size_t length() const { return len_; }
  bool overflowed() const { return overflowed_; }
  const char *c_str() const { return cap_ > 0 ? buffer_ : ""; }

 private:
  char *buffer_;
  size_t cap_;
  size_t len_;
  bool overflowed_;
};

// Where console text goes: Serial on the Opta, a string in the tests.
class OvOut {
 public:
  virtual ~OvOut() {}
  virtual void write(const char *text, size_t len) = 0;

  void print(const char *text) {
    if (text != nullptr) {
      write(text, strlen(text));
    }
  }
  void printChar(char c) { write(&c, 1); }
  void println() { write("\r\n", 2); }
  void println(const char *text) {
    print(text);
    println();
  }
  void printU(uint32_t value) {
    char tmp[24];
    write(tmp, ovFormatU64(value, tmp, sizeof(tmp)));
  }
  void printI(int32_t value) {
    if (value < 0) {
      printChar('-');
      printU((uint32_t)(-(int64_t)value));
    } else {
      printU((uint32_t)value);
    }
  }
  // Hex digits only; the caller prints "0x" where it wants one.
  void printHex(uint32_t value, uint8_t digits) {
    char tmp[12];
    write(tmp, ovFormatHex(value, digits, tmp, sizeof(tmp)));
  }
  void printFixed(double value, uint8_t decimals) {
    char tmp[40];
    write(tmp, ovFormatFixed(value, decimals, tmp, sizeof(tmp)));
  }
  void spaces(size_t count) {
    static const char kSpaces[] = "                ";
    while (count > 0) {
      const size_t n = count < sizeof(kSpaces) - 1 ? count : sizeof(kSpaces) - 1;
      write(kSpaces, n);
      count -= n;
    }
  }
  // text, then spaces up to `width` columns (always at least one space).
  void printPadded(const char *text, size_t width) {
    const size_t len = text != nullptr ? strlen(text) : 0;
    print(text);
    spaces(len < width ? width - len : 1);
  }
  void printHexBytes(const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
      if (i > 0) {
        printChar(' ');
      }
      printHex(data[i], 2);
    }
  }
};

}  // namespace optaview

#endif  // OPTAVIEW_TEXT_H
