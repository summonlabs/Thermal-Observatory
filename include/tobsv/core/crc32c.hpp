// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace tobsv {

namespace detail {

// CRC-32C (Castagnoli, reflected polynomial 0x82F63B78). Built at compile time as an inline
// variable so that no runtime initialisation guard is needed on the hot path.
struct Crc32cTable {
  std::uint32_t entries[256]{};
  constexpr Crc32cTable() {
    for (std::uint32_t index = 0; index < 256U; ++index) {
      std::uint32_t crc = index;
      for (int bit = 0; bit < 8; ++bit) {
        crc = (crc & 1U) != 0U ? ((crc >> 1U) ^ 0x82F63B78U) : (crc >> 1U);
      }
      entries[index] = crc;
    }
  }
};

inline constexpr Crc32cTable kCrc32cTable{};

}  // namespace detail

// CRC-32C is an integrity check against corruption, not a defence against tampering: an actor who
// can rewrite a payload can rewrite its checksum. It is used with that limitation stated wherever
// integrity is claimed.
class Crc32c {
 public:
  void update(const std::uint8_t* data, std::size_t length) {
    std::uint32_t state = state_;
    for (std::size_t index = 0; index < length; ++index) {
      state = (state >> 8U) ^ detail::kCrc32cTable.entries[(state ^ data[index]) & 0xFFU];
    }
    state_ = state;
  }

  void update(std::string_view text) {
    update(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
  }

  void update_u8(std::uint8_t value) { update(&value, 1); }

  void update_u32(std::uint32_t value) {
    const std::uint8_t bytes[4] = {
        static_cast<std::uint8_t>(value & 0xFFU),
        static_cast<std::uint8_t>((value >> 8U) & 0xFFU),
        static_cast<std::uint8_t>((value >> 16U) & 0xFFU),
        static_cast<std::uint8_t>((value >> 24U) & 0xFFU),
    };
    update(bytes, 4);
  }

  void update_u64(std::uint64_t value) {
    update_u32(static_cast<std::uint32_t>(value & 0xFFFFFFFFULL));
    update_u32(static_cast<std::uint32_t>((value >> 32U) & 0xFFFFFFFFULL));
  }

  std::uint32_t value() const noexcept { return state_ ^ 0xFFFFFFFFU; }

  static std::uint32_t compute(std::string_view text) {
    Crc32c crc;
    crc.update(text);
    return crc.value();
  }

 private:
  std::uint32_t state_ = 0xFFFFFFFFU;
};

inline std::uint32_t crc32c(std::string_view text) { return Crc32c::compute(text); }

}  // namespace tobsv
