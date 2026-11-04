#pragma once
// Common result / status types and model-wide constants.

#include <optional>
#include <string>
#include <utility>

namespace sbr {

// Model units are millimetres throughout the core library.
namespace tolerance {
// Geometric comparison tolerance in mm (coplanarity, coincidence, ...).
// Deliberately much larger than OCCT's Precision::Confusion() (1e-7) so that
// values typed by the user and round-tripped through files compare stably.
constexpr double kLinear = 1e-4;
// Angular tolerance in radians used for parallelism tests.
constexpr double kAngular = 1e-7;
// Smallest dimension accepted for a sketch or an extrusion (mm).
constexpr double kMinDimension = 1e-3;
}  // namespace tolerance

class Status {
 public:
  static Status success() { return Status(true, {}); }
  static Status failure(std::string message) { return Status(false, std::move(message)); }

  bool ok() const { return ok_; }
  explicit operator bool() const { return ok_; }
  const std::string& message() const { return message_; }

 private:
  Status(bool ok, std::string message) : ok_(ok), message_(std::move(message)) {}
  bool ok_;
  std::string message_;
};

template <typename T>
class Result {
 public:
  Result(T value) : value_(std::move(value)) {}  // NOLINT(google-explicit-constructor)
  static Result failure(std::string message) { return Result(std::move(message), 0); }

  bool ok() const { return value_.has_value(); }
  explicit operator bool() const { return ok(); }
  const T& value() const { return *value_; }
  T& value() { return *value_; }
  const T& operator*() const { return *value_; }
  const T* operator->() const { return &*value_; }
  const std::string& error() const { return error_; }
  Status status() const { return ok() ? Status::success() : Status::failure(error_); }

 private:
  Result(std::string message, int) : error_(std::move(message)) {}
  std::optional<T> value_;
  std::string error_;
};

}  // namespace sbr
