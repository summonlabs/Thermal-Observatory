// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "tobsv/core/hash.hpp"

namespace tobsv {
namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

}  // namespace

std::string StableDigest::hex() const {
  std::string out;
  out.reserve(16);
  for (int shift = 60; shift >= 0; shift -= 4) {
    out.push_back(kHexDigits[(value_ >> static_cast<unsigned>(shift)) & 0xFULL]);
  }
  return out;
}

std::string hex_u64(std::uint64_t value) {
  std::string out;
  out.reserve(16);
  for (int shift = 60; shift >= 0; shift -= 4) {
    out.push_back(kHexDigits[(value >> static_cast<unsigned>(shift)) & 0xFULL]);
  }
  return out;
}

}  // namespace tobsv
