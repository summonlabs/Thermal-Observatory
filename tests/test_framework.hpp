// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <functional>
#include <sstream>
#include <string>
#include <vector>

#include "tobsv/core/result.hpp"

namespace tobstest {

// A failing assertion has to print the whole reason, whether the expression produced a Status or a
// Result. These helpers give both shapes one spelling.
inline std::string describe_failure(const tobsv::Status& status) { return status.describe(); }
template <class T>
std::string describe_failure(const tobsv::Result<T>& result) {
  return result.error().describe();
}
inline tobsv::ErrorCode status_code(const tobsv::Status& status) { return status.code(); }
template <class T>
tobsv::ErrorCode status_code(const tobsv::Result<T>& result) {
  return result.error().code();
}
inline bool status_ok(const tobsv::Status& status) { return status.ok(); }
template <class T>
bool status_ok(const tobsv::Result<T>& result) {
  return result.ok();
}

struct AssertionFailure {
  std::string message;
};

struct TestCase {
  std::string suite;
  std::string name;
  std::function<void()> fn;
};

std::vector<TestCase>& registry();

struct Registrar {
  Registrar(const char* suite, const char* name, std::function<void()> fn) {
    registry().push_back(TestCase{suite, name, std::move(fn)});
  }
};

// Runs every test in the named suite, or every test when the filter is empty. Returns the number of
// failures. There is no timeout anywhere in this framework: a hang is a defect, not a result.
int run_all(const std::string& suite_filter, bool list_only);

}  // namespace tobstest

#define TOBSV_CONCAT_INNER(a, b) a##b
#define TOBSV_CONCAT(a, b) TOBSV_CONCAT_INNER(a, b)

#define TOBSV_TEST(SUITE, NAME)                                                          \
  static void TOBSV_CONCAT(tobsv_test_, __LINE__)();                                     \
  static ::tobstest::Registrar TOBSV_CONCAT(tobsv_reg_, __LINE__)(                       \
      SUITE, NAME, TOBSV_CONCAT(tobsv_test_, __LINE__));                                 \
  static void TOBSV_CONCAT(tobsv_test_, __LINE__)()

#define TOBSV_ASSERT_TRUE(COND)                                                          \
  do {                                                                                   \
    if (!(COND)) {                                                                       \
      std::ostringstream tobsv_oss_;                                                     \
      tobsv_oss_ << __FILE__ << ":" << __LINE__ << ": expected true: " << #COND;         \
      throw ::tobstest::AssertionFailure{tobsv_oss_.str()};                              \
    }                                                                                    \
  } while (0)

#define TOBSV_ASSERT_FALSE(COND)                                                         \
  do {                                                                                   \
    if ((COND)) {                                                                        \
      std::ostringstream tobsv_oss_;                                                     \
      tobsv_oss_ << __FILE__ << ":" << __LINE__ << ": expected false: " << #COND;        \
      throw ::tobstest::AssertionFailure{tobsv_oss_.str()};                              \
    }                                                                                    \
  } while (0)

#define TOBSV_ASSERT_EQ(A, B)                                                            \
  do {                                                                                   \
    auto tobsv_a_ = (A);                                                                 \
    auto tobsv_b_ = (B);                                                                 \
    if (!(tobsv_a_ == tobsv_b_)) {                                                       \
      std::ostringstream tobsv_oss_;                                                     \
      tobsv_oss_ << __FILE__ << ":" << __LINE__ << ": expected equality, left=" << tobsv_a_ \
                 << " right=" << tobsv_b_;                                               \
      throw ::tobstest::AssertionFailure{tobsv_oss_.str()};                              \
    }                                                                                    \
  } while (0)

#define TOBSV_ASSERT_NEAR(A, B, EPSILON)                                                 \
  do {                                                                                   \
    const double tobsv_a_ = static_cast<double>(A);                                      \
    const double tobsv_b_ = static_cast<double>(B);                                      \
    const double tobsv_e_ = static_cast<double>(EPSILON);                                \
    const double tobsv_d_ = tobsv_a_ > tobsv_b_ ? tobsv_a_ - tobsv_b_ : tobsv_b_ - tobsv_a_; \
    if (!(tobsv_d_ <= tobsv_e_)) {                                                       \
      std::ostringstream tobsv_oss_;                                                     \
      tobsv_oss_ << __FILE__ << ":" << __LINE__ << ": expected " << tobsv_a_             \
                 << " within " << tobsv_e_ << " of " << tobsv_b_;                        \
      throw ::tobstest::AssertionFailure{tobsv_oss_.str()};                              \
    }                                                                                    \
  } while (0)

#define TOBSV_ASSERT_THROWS(EXPR, EXTYPE)                                                \
  do {                                                                                   \
    bool tobsv_caught_ = false;                                                          \
    try {                                                                                \
      (void)(EXPR);                                                                      \
    } catch (const EXTYPE&) {                                                            \
      tobsv_caught_ = true;                                                              \
    } catch (...) {                                                                      \
      std::ostringstream tobsv_oss_;                                                     \
      tobsv_oss_ << __FILE__ << ":" << __LINE__                                          \
                 << ": wrong exception type for " << #EXPR;                              \
      throw ::tobstest::AssertionFailure{tobsv_oss_.str()};                              \
    }                                                                                    \
    if (!tobsv_caught_) {                                                                \
      std::ostringstream tobsv_oss_;                                                     \
      tobsv_oss_ << __FILE__ << ":" << __LINE__ << ": expected " << #EXTYPE              \
                 << " from " << #EXPR;                                                   \
      throw ::tobstest::AssertionFailure{tobsv_oss_.str()};                              \
    }                                                                                    \
  } while (0)

// Status-aware assertions. A failure always prints the full reason so that a failing test explains
// itself without a debugger.
#define TOBSV_ASSERT_OK(EXPR)                                                            \
  do {                                                                                   \
    const auto tobsv_s_ = (EXPR);                                                        \
    if (!::tobstest::status_ok(tobsv_s_)) {                                              \
      std::ostringstream tobsv_oss_;                                                     \
      tobsv_oss_ << __FILE__ << ":" << __LINE__ << ": expected success from " << #EXPR    \
                 << " but saw " << ::tobstest::describe_failure(tobsv_s_);               \
      throw ::tobstest::AssertionFailure{tobsv_oss_.str()};                              \
    }                                                                                    \
  } while (0)

#define TOBSV_ASSERT_FAILS_WITH(EXPR, CODE)                                              \
  do {                                                                                   \
    const auto tobsv_s_ = (EXPR);                                                        \
    if (::tobstest::status_ok(tobsv_s_)) {                                               \
      std::ostringstream tobsv_oss_;                                                     \
      tobsv_oss_ << __FILE__ << ":" << __LINE__ << ": expected failure " << #CODE        \
                 << " from " << #EXPR << " but it succeeded";                           \
      throw ::tobstest::AssertionFailure{tobsv_oss_.str()};                              \
    }                                                                                    \
    if (::tobstest::status_code(tobsv_s_) != (CODE)) {                                   \
      std::ostringstream tobsv_oss_;                                                     \
      tobsv_oss_ << __FILE__ << ":" << __LINE__ << ": expected " << #CODE << " from "    \
                 << #EXPR << " but saw " << ::tobstest::describe_failure(tobsv_s_);      \
      throw ::tobstest::AssertionFailure{tobsv_oss_.str()};                              \
    }                                                                                    \
  } while (0)