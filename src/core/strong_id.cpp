// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "tobsv/core/strong_id.hpp"

namespace tobsv {
namespace {

bool is_ascii_alphanumeric(char ch) {
  return (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
}

bool is_allowed_continuation(char ch) {
  switch (ch) {
    case '.': case '_': case ':': case '@': case '/': case '+': case '#': case '-':
      return true;
    default:
      return is_ascii_alphanumeric(ch);
  }
}

}  // namespace

Status validate_identity_token(std::string_view text, std::string_view kind) {
  if (text.empty()) {
    return Status::failure(ErrorCode::kInvalidArgument, std::string(kind) + " is empty");
  }
  if (text.size() > kMaxIdentifierLength) {
    return Status::failure(ErrorCode::kLimitExceeded,
                           std::string(kind) + " exceeds " +
                               std::to_string(kMaxIdentifierLength) + " characters");
  }
  if (!is_ascii_alphanumeric(text.front())) {
    return Status::failure(ErrorCode::kInvalidArgument,
                           std::string(kind) + " must start with an alphanumeric character");
  }
  for (std::size_t index = 1; index < text.size(); ++index) {
    if (!is_allowed_continuation(text[index])) {
      return Status::failure(ErrorCode::kInvalidArgument,
                             std::string(kind) + " contains a character that is not permitted in "
                                                 "an identity at offset " +
                                 std::to_string(index));
    }
  }
  return Status::success();
}

}  // namespace tobsv
