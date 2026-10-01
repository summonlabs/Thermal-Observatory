// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "test_framework.hpp"

#include <cstdio>
#include <cstring>
#include <string>

namespace tobstest {

std::vector<TestCase>& registry() {
  static std::vector<TestCase> cases;
  return cases;
}

int run_all(const std::string& suite_filter, bool list_only) {
  int failures = 0;
  int executed = 0;
  for (const TestCase& test : registry()) {
    if (!suite_filter.empty() && test.suite != suite_filter) {
      continue;
    }
    if (list_only) {
      std::printf("%s.%s\n", test.suite.c_str(), test.name.c_str());
      continue;
    }
    ++executed;
    try {
      test.fn();
    } catch (const AssertionFailure& failure) {
      ++failures;
      std::printf("FAIL %s.%s\n  %s\n", test.suite.c_str(), test.name.c_str(),
                  failure.message.c_str());
      continue;
    } catch (const std::exception& error) {
      ++failures;
      std::printf("FAIL %s.%s\n  unexpected exception: %s\n", test.suite.c_str(),
                  test.name.c_str(), error.what());
      continue;
    } catch (...) {
      ++failures;
      std::printf("FAIL %s.%s\n  unexpected non-standard exception\n", test.suite.c_str(),
                  test.name.c_str());
      continue;
    }
    std::printf("ok   %s.%s\n", test.suite.c_str(), test.name.c_str());
  }
  if (!list_only) {
    std::printf("%d test(s) executed, %d failure(s)\n", executed, failures);
  }
  return failures;
}

}  // namespace tobstest

int main(int argc, char** argv) {
  std::string suite;
  bool list_only = false;
  for (int index = 1; index < argc; ++index) {
    const std::string flag = argv[index];
    if (flag == "--suite" && index + 1 < argc) {
      suite = argv[++index];
    } else if (flag == "--list") {
      list_only = true;
    } else {
      std::printf("usage: tobsv_tests [--suite <name>] [--list]\n");
      return 2;
    }
  }
  return tobstest::run_all(suite, list_only) == 0 ? 0 : 1;
}
