// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <bit>
#include <cstdint>
#include <string>
#include <string_view>

namespace tobsv {

// A stable, non-cryptographic digest used for identity derivation and for deterministic ordering.
// FNV-1a is chosen because its output is byte-for-byte reproducible across compilers, platforms and
// standard library versions, which is a requirement for durable identity in this runtime. It is an
// integrity aid, never an authenticity claim.
//
// Every absorb_* overload is deliberately named rather than overloaded: an overload set containing
// both std::string_view and bool would silently route a string literal to the boolean overload.
class StableDigest {
 public:
  static constexpr std::uint64_t kOffsetBasis = 14695981039346656037ULL;
  static constexpr std::uint64_t kPrime = 1099511628211ULL;

  constexpr StableDigest() = default;

  constexpr void absorb_byte(std::uint8_t byte) {
    value_ = (value_ ^ static_cast<std::uint64_t>(byte)) * kPrime;
  }

  constexpr void absorb_text(std::string_view text) {
    for (const char ch : text) {
      absorb_byte(static_cast<std::uint8_t>(static_cast<unsigned char>(ch)));
    }
    // A terminator keeps "ab"+"c" distinct from "a"+"bc".
    absorb_byte(0xFFU);
  }

  constexpr void absorb_number(std::uint64_t number) {
    for (int shift = 0; shift < 64; shift += 8) {
      absorb_byte(static_cast<std::uint8_t>((number >> static_cast<unsigned>(shift)) & 0xFFULL));
    }
    absorb_byte(0xFEU);
  }

  constexpr void absorb_flag(bool flag) { absorb_byte(flag ? 0x01U : 0x00U); }

  // The bit pattern is absorbed verbatim so that -0.0 and 0.0 differ and NaN payloads are visible.
  constexpr void absorb_real(double number) { absorb_number(std::bit_cast<std::uint64_t>(number)); }

  constexpr std::uint64_t value() const noexcept { return value_; }

  std::string hex() const;

 private:
  std::uint64_t value_ = kOffsetBasis;
};

inline std::uint64_t stable_hash(std::string_view text) {
  StableDigest digest;
  digest.absorb_text(text);
  return digest.value();
}

// Combines an existing digest with a new one without collisions on ordering.
constexpr std::uint64_t hash_combine(std::uint64_t seed, std::uint64_t value) {
  std::uint64_t mixed = seed ^ (value + 0x9E3779B97F4A7C15ULL + (seed << 6U) + (seed >> 2U));
  mixed ^= mixed >> 33U;
  mixed *= 0xFF51AFD7ED558CCDULL;
  mixed ^= mixed >> 33U;
  return mixed;
}

std::string hex_u64(std::uint64_t value);

}  // namespace tobsv
