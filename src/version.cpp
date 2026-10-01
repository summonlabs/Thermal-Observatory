// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "tobsv/version.hpp"

namespace tobsv {
namespace {

std::string make_version_string() {
  return std::to_string(kVersionMajor) + "." + std::to_string(kVersionMinor) + "." +
         std::to_string(kVersionPatch);
}

}  // namespace

const std::string& product_id() {
  static const std::string value = "thermal-observatory";
  return value;
}

const std::string& version_string() {
  static const std::string value = make_version_string();
  return value;
}

const std::string& build_compiler() {
#if defined(_MSC_VER)
  static const std::string value = "msvc-" + std::to_string(_MSC_VER);
#elif defined(__clang__)
  static const std::string value = std::string("clang-") + __clang_version__;
#elif defined(__GNUC__)
  static const std::string value = "gcc-" + std::to_string(__GNUC__) + "." +
                                   std::to_string(__GNUC_MINOR__) + "." +
                                   std::to_string(__GNUC_PATCHLEVEL__);
#else
  static const std::string value = "unknown";
#endif
  return value;
}

const std::string& build_configuration() {
#if defined(NDEBUG)
  static const std::string value = "release";
#else
  static const std::string value = "debug";
#endif
  return value;
}

}  // namespace tobsv
