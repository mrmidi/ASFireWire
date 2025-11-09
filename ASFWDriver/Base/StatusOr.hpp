// #pragma once
// #include <string>
// #include <utility>
// #include <type_traits>
// #include <cassert>

// namespace ASFW {
// class Status {
// public:
//     enum Code : int { kOk=0, kUnknown=1, kInvalidArgument=2, kNotFound=3, kUnavailable=4, kInternal=5 };
//     constexpr Status() : code_(kOk) {}
//     constexpr explicit Status(Code c, std::string msg = {}) : code_(c), msg_(std::move(msg)) {}
//     [[nodiscard]] constexpr bool ok() const { return code_ == kOk; }
//     [[nodiscard]] constexpr Code code() const { return code_; }
//     [[nodiscard]] const std::string& message() const { return msg_; }
//     static constexpr Status Ok() { return Status{}; }
// private:
//     Code code_;
//     std::string msg_;
// };

// template <class T>
// class StatusOr {
// public:
//     using value_type = T;
//     StatusOr(T v) requires (!std::is_same_v<T, Status>) : ok_(true) { new (&storage_) T(std::move(v)); }
//     StatusOr(const T& v) : ok_(true) { new (&storage_) T(v); }
//     StatusOr(Status s) : ok_(false), status_(std::move(s)) { assert(!status_.ok()); }
//     StatusOr(const StatusOr& o) : ok_(o.ok_) {
//         if (ok_) new (&storage_) T(o.value());
//         else status_ = o.status_;
//     }
//     StatusOr(StatusOr&& o) noexcept(std::is_nothrow_move_constructible_v<T>) : ok_(o.ok_) {
//         if (ok_) new (&storage_) T(std::move(o.value()));
//         else status_ = std::move(o.status_);
//     }
//     StatusOr& operator=(StatusOr o) { swap(o); return *this; }
//     ~StatusOr() { reset(); }
//     [[nodiscard]] bool ok() const { return ok_; }
//     [[nodiscard]] const Status& status() const { return ok_ ? kOk_ : status_; }
//     T& value() & { assert(ok_); return *ptr(); }
//     const T& value() const & { assert(ok_); return *ptr(); }
//     T&& value() && { assert(ok_); return std::move(*ptr()); }
//     template<class U>
//     T value_or(U&& alt) const { return ok_ ? *ptr_const() : static_cast<T>(std::forward<U>(alt)); }
//     void swap(StatusOr& other) {
//         if (this == &other) return;
//         if (ok_ && other.ok_) { using std::swap; swap(value(), other.value()); return; }
//         if (ok_ && !other.ok_) {
//             Status tmp = std::move(other.status_);
//             other.destroy(); other.ok_ = true; new (&other.storage_) T(std::move(value()));
//             destroy(); ok_ = false; status_ = std::move(tmp);
//             return;
//         }
//         if (!ok_ && other.ok_) { other.swap(*this); return; }
//         using std::swap; swap(status_, other.status_);
//     }
// private:
//     void destroy() { if (ok_) ptr()->~T(); }
//     void reset() { destroy(); }
//     T* ptr() { return std::launder(reinterpret_cast<T*>(&storage_)); }
//     const T* ptr_const() const { return std::launder(reinterpret_cast<const T*>(&storage_)); }
//     static const Status kOk_;
//     bool ok_{false};
//     union { std::aligned_storage_t<sizeof(T), alignof(T)> storage_; };
//     Status status_{Status::kUnknown, "uninitialized"};
// };

// template<class T>
// const Status StatusOr<T>::kOk_ = Status::Ok();
// } // namespace ASFW
