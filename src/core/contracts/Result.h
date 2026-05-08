#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/Types.h"

namespace vkpt::core {

enum class StatusCode : std::uint8_t {
  Ok = 0,
  Unsupported,
  InvalidArgument,
  NotReady,
  Busy,
  AllocFailed,
  Timeout,
  Cancelled,
  InternalError,
};

inline ErrorCode ToErrorCode(StatusCode code) noexcept {
  switch (code) {
    case StatusCode::Ok:
      return ErrorCode::Ok;
    case StatusCode::Unsupported:
      return ErrorCode::Unsupported;
    case StatusCode::InvalidArgument:
      return ErrorCode::InvalidArgument;
    case StatusCode::Timeout:
      return ErrorCode::Timeout;
    case StatusCode::Cancelled:
      return ErrorCode::Cancelled;
    case StatusCode::NotReady:
    case StatusCode::Busy:
    case StatusCode::AllocFailed:
    case StatusCode::InternalError:
      return ErrorCode::Internal;
  }
  return ErrorCode::Internal;
}

inline StatusCode ToStatusCode(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::Ok:
      return StatusCode::Ok;
    case ErrorCode::InvalidArgument:
      return StatusCode::InvalidArgument;
    case ErrorCode::NotFound:
      return StatusCode::InvalidArgument;
    case ErrorCode::IOError:
      return StatusCode::InternalError;
    case ErrorCode::Unsupported:
      return StatusCode::Unsupported;
    case ErrorCode::Timeout:
      return StatusCode::Timeout;
    case ErrorCode::Internal:
      return StatusCode::InternalError;
    case ErrorCode::Cancelled:
      return StatusCode::Cancelled;
  }
  return StatusCode::InternalError;
}

inline const char* StatusCodeName(StatusCode code) noexcept {
  switch (code) {
    case StatusCode::Ok:
      return "ok";
    case StatusCode::Unsupported:
      return "unsupported";
    case StatusCode::InvalidArgument:
      return "invalid_argument";
    case StatusCode::NotReady:
      return "not_ready";
    case StatusCode::Busy:
      return "busy";
    case StatusCode::AllocFailed:
      return "alloc_failed";
    case StatusCode::Timeout:
      return "timeout";
    case StatusCode::Cancelled:
      return "cancelled";
    case StatusCode::InternalError:
      return "internal_error";
  }
  return "unknown";
}

struct Status {
  using Code = StatusCode;

  Code code = Code::Ok;
  std::string message;
  std::vector<std::string> warnings;
  std::optional<std::string> recovery_hint;

  static Status ok(std::string message = {}) {
    Status out;
    out.message = std::move(message);
    return out;
  }

  static Status error(Code code,
                      std::string message,
                      std::optional<std::string> recovery_hint = std::nullopt) {
    Status out;
    out.code = code;
    out.message = std::move(message);
    out.recovery_hint = std::move(recovery_hint);
    return out;
  }

  bool is_ok() const noexcept { return code == Code::Ok; }
  bool is_error() const noexcept { return !is_ok(); }
  operator bool() const noexcept { return is_ok(); }
};

inline Status StatusFromErrorCode(ErrorCode code, std::string message = {}) {
  const StatusCode status_code = ToStatusCode(code);
  if (status_code == StatusCode::Ok) {
    return Status::ok(std::move(message));
  }
  return Status::error(status_code, std::move(message));
}

inline Status StatusFromResult(const Result<void>& result, std::string message = {}) {
  return StatusFromErrorCode(result.error(), std::move(message));
}

inline Result<void> ResultFromStatus(const Status& status) {
  if (status.is_ok()) {
    return Result<void>::ok();
  }
  return Result<void>::error(ToErrorCode(status.code));
}

}  // namespace vkpt::core

#define VKP_TRY(expr)                                                         \
  do {                                                                        \
    auto _vkp_try_result = (expr);                                            \
    if (_vkp_try_result.is_error()) {                                         \
      return decltype(_vkp_try_result)::error(_vkp_try_result.error());       \
    }                                                                         \
  } while (false)
