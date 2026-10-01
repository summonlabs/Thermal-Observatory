// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "tobsv/core/limits.hpp"
#include "tobsv/core/result.hpp"

namespace tobsv {

// A small, strict, canonical JSON value. It exists for three reasons:
//   * durable records are stored as one canonical JSON document per line, so the encoding has to be
//     byte-stable across compilers and standard libraries;
//   * the CLI emits machine-readable explanations;
//   * malformed input has to be rejected precisely rather than repaired.
//
// The parser is deliberately unforgiving: no comments, no trailing commas, no NaN/Infinity
// literals, no duplicate object keys, no leading zeros, no lone surrogates, no invalid UTF-8, no
// trailing content, and hard bounds on depth, node count and byte count.
class JsonValue {
 public:
  enum class Kind : std::uint8_t { kNull = 0, kBool, kInteger, kReal, kString, kArray, kObject };

  struct Number {
    bool is_integer = false;
    std::int64_t integer = 0;
    double real = 0.0;
  };

  using Array = std::vector<JsonValue>;
  using Object = std::map<std::string, JsonValue, std::less<>>;

  JsonValue() = default;
  explicit JsonValue(bool value) : kind_(Kind::kBool), boolean_(value) {}
  explicit JsonValue(std::string value) : kind_(Kind::kString), text_(std::move(value)) {}
  explicit JsonValue(const char* value) : kind_(Kind::kString), text_(value) {}
  explicit JsonValue(Array value) : kind_(Kind::kArray), array_(std::move(value)) {}
  explicit JsonValue(Object value) : kind_(Kind::kObject), object_(std::move(value)) {}

  static JsonValue null() { return JsonValue{}; }
  static JsonValue boolean(bool value) { return JsonValue(value); }
  static JsonValue text(std::string value) { return JsonValue(std::move(value)); }
  static JsonValue text(const char* value) { return JsonValue(value); }
  static JsonValue integer(std::int64_t value);
  static JsonValue real(double value);
  static JsonValue array() { return JsonValue(Array{}); }
  static JsonValue object() { return JsonValue(Object{}); }

  Kind kind() const noexcept { return kind_; }
  bool is_null() const noexcept { return kind_ == Kind::kNull; }
  bool is_bool() const noexcept { return kind_ == Kind::kBool; }
  bool is_number() const noexcept { return kind_ == Kind::kInteger || kind_ == Kind::kReal; }
  bool is_string() const noexcept { return kind_ == Kind::kString; }
  bool is_array() const noexcept { return kind_ == Kind::kArray; }
  bool is_object() const noexcept { return kind_ == Kind::kObject; }

  bool as_bool() const noexcept { return boolean_; }
  const Number& number() const noexcept { return number_; }
  const std::string& as_string() const noexcept { return text_; }
  const Array& items() const noexcept { return array_; }
  Array& items() noexcept { return array_; }
  const Object& fields() const noexcept { return object_; }
  Object& fields() noexcept { return object_; }

  // Mutation. Arrays append; objects overwrite an existing key. Both return *this so that a
  // document can be built in a single expression.
  JsonValue& push(JsonValue value);
  JsonValue& set(std::string key, JsonValue value);

  // Field accessors. Every one returns a Status-bearing result instead of throwing, and a missing
  // or wrongly typed field is an explicit failure rather than a default.
  const JsonValue* find(std::string_view key) const;
  Result<std::string> require_string(std::string_view key) const;
  Result<std::int64_t> require_integer(std::string_view key) const;
  Result<bool> require_bool(std::string_view key) const;
  Result<double> require_real(std::string_view key) const;

  // Canonical single-line encoding with lexicographically ordered object keys and no insignificant
  // whitespace. This is the form written to durable records.
  Result<std::string> dump_compact() const;
  // The same encoding, indented for people. Key order is identical, so the two forms never
  // disagree about content.
  Result<std::string> dump_indented() const;

  static Result<JsonValue> parse(std::string_view text, const Limits& limits);

 private:
  static Result<void> write(const JsonValue& value, std::string& out, bool indented, int depth,
                            const Limits& limits);

  Kind kind_ = Kind::kNull;
  bool boolean_ = false;
  Number number_{};
  std::string text_;
  Array array_;
  Object object_;
};

// True when the byte sequence is well-formed UTF-8 (no overlong forms, no surrogates, no values
// above U+10FFFF). Used by both the parser and the record writer.
bool is_valid_utf8(std::string_view text);

}  // namespace tobsv
