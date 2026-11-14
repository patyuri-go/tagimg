#pragma once

#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

namespace tagimg {

// 「值或错误」容器。
//
// Result<T, E> 要么保存 T（成功），要么保存 E（失败）。
// E 必须是枚举类型。Result<void, E> 表示只有成功/失败、没有值的结果。
//
// 在错误结果上调用 value()，或在成功结果上调用 error()，会抛出
// std::bad_variant_access（void 特化抛 std::bad_optional_access）。
template <typename T, typename E> class [[nodiscard]] Result {
  static_assert(std::is_enum_v<E>, "Result error type E must be an enum");
  static_assert(!std::is_const_v<E> && !std::is_volatile_v<E>,
                "Result error type E must not be cv-qualified");
  static_assert(!std::is_void_v<T>,
                "Use Result<void, E> for results without a value");
  static_assert(!std::is_reference_v<T>, "Result cannot hold references");
  static_assert(std::is_object_v<T> && !std::is_array_v<T>,
                "Result value type T must be a non-array object type");
  static_assert(!std::is_same_v<std::remove_cv_t<T>, std::remove_cv_t<E>>,
                "Result value type T and error type E must differ");

public:
  using value_type = T;
  using error_type = E;

  Result() = delete;

  Result(T value) : storage_(std::in_place_index<0>, std::move(value)) {
  }

  Result(E error) noexcept : storage_(std::in_place_index<1>, error) {
  }

  template <typename... Args>
  explicit Result(std::in_place_t, Args &&...args)
      : storage_(std::in_place_index<0>, std::forward<Args>(args)...) {
  }

  [[nodiscard]] bool has_value() const noexcept {
    return storage_.index() == 0;
  }

  [[nodiscard]] bool has_error() const noexcept {
    return !has_value();
  }

  explicit operator bool() const noexcept {
    return has_value();
  }

  [[nodiscard]] T &value() & {
    return std::get<0>(storage_);
  }

  [[nodiscard]] const T &value() const & {
    return std::get<0>(storage_);
  }

  [[nodiscard]] T &&value() && {
    return std::get<0>(std::move(storage_));
  }

  [[nodiscard]] const T &&value() const && {
    return std::get<0>(std::move(storage_));
  }

  [[nodiscard]] E error() const {
    return std::get<1>(storage_);
  }

  [[nodiscard]] T &operator*() & {
    return value();
  }

  [[nodiscard]] const T &operator*() const & {
    return value();
  }

  [[nodiscard]] T &&operator*() && {
    return std::move(*this).value();
  }

  [[nodiscard]] const T &&operator*() const && {
    return std::move(*this).value();
  }

  [[nodiscard]] T *operator->() {
    return std::addressof(value());
  }

  [[nodiscard]] const T *operator->() const {
    return std::addressof(value());
  }

  template <typename U> [[nodiscard]] T value_or(U &&default_value) const & {
    return has_value() ? value()
                       : static_cast<T>(std::forward<U>(default_value));
  }

  template <typename U> [[nodiscard]] T value_or(U &&default_value) && {
    return has_value() ? std::move(*this).value()
                       : static_cast<T>(std::forward<U>(default_value));
  }

  static Result ok(T value) {
    return Result(std::move(value));
  }

  template <typename... Args>
  static Result ok(std::in_place_t, Args &&...args) {
    return Result(std::in_place, std::forward<Args>(args)...);
  }

  static Result err(E error) noexcept {
    return Result(error);
  }

private:
  std::variant<T, E> storage_;
};

template <typename E> class [[nodiscard]] Result<void, E> {
  static_assert(std::is_enum_v<E>, "Result error type E must be an enum");
  static_assert(!std::is_const_v<E> && !std::is_volatile_v<E>,
                "Result error type E must not be cv-qualified");

public:
  using value_type = void;
  using error_type = E;

  Result() noexcept = default;

  Result(E error) noexcept : error_(error) {
  }

  [[nodiscard]] bool has_value() const noexcept {
    return !error_.has_value();
  }

  [[nodiscard]] bool has_error() const noexcept {
    return error_.has_value();
  }

  explicit operator bool() const noexcept {
    return has_value();
  }

  void value() const {
    if (error_) {
      throw std::bad_optional_access();
    }
  }

  [[nodiscard]] E error() const {
    if (!error_) {
      throw std::bad_optional_access();
    }
    return *error_;
  }

  static Result ok() noexcept {
    return Result();
  }

  static Result err(E error) noexcept {
    return Result(error);
  }

private:
  std::optional<E> error_;
};

} // namespace tagimg
