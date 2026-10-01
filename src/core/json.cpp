// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "tobsv/core/json.hpp"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <utility>

#include "tobsv/core/checked.hpp"
#include "tobsv/core/format.hpp"

namespace tobsv {
namespace {

bool is_digit(char ch) { return ch >= '0' && ch <= '9'; }

unsigned hex_value(char ch) {
  if (ch >= '0' && ch <= '9') return static_cast<unsigned>(ch - '0');
  if (ch >= 'a' && ch <= 'f') return static_cast<unsigned>(ch - 'a' + 10);
  if (ch >= 'A' && ch <= 'F') return static_cast<unsigned>(ch - 'A' + 10);
  return 16U;
}

void append_utf8(std::string& out, std::uint32_t code_point) {
  if (code_point <= 0x7FU) {
    out.push_back(static_cast<char>(code_point));
  } else if (code_point <= 0x7FFU) {
    out.push_back(static_cast<char>(0xC0U | (code_point >> 6U)));
    out.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
  } else if (code_point <= 0xFFFFU) {
    out.push_back(static_cast<char>(0xE0U | (code_point >> 12U)));
    out.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
    out.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
  } else {
    out.push_back(static_cast<char>(0xF0U | (code_point >> 18U)));
    out.push_back(static_cast<char>(0x80U | ((code_point >> 12U) & 0x3FU)));
    out.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
    out.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
  }
}

void escape_into(std::string& out, std::string_view text) {
  out.push_back('"');
  for (const char raw : text) {
    const unsigned char ch = static_cast<unsigned char>(raw);
    switch (ch) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (ch < 0x20U) {
          char buffer[8];
          std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned>(ch));
          out += buffer;
        } else {
          out.push_back(raw);
        }
        break;
    }
  }
  out.push_back('"');
}

class Parser {
 public:
  Parser(std::string_view text, const Limits& limits) : text_(text), limits_(limits) {}

  Result<JsonValue> run() {
    if (text_.size() > limits_.max_json_bytes) {
      return Status::failure(ErrorCode::kLimitExceeded, "json document exceeds the byte budget");
    }
    JsonValue value;
    Status status = parse_value(value, 0);
    if (!status.ok()) {
      return status;
    }
    skip_whitespace();
    if (cursor_ != text_.size()) {
      return failure(ErrorCode::kMalformedInput, "trailing content after the document");
    }
    return value;
  }

 private:
  Status failure(ErrorCode code, const char* what) const {
    return Status::failure(code, std::string(what) + " at byte " + std::to_string(cursor_));
  }

  void skip_whitespace() {
    while (cursor_ < text_.size()) {
      const char ch = text_[cursor_];
      if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
        ++cursor_;
      } else {
        break;
      }
    }
  }

  Status count_node() {
    ++nodes_;
    if (nodes_ > limits_.max_json_nodes) {
      return Status::failure(ErrorCode::kLimitExceeded, "json document exceeds the node budget");
    }
    return Status::success();
  }

  Status parse_value(JsonValue& out, std::size_t depth) {
    if (depth > limits_.max_json_depth) {
      return Status::failure(ErrorCode::kLimitExceeded, "json document exceeds the depth budget");
    }
    Status status = count_node();
    if (!status.ok()) {
      return status;
    }
    skip_whitespace();
    if (cursor_ >= text_.size()) {
      return failure(ErrorCode::kMalformedInput, "unexpected end of document");
    }
    const char ch = text_[cursor_];
    switch (ch) {
      case '{': return parse_object(out, depth);
      case '[': return parse_array(out, depth);
      case '"': {
        std::string text;
        status = parse_string(text);
        if (!status.ok()) {
          return status;
        }
        out = JsonValue::text(std::move(text));
        return Status::success();
      }
      case 't':
        if (text_.compare(cursor_, 4, "true") == 0) {
          cursor_ += 4;
          out = JsonValue::boolean(true);
          return Status::success();
        }
        return failure(ErrorCode::kMalformedInput, "expected 'true'");
      case 'f':
        if (text_.compare(cursor_, 5, "false") == 0) {
          cursor_ += 5;
          out = JsonValue::boolean(false);
          return Status::success();
        }
        return failure(ErrorCode::kMalformedInput, "expected 'false'");
      case 'n':
        if (text_.compare(cursor_, 4, "null") == 0) {
          cursor_ += 4;
          out = JsonValue::null();
          return Status::success();
        }
        return failure(ErrorCode::kMalformedInput, "expected 'null'");
      default:
        if (ch == '-' || is_digit(ch)) {
          return parse_number(out);
        }
        if (ch == 'N' || ch == 'I') {
          return failure(ErrorCode::kMalformedInput,
                         "NaN and Infinity literals are not part of JSON and are not accepted");
        }
        return failure(ErrorCode::kMalformedInput, "unexpected character");
    }
  }

  Status parse_object(JsonValue& out, std::size_t depth) {
    ++cursor_;  // '{'
    JsonValue::Object object;
    skip_whitespace();
    if (cursor_ < text_.size() && text_[cursor_] == '}') {
      ++cursor_;
      out = JsonValue::object();
      out.fields() = std::move(object);
      return Status::success();
    }
    for (;;) {
      skip_whitespace();
      if (cursor_ >= text_.size() || text_[cursor_] != '"') {
        return failure(ErrorCode::kMalformedInput, "expected an object key string");
      }
      std::string key;
      Status status = parse_string(key);
      if (!status.ok()) {
        return status;
      }
      skip_whitespace();
      if (cursor_ >= text_.size() || text_[cursor_] != ':') {
        return failure(ErrorCode::kMalformedInput, "expected ':' after an object key");
      }
      ++cursor_;
      JsonValue member;
      status = parse_value(member, depth + 1);
      if (!status.ok()) {
        return status;
      }
      const auto inserted = object.emplace(std::move(key), std::move(member));
      if (!inserted.second) {
        return failure(ErrorCode::kMalformedInput, "duplicate object key");
      }
      skip_whitespace();
      if (cursor_ >= text_.size()) {
        return failure(ErrorCode::kMalformedInput, "unterminated object");
      }
      if (text_[cursor_] == ',') {
        ++cursor_;
        continue;
      }
      if (text_[cursor_] == '}') {
        ++cursor_;
        break;
      }
      return failure(ErrorCode::kMalformedInput, "expected ',' or '}' in an object");
    }
    out = JsonValue::object();
    out.fields() = std::move(object);
    return Status::success();
  }

  Status parse_array(JsonValue& out, std::size_t depth) {
    ++cursor_;  // '['
    JsonValue::Array array;
    skip_whitespace();
    if (cursor_ < text_.size() && text_[cursor_] == ']') {
      ++cursor_;
      out = JsonValue::array();
      out.items() = std::move(array);
      return Status::success();
    }
    for (;;) {
      JsonValue element;
      const Status status = parse_value(element, depth + 1);
      if (!status.ok()) {
        return status;
      }
      array.push_back(std::move(element));
      skip_whitespace();
      if (cursor_ >= text_.size()) {
        return failure(ErrorCode::kMalformedInput, "unterminated array");
      }
      if (text_[cursor_] == ',') {
        ++cursor_;
        continue;
      }
      if (text_[cursor_] == ']') {
        ++cursor_;
        break;
      }
      return failure(ErrorCode::kMalformedInput, "expected ',' or ']' in an array");
    }
    out = JsonValue::array();
    out.items() = std::move(array);
    return Status::success();
  }

  Status parse_string(std::string& out) {
    ++cursor_;  // opening quote
    std::string value;
    for (;;) {
      if (cursor_ >= text_.size()) {
        return failure(ErrorCode::kMalformedInput, "unterminated string");
      }
      const unsigned char ch = static_cast<unsigned char>(text_[cursor_]);
      if (ch == '"') {
        ++cursor_;
        break;
      }
      if (ch < 0x20U) {
        return failure(ErrorCode::kMalformedInput, "unescaped control character in a string");
      }
      if (ch == '\\') {
        ++cursor_;
        if (cursor_ >= text_.size()) {
          return failure(ErrorCode::kMalformedInput, "unterminated escape");
        }
        const char escape = text_[cursor_];
        switch (escape) {
          case '"': value.push_back('"'); ++cursor_; break;
          case '\\': value.push_back('\\'); ++cursor_; break;
          case '/': value.push_back('/'); ++cursor_; break;
          case 'b': value.push_back('\b'); ++cursor_; break;
          case 'f': value.push_back('\f'); ++cursor_; break;
          case 'n': value.push_back('\n'); ++cursor_; break;
          case 'r': value.push_back('\r'); ++cursor_; break;
          case 't': value.push_back('\t'); ++cursor_; break;
          case 'u': {
            ++cursor_;
            if (cursor_ + 4 > text_.size()) {
              return failure(ErrorCode::kMalformedInput, "truncated \\u escape");
            }
            std::uint32_t code_point = 0;
            for (int index = 0; index < 4; ++index) {
              const unsigned digit = hex_value(text_[cursor_ + static_cast<std::size_t>(index)]);
              if (digit > 15U) {
                return failure(ErrorCode::kMalformedInput, "invalid hexadecimal digit in \\u");
              }
              code_point = (code_point << 4U) | digit;
            }
            cursor_ += 4;
            if (code_point >= 0xD800U && code_point <= 0xDBFFU) {
              if (cursor_ + 6 > text_.size() || text_[cursor_] != '\\' || text_[cursor_ + 1] != 'u') {
                return failure(ErrorCode::kMalformedInput, "high surrogate without a low surrogate");
              }
              cursor_ += 2;
              std::uint32_t low = 0;
              for (int index = 0; index < 4; ++index) {
                const unsigned digit = hex_value(text_[cursor_ + static_cast<std::size_t>(index)]);
                if (digit > 15U) {
                  return failure(ErrorCode::kMalformedInput, "invalid hexadecimal digit in \\u");
                }
                low = (low << 4U) | digit;
              }
              cursor_ += 4;
              if (low < 0xDC00U || low > 0xDFFFU) {
                return failure(ErrorCode::kMalformedInput, "invalid low surrogate");
              }
              code_point = 0x10000U + ((code_point - 0xD800U) << 10U) + (low - 0xDC00U);
            } else if (code_point >= 0xDC00U && code_point <= 0xDFFFU) {
              return failure(ErrorCode::kMalformedInput, "lone low surrogate");
            }
            append_utf8(value, code_point);
            break;
          }
          default:
            return failure(ErrorCode::kMalformedInput, "unsupported escape sequence");
        }
        continue;
      }
      // Raw byte: accepted only as part of a well-formed UTF-8 sequence or as plain ASCII. The
      // sequence is consumed atomically so that a malformed continuation byte is caught here.
      std::size_t sequence_length = 1;
      if (ch >= 0x80U) {
        if ((ch & 0xE0U) == 0xC0U) {
          sequence_length = 2;
        } else if ((ch & 0xF0U) == 0xE0U) {
          sequence_length = 3;
        } else if ((ch & 0xF8U) == 0xF0U) {
          sequence_length = 4;
        } else {
          return failure(ErrorCode::kMalformedInput, "invalid UTF-8 lead byte in a string");
        }
        if (cursor_ + sequence_length > text_.size()) {
          return failure(ErrorCode::kMalformedInput, "truncated UTF-8 sequence in a string");
        }
        const std::string_view sequence = text_.substr(cursor_, sequence_length);
        if (!is_valid_utf8(sequence)) {
          return failure(ErrorCode::kMalformedInput, "invalid UTF-8 sequence in a string");
        }
      }
      value.append(text_.substr(cursor_, sequence_length));
      cursor_ += sequence_length;
    }
    out = std::move(value);
    return Status::success();
  }

  Status parse_number(JsonValue& out) {
    const std::size_t start = cursor_;
    if (cursor_ < text_.size() && text_[cursor_] == '-') {
      ++cursor_;
    }
    if (cursor_ >= text_.size() || !is_digit(text_[cursor_])) {
      return failure(ErrorCode::kMalformedInput, "a number requires at least one digit");
    }
    if (text_[cursor_] == '0') {
      ++cursor_;
      if (cursor_ < text_.size() && is_digit(text_[cursor_])) {
        return failure(ErrorCode::kMalformedInput, "a number may not have a leading zero");
      }
    } else {
      while (cursor_ < text_.size() && is_digit(text_[cursor_])) {
        ++cursor_;
      }
    }
    bool integral = true;
    if (cursor_ < text_.size() && text_[cursor_] == '.') {
      integral = false;
      ++cursor_;
      if (cursor_ >= text_.size() || !is_digit(text_[cursor_])) {
        return failure(ErrorCode::kMalformedInput, "a fraction requires at least one digit");
      }
      while (cursor_ < text_.size() && is_digit(text_[cursor_])) {
        ++cursor_;
      }
    }
    if (cursor_ < text_.size() && (text_[cursor_] == 'e' || text_[cursor_] == 'E')) {
      integral = false;
      ++cursor_;
      if (cursor_ < text_.size() && (text_[cursor_] == '+' || text_[cursor_] == '-')) {
        ++cursor_;
      }
      if (cursor_ >= text_.size() || !is_digit(text_[cursor_])) {
        return failure(ErrorCode::kMalformedInput, "an exponent requires at least one digit");
      }
      while (cursor_ < text_.size() && is_digit(text_[cursor_])) {
        ++cursor_;
      }
    }
    const std::string token(text_.substr(start, cursor_ - start));
    // std::from_chars is used rather than strtoll/strtod because it is defined to be independent of
    // the global locale. Under a comma-decimal locale strtod would read "41,5" as 41 and stop, and
    // the payload would be silently truncated instead of refused.
    if (integral) {
      std::int64_t parsed = 0;
      const std::from_chars_result conversion =
          std::from_chars(token.data(), token.data() + token.size(), parsed);
      if (conversion.ec == std::errc{} && conversion.ptr == token.data() + token.size()) {
        out = JsonValue::integer(parsed);
        return Status::success();
      }
    }
    double parsed = 0.0;
    const std::from_chars_result conversion =
        std::from_chars(token.data(), token.data() + token.size(), parsed);
    if (conversion.ec == std::errc::result_out_of_range) {
      return failure(ErrorCode::kOutOfRange, "number is outside the representable range");
    }
    if (conversion.ec != std::errc{} || conversion.ptr != token.data() + token.size() ||
        !std::isfinite(parsed)) {
      return failure(ErrorCode::kMalformedInput, "number is not representable");
    }
    out = JsonValue::real(parsed);
    return Status::success();
  }

  std::string_view text_;
  const Limits& limits_;
  std::size_t cursor_ = 0;
  std::size_t nodes_ = 0;
};

}  // namespace

bool is_valid_utf8(std::string_view text) {
  std::size_t index = 0;
  while (index < text.size()) {
    const unsigned char lead = static_cast<unsigned char>(text[index]);
    std::size_t length = 0;
    std::uint32_t code_point = 0;
    if (lead < 0x80U) {
      ++index;
      continue;
    } else if ((lead & 0xE0U) == 0xC0U) {
      length = 2;
      code_point = lead & 0x1FU;
    } else if ((lead & 0xF0U) == 0xE0U) {
      length = 3;
      code_point = lead & 0x0FU;
    } else if ((lead & 0xF8U) == 0xF0U) {
      length = 4;
      code_point = lead & 0x07U;
    } else {
      return false;
    }
    if (index + length > text.size()) {
      return false;
    }
    for (std::size_t offset = 1; offset < length; ++offset) {
      const unsigned char continuation = static_cast<unsigned char>(text[index + offset]);
      if ((continuation & 0xC0U) != 0x80U) {
        return false;
      }
      code_point = (code_point << 6U) | (continuation & 0x3FU);
    }
    if (length == 2 && code_point < 0x80U) return false;
    if (length == 3 && code_point < 0x800U) return false;
    if (length == 4 && code_point < 0x10000U) return false;
    if (code_point > 0x10FFFFU) return false;
    if (code_point >= 0xD800U && code_point <= 0xDFFFU) return false;
    index += length;
  }
  return true;
}

JsonValue JsonValue::integer(std::int64_t value) {
  JsonValue out;
  out.kind_ = Kind::kInteger;
  out.number_.is_integer = true;
  out.number_.integer = value;
  out.number_.real = static_cast<double>(value);
  return out;
}

JsonValue JsonValue::real(double value) {
  JsonValue out;
  out.kind_ = Kind::kReal;
  out.number_.is_integer = false;
  out.number_.real = value;
  return out;
}

JsonValue& JsonValue::push(JsonValue value) {
  if (kind_ != Kind::kArray) {
    kind_ = Kind::kArray;
    array_.clear();
  }
  array_.push_back(std::move(value));
  return *this;
}

JsonValue& JsonValue::set(std::string key, JsonValue value) {
  if (kind_ != Kind::kObject) {
    kind_ = Kind::kObject;
    object_.clear();
  }
  object_.insert_or_assign(std::move(key), std::move(value));
  return *this;
}

const JsonValue* JsonValue::find(std::string_view key) const {
  if (kind_ != Kind::kObject) {
    return nullptr;
  }
  const auto it = object_.find(key);
  return it == object_.end() ? nullptr : &it->second;
}

Result<std::string> JsonValue::require_string(std::string_view key) const {
  const JsonValue* field = find(key);
  if (field == nullptr) {
    return Status::failure(ErrorCode::kMalformedInput, "missing string field '" + std::string(key) + "'");
  }
  if (!field->is_string()) {
    return Status::failure(ErrorCode::kMalformedInput, "field '" + std::string(key) + "' is not a string");
  }
  return field->as_string();
}

Result<std::int64_t> JsonValue::require_integer(std::string_view key) const {
  const JsonValue* field = find(key);
  if (field == nullptr) {
    return Status::failure(ErrorCode::kMalformedInput,
                           "missing integer field '" + std::string(key) + "'");
  }
  if (!field->is_number()) {
    return Status::failure(ErrorCode::kMalformedInput,
                           "field '" + std::string(key) + "' is not a number");
  }
  const Number& number = field->number();
  if (number.is_integer) {
    return number.integer;
  }
  if (!std::isfinite(number.real) || number.real < -9.2233720368547758e18 ||
      number.real > 9.2233720368547758e18) {
    return Status::failure(ErrorCode::kOutOfRange,
                           "field '" + std::string(key) + "' does not fit in an integer");
  }
  const double truncated = std::trunc(number.real);
  if (truncated != number.real) {
    return Status::failure(ErrorCode::kMalformedInput,
                           "field '" + std::string(key) + "' is not integral");
  }
  return static_cast<std::int64_t>(number.real);
}

Result<bool> JsonValue::require_bool(std::string_view key) const {
  const JsonValue* field = find(key);
  if (field == nullptr) {
    return Status::failure(ErrorCode::kMalformedInput, "missing boolean field '" + std::string(key) + "'");
  }
  if (!field->is_bool()) {
    return Status::failure(ErrorCode::kMalformedInput, "field '" + std::string(key) + "' is not a boolean");
  }
  return field->as_bool();
}

Result<double> JsonValue::require_real(std::string_view key) const {
  const JsonValue* field = find(key);
  if (field == nullptr) {
    return Status::failure(ErrorCode::kMalformedInput, "missing number field '" + std::string(key) + "'");
  }
  if (!field->is_number()) {
    return Status::failure(ErrorCode::kMalformedInput,
                           "field '" + std::string(key) + "' is not a number");
  }
  const Number& number = field->number();
  return number.is_integer ? static_cast<double>(number.integer) : number.real;
}

Result<void> JsonValue::write(const JsonValue& value, std::string& out, bool indented, int depth,
                              const Limits& limits) {
  if (depth > static_cast<int>(limits.max_json_depth)) {
    return Status::failure(ErrorCode::kLimitExceeded, "json value is nested too deeply to encode");
  }
  switch (value.kind_) {
    case Kind::kNull:
      out += "null";
      return Status::success();
    case Kind::kBool:
      out += value.boolean_ ? "true" : "false";
      return Status::success();
    case Kind::kInteger:
      out += std::to_string(value.number_.integer);
      return Status::success();
    case Kind::kReal: {
      if (!std::isfinite(value.number_.real)) {
        return Status::failure(ErrorCode::kIndeterminate,
                               "a non-finite number cannot be encoded as JSON; the value is "
                               "indeterminate rather than representable");
      }
      out += format_real(value.number_.real);
      return Status::success();
    }
    case Kind::kString:
      escape_into(out, value.text_);
      return Status::success();
    case Kind::kArray: {
      if (value.array_.empty()) {
        out += "[]";
        return Status::success();
      }
      out.push_back('[');
      bool first = true;
      for (const JsonValue& element : value.array_) {
        if (!first) {
          out.push_back(',');
        }
        first = false;
        if (indented) {
          out.push_back('\n');
          out.append(static_cast<std::size_t>(depth + 1) * 2U, ' ');
        }
        const Result<void> status = write(element, out, indented, depth + 1, limits);
        if (!status.ok()) {
          return status;
        }
      }
      if (indented) {
        out.push_back('\n');
        out.append(static_cast<std::size_t>(depth) * 2U, ' ');
      }
      out.push_back(']');
      return Status::success();
    }
    case Kind::kObject: {
      if (value.object_.empty()) {
        out += "{}";
        return Status::success();
      }
      out.push_back('{');
      bool first = true;
      for (const auto& entry : value.object_) {
        if (!first) {
          out.push_back(',');
        }
        first = false;
        if (indented) {
          out.push_back('\n');
          out.append(static_cast<std::size_t>(depth + 1) * 2U, ' ');
        }
        escape_into(out, entry.first);
        out.push_back(':');
        if (indented) {
          out.push_back(' ');
        }
        const Result<void> status = write(entry.second, out, indented, depth + 1, limits);
        if (!status.ok()) {
          return status;
        }
      }
      if (indented) {
        out.push_back('\n');
        out.append(static_cast<std::size_t>(depth) * 2U, ' ');
      }
      out.push_back('}');
      return Status::success();
    }
  }
  return Status::failure(ErrorCode::kInternal, "unreachable json kind");
}

Result<std::string> JsonValue::dump_compact() const {
  Limits limits;
  Status status = limits.validate();
  if (!status.ok()) {
    return status;
  }
  std::string out;
  const Result<void> encoded = write(*this, out, false, 0, limits);
  if (!encoded.ok()) {
    return encoded.error();
  }
  return out;
}

Result<std::string> JsonValue::dump_indented() const {
  Limits limits;
  Status status = limits.validate();
  if (!status.ok()) {
    return status;
  }
  std::string out;
  const Result<void> encoded = write(*this, out, true, 0, limits);
  if (!encoded.ok()) {
    return encoded.error();
  }
  return out;
}

Result<JsonValue> JsonValue::parse(std::string_view text, const Limits& limits) {
  Parser parser(text, limits);
  return parser.run();
}

}  // namespace tobsv