#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <span>
#include <variant>

namespace vkpt::core {

using StableId = std::uint64_t;
using RuntimeHandle = std::uint64_t;
using ByteSpan = std::span<const std::byte>;
using StringView = std::string_view;
using FrameIndex = std::uint64_t;
using Hash128 = std::array<std::uint8_t, 16>;
using Hash256 = std::array<std::uint8_t, 32>;

using StableEntityId = StableId;
using EntityHandle = RuntimeHandle;
using AssetId = StableId;
using MaterialId = StableId;
using TextureHandle = RuntimeHandle;
using BufferHandle = RuntimeHandle;
using PipelineHandle = RuntimeHandle;
using ShaderHandle = RuntimeHandle;
using SceneHandle = RuntimeHandle;
using BackendDeviceHandle = RuntimeHandle;
using AccelerationHandle = RuntimeHandle;
using BenchmarkRunId = RuntimeHandle;
using JobHandle = RuntimeHandle;

enum class ErrorCode : std::uint8_t {
  Ok = 0,
  InvalidArgument,
  NotFound,
  IOError,
  Unsupported,
  Timeout,
  Internal,
  Cancelled
};

template <typename T>
class [[nodiscard]] Result {
 public:
  using ValueType = T;

  Result() = delete;
  static Result ok(T value) { return Result(InnerOk{std::move(value)}); }
  static Result error(ErrorCode error) { return Result(InnerError{error}); }

  bool has_value() const { return m_state.index() == 0; }
  explicit operator bool() const { return has_value(); }

  T& value() { return std::get<0>(m_state).value; }
  const T& value() const { return std::get<0>(m_state).value; }
  ErrorCode error() const { return has_value() ? ErrorCode::Ok : std::get<1>(m_state).code; }

 private:
  struct InnerOk { T value; };
  struct InnerError { ErrorCode code; };
  std::variant<InnerOk, InnerError> m_state;

  explicit Result(InnerOk ok) : m_state{std::move(ok)} {}
  explicit Result(InnerError error) : m_state{error} {}
};

template <>
class [[nodiscard]] Result<void> {
 public:
  static Result ok() { return Result(true); }
  static Result error(ErrorCode code) { return Result(false, code); }

  bool has_value() const { return m_ok; }
  explicit operator bool() const { return m_ok; }
  ErrorCode error() const { return m_ok ? ErrorCode::Ok : m_error; }

 private:
  bool m_ok = true;
  ErrorCode m_error = ErrorCode::Ok;
  Result(bool ok, ErrorCode error = ErrorCode::Ok) : m_ok(ok), m_error(error) {}
};

}  // namespace vkpt::core
