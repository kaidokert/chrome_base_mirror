// Copyright 2014 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BASE_NUMERICS_SAFE_CONVERSIONS_IMPL_H_
#define BASE_NUMERICS_SAFE_CONVERSIONS_IMPL_H_

// IWYU pragma: private, include "base/numerics/safe_conversions.h"

#include <stddef.h>
#include <stdint.h>

#include <concepts>
#include <limits>
#include <type_traits>

namespace base::internal {

// The std library doesn't provide a binary max_exponent for integers, however
// we can compute an analog using std::numeric_limits<>::digits.
template <typename NumericType>
inline constexpr int kMaxExponent =
    std::is_floating_point_v<NumericType>
        ? std::numeric_limits<NumericType>::max_exponent
        : std::numeric_limits<NumericType>::digits + 1;

// The number of bits (including the sign) in an integer. Eliminates sizeof
// hacks.
template <typename NumericType>
inline constexpr int kIntegerBitsPlusSign =
    std::numeric_limits<NumericType>::digits + std::is_signed_v<NumericType>;

// Determines if a numeric value is negative without throwing compiler
// warnings on: unsigned(value) < 0.
template <typename T>
  requires(std::is_arithmetic_v<T>)
constexpr bool IsValueNegative(T value) {
  if constexpr (std::is_signed_v<T>) {
    return value < 0;
  } else {
    return false;
  }
}

// This performs a fast negation, returning a signed value. It works on unsigned
// arguments, but probably doesn't do what you want for any unsigned value
// larger than max / 2 + 1 (i.e. signed min cast to unsigned).
template <typename T>
  requires std::is_integral_v<T>
constexpr auto ConditionalNegate(T x, bool is_negative) {
  using SignedT = std::make_signed_t<T>;
  using UnsignedT = std::make_unsigned_t<T>;
  return static_cast<SignedT>((static_cast<UnsignedT>(x) ^
                               static_cast<UnsignedT>(-SignedT(is_negative))) +
                              is_negative);
}

// This performs a safe, absolute value via unsigned overflow.
template <typename T>
  requires std::is_integral_v<T>
constexpr auto SafeUnsignedAbs(T value) {
  using UnsignedT = std::make_unsigned_t<T>;
  return IsValueNegative(value)
             ? static_cast<UnsignedT>(0u - static_cast<UnsignedT>(value))
             : static_cast<UnsignedT>(value);
}

// TODO(jschuh): Debug builds don't reliably propagate constants, so we restrict
// some accelerated runtime paths to release builds until this can be forced
// with consteval support in C++20 or C++23.
#if defined(NDEBUG)
inline constexpr bool kEnableAsmCode = true;
#else
inline constexpr bool kEnableAsmCode = false;
#endif

// Forces a crash, like a CHECK(false). Used for numeric boundary errors.
// Also used in a constexpr template to trigger a compilation failure on
// an error condition.
struct CheckOnFailure {
  template <typename T>
  static T HandleFailure() {
#if defined(_MSC_VER)
    __debugbreak();
#elif defined(__GNUC__) || defined(__clang__)
    __builtin_trap();
#else
    ((void)(*(volatile char*)0 = 0));
#endif
    return T();
  }
};

enum class IntegerRepresentation { kUnsigned, kSigned };

// A range for a given nunmeric Src type is contained for a given numeric Dst
// type if both numeric_limits<Src>::max() <= numeric_limits<Dst>::max() and
// numeric_limits<Src>::lowest() >= numeric_limits<Dst>::lowest() are true.
// We implement this as template specializations rather than simple static
// comparisons to ensure type correctness in our comparisons.
enum class NumericRangeRepresentation { kNotContained, kContained };

// Helper templates to statically determine if our destination type can contain
// maximum and minimum values represented by the source type.

// Default case, used for same sign: Dst is guaranteed to contain Src only if
// its range is equal or larger.
template <typename Dst,
          typename Src,
          IntegerRepresentation DstSign =
              std::is_signed_v<Dst> ? IntegerRepresentation::kSigned
                                    : IntegerRepresentation::kUnsigned,
          IntegerRepresentation SrcSign =
              std::is_signed_v<Src> ? IntegerRepresentation::kSigned
                                    : IntegerRepresentation::kUnsigned>
inline constexpr auto kStaticDstRangeRelationToSrcRange =
    kMaxExponent<Dst> >= kMaxExponent<Src>
        ? NumericRangeRepresentation::kContained
        : NumericRangeRepresentation::kNotContained;

// Unsigned to signed: Dst is guaranteed to contain source only if its range is
// larger.
template <typename Dst, typename Src>
inline constexpr auto
    kStaticDstRangeRelationToSrcRange<Dst,
                                      Src,
                                      IntegerRepresentation::kSigned,
                                      IntegerRepresentation::kUnsigned> =
        kMaxExponent<Dst> > kMaxExponent<Src>
            ? NumericRangeRepresentation::kContained
            : NumericRangeRepresentation::kNotContained;

// Signed to unsigned: Dst cannot be statically determined to contain Src.
template <typename Dst, typename Src>
inline constexpr auto
    kStaticDstRangeRelationToSrcRange<Dst,
                                      Src,
                                      IntegerRepresentation::kUnsigned,
                                      IntegerRepresentation::kSigned> =
        NumericRangeRepresentation::kNotContained;

// This class wraps the range constraints as separate booleans so the compiler
// can identify constants and eliminate unused code paths.
class RangeCheck {
 public:
  constexpr RangeCheck() = default;
  constexpr RangeCheck(bool is_in_lower_bound, bool is_in_upper_bound)
      : is_underflow_(!is_in_lower_bound), is_overflow_(!is_in_upper_bound) {}

  constexpr bool operator==(const RangeCheck& rhs) const = default;

  constexpr bool IsValid() const { return !is_overflow_ && !is_underflow_; }
  constexpr bool IsInvalid() const { return is_overflow_ && is_underflow_; }
  constexpr bool IsOverflow() const { return is_overflow_ && !is_underflow_; }
  constexpr bool IsUnderflow() const { return !is_overflow_ && is_underflow_; }
  constexpr bool IsOverflowFlagSet() const { return is_overflow_; }
  constexpr bool IsUnderflowFlagSet() const { return is_underflow_; }

 private:
  // Do not change the order of these member variables. The integral conversion
  // optimization depends on this exact order.
  const bool is_underflow_ = false;
  const bool is_overflow_ = false;
};

// The following helper template addresses a corner case in range checks for
// conversion from a floating-point type to an integral type of smaller range
// but larger precision (e.g. float -> unsigned). The problem is as follows:
//   1. Integral maximum is always one less than a power of two, so it must be
//      truncated to fit the mantissa of the floating point. The direction of
//      rounding is implementation defined, but by default it's always IEEE
//      floats, which round to nearest and thus result in a value of larger
//      magnitude than the integral value.
//      Example: float f = UINT_MAX; // f is 4294967296f but UINT_MAX
//                                   // is 4294967295u.
//   2. If the floating point value is equal to the promoted integral maximum
//      value, a range check will erroneously pass.
//      Example: (4294967296f <= 4294967295u) // This is true due to a precision
//                                            // loss in rounding up to float.
//   3. When the floating point value is then converted to an integral, the
//      resulting value is out of range for the target integral type and
//      thus is implementation defined.
//      Example: unsigned u = (float)INT_MAX; // u will typically overflow to 0.
// To fix this bug we manually truncate the maximum value when the destination
// type is an integral of larger precision than the source floating-point type,
// such that the resulting maximum is represented exactly as a floating point.
template <typename Dst, typename Src, template <typename> class Bounds>
struct NarrowingRange {
  using SrcLimits = std::numeric_limits<Src>;
  using DstLimits = std::numeric_limits<Dst>;

  // Computes the mask required to make an accurate comparison between types.
  static constexpr int kShift = (kMaxExponent<Src> > kMaxExponent<Dst> &&
                                 SrcLimits::digits < DstLimits::digits)
                                    ? (DstLimits::digits - SrcLimits::digits)
                                    : 0;

  template <typename T>
    requires(std::same_as<T, Dst> &&
             ((std::integral<T> && kShift < DstLimits::digits) ||
              (std::floating_point<T> && kShift == 0)))
  // Masks out the integer bits that are beyond the precision of the
  // intermediate type used for comparison.
  static constexpr T Adjust(T value) {
    if constexpr (std::integral<T>) {
      using UnsignedDst = typename std::make_unsigned_t<T>;
      return static_cast<T>(
          ConditionalNegate(SafeUnsignedAbs(value) &
                                ~((UnsignedDst{1} << kShift) - UnsignedDst{1}),
                            IsValueNegative(value)));
    } else {
      return value;
    }
  }

  static constexpr Dst max() { return Adjust(Bounds<Dst>::max()); }
  static constexpr Dst lowest() { return Adjust(Bounds<Dst>::lowest()); }
};

// The following templates are for ranges that must be verified at runtime. We
// split it into checks based on signedness to avoid confusing casts and
// compiler warnings on signed an unsigned comparisons.

// Default case, used for same sign narrowing: The range is contained for normal
// limits.
template <typename Dst,
          typename Src,
          template <typename>
          class Bounds,
          IntegerRepresentation DstSign =
              std::is_signed_v<Dst> ? IntegerRepresentation::kSigned
                                    : IntegerRepresentation::kUnsigned,
          IntegerRepresentation SrcSign =
              std::is_signed_v<Src> ? IntegerRepresentation::kSigned
                                    : IntegerRepresentation::kUnsigned,
          NumericRangeRepresentation DstRange =
              kStaticDstRangeRelationToSrcRange<Dst, Src>>
struct DstRangeRelationToSrcRangeImpl {
  static constexpr RangeCheck Check(Src value) {
    using SrcLimits = std::numeric_limits<Src>;
    using DstLimits = NarrowingRange<Dst, Src, Bounds>;
    return RangeCheck(
        static_cast<Dst>(SrcLimits::lowest()) >= DstLimits::lowest() ||
            static_cast<Dst>(value) >= DstLimits::lowest(),
        static_cast<Dst>(SrcLimits::max()) <= DstLimits::max() ||
            static_cast<Dst>(value) <= DstLimits::max());
  }
};

// Signed to signed narrowing: Both the upper and lower boundaries may be
// exceeded for standard limits.
template <typename Dst, typename Src, template <typename> class Bounds>
struct DstRangeRelationToSrcRangeImpl<
    Dst,
    Src,
    Bounds,
    IntegerRepresentation::kSigned,
    IntegerRepresentation::kSigned,
    NumericRangeRepresentation::kNotContained> {
  static constexpr RangeCheck Check(Src value) {
    using DstLimits = NarrowingRange<Dst, Src, Bounds>;
    return RangeCheck(value >= DstLimits::lowest(), value <= DstLimits::max());
  }
};

// Unsigned to unsigned narrowing: Only the upper bound can be exceeded for
// standard limits.
template <typename Dst, typename Src, template <typename> class Bounds>
struct DstRangeRelationToSrcRangeImpl<
    Dst,
    Src,
    Bounds,
    IntegerRepresentation::kUnsigned,
    IntegerRepresentation::kUnsigned,
    NumericRangeRepresentation::kNotContained> {
  static constexpr RangeCheck Check(Src value) {
    using DstLimits = NarrowingRange<Dst, Src, Bounds>;
    return RangeCheck(
        DstLimits::lowest() == Dst(0) || value >= DstLimits::lowest(),
        value <= DstLimits::max());
  }
};

// Unsigned to signed: Only the upper bound can be exceeded for standard limits.
template <typename Dst, typename Src, template <typename> class Bounds>
struct DstRangeRelationToSrcRangeImpl<
    Dst,
    Src,
    Bounds,
    IntegerRepresentation::kSigned,
    IntegerRepresentation::kUnsigned,
    NumericRangeRepresentation::kNotContained> {
  static constexpr RangeCheck Check(Src value) {
    using DstLimits = NarrowingRange<Dst, Src, Bounds>;
    using Promotion = decltype(Src() + Dst());
    return RangeCheck(DstLimits::lowest() <= Dst(0) ||
                          static_cast<Promotion>(value) >=
                              static_cast<Promotion>(DstLimits::lowest()),
                      static_cast<Promotion>(value) <=
                          static_cast<Promotion>(DstLimits::max()));
  }
};

// Signed to unsigned: The upper boundary may be exceeded for a narrower Dst,
// and any negative value exceeds the lower boundary for standard limits.
template <typename Dst, typename Src, template <typename> class Bounds>
struct DstRangeRelationToSrcRangeImpl<
    Dst,
    Src,
    Bounds,
    IntegerRepresentation::kUnsigned,
    IntegerRepresentation::kSigned,
    NumericRangeRepresentation::kNotContained> {
  static constexpr RangeCheck Check(Src value) {
    using SrcLimits = std::numeric_limits<Src>;
    using DstLimits = NarrowingRange<Dst, Src, Bounds>;
    using Promotion = decltype(Src() + Dst());
    bool ge_zero = false;
    // Converting floating-point to integer will discard fractional part, so
    // values in (-1.0, -0.0) will truncate to 0 and fit in Dst.
    if constexpr (std::is_floating_point_v<Src>) {
      ge_zero = value > Src(-1);
    } else {
      ge_zero = value >= Src(0);
    }
    return RangeCheck(
        ge_zero && (DstLimits::lowest() == 0 ||
                    static_cast<Dst>(value) >= DstLimits::lowest()),
        static_cast<Promotion>(SrcLimits::max()) <=
                static_cast<Promotion>(DstLimits::max()) ||
            static_cast<Promotion>(value) <=
                static_cast<Promotion>(DstLimits::max()));
  }
};

// Simple wrapper for statically checking if a type's range is contained.
template <typename Dst, typename Src>
inline constexpr bool kIsTypeInRangeForNumericType =
    kStaticDstRangeRelationToSrcRange<Dst, Src> ==
    NumericRangeRepresentation::kContained;

template <typename Dst,
          template <typename> class Bounds = std::numeric_limits,
          typename Src>
  requires(std::is_arithmetic_v<Src> && std::is_arithmetic_v<Dst> &&
           Bounds<Dst>::lowest() < Bounds<Dst>::max())
constexpr RangeCheck DstRangeRelationToSrcRange(Src value) {
  return DstRangeRelationToSrcRangeImpl<Dst, Src, Bounds>::Check(value);
}

// Integer promotion templates used by the portable checked integer arithmetic.
template <size_t Size, bool IsSigned>
struct IntegerForDigitsAndSignImpl;

#define INTEGER_FOR_DIGITS_AND_SIGN(I)                        \
  template <>                                                 \
  struct IntegerForDigitsAndSignImpl<kIntegerBitsPlusSign<I>, \
                                     std::is_signed_v<I>> {   \
    using type = I;                                           \
  }

INTEGER_FOR_DIGITS_AND_SIGN(int8_t);
INTEGER_FOR_DIGITS_AND_SIGN(uint8_t);
INTEGER_FOR_DIGITS_AND_SIGN(int16_t);
INTEGER_FOR_DIGITS_AND_SIGN(uint16_t);
INTEGER_FOR_DIGITS_AND_SIGN(int32_t);
INTEGER_FOR_DIGITS_AND_SIGN(uint32_t);
INTEGER_FOR_DIGITS_AND_SIGN(int64_t);
INTEGER_FOR_DIGITS_AND_SIGN(uint64_t);
#undef INTEGER_FOR_DIGITS_AND_SIGN

template <size_t Size, bool IsSigned>
using IntegerForDigitsAndSign =
    IntegerForDigitsAndSignImpl<Size, IsSigned>::type;

// WARNING: We have no IntegerForSizeAndSign<16, *>. If we ever add one to
// support 128-bit math, then the ArithmeticPromotion template below will need
// to be updated (or more likely replaced with a decltype expression).
static_assert(kIntegerBitsPlusSign<intmax_t> == 64,
              "Max integer size not supported for this toolchain.");

template <typename Integer, bool IsSigned = std::is_signed_v<Integer>>
using TwiceWiderInteger =
    IntegerForDigitsAndSign<kIntegerBitsPlusSign<Integer> * 2, IsSigned>;

// Determines the type that can represent the largest positive value.
template <typename Lhs, typename Rhs>
using MaxExponentPromotion =
    std::conditional_t<(kMaxExponent<Lhs> > kMaxExponent<Rhs>), Lhs, Rhs>;

// Determines the type that can represent the lowest arithmetic value.
template <typename Lhs, typename Rhs>
using LowestValuePromotion = std::conditional_t<
    std::is_signed_v<Lhs>
        ? (!std::is_signed_v<Rhs> || kMaxExponent<Lhs> > kMaxExponent<Rhs>)
        : (!std::is_signed_v<Rhs> && kMaxExponent<Lhs> < kMaxExponent<Rhs>),
    Lhs,
    Rhs>;

// Determines the type that is best able to represent an arithmetic result.

// Default case, used when the side with the max exponent is big enough.
template <typename Lhs,
          typename Rhs = Lhs,
          bool is_intmax_type =
              std::is_integral_v<MaxExponentPromotion<Lhs, Rhs>> &&
              kIntegerBitsPlusSign<MaxExponentPromotion<Lhs, Rhs>> ==
                  kIntegerBitsPlusSign<intmax_t>,
          bool is_max_exponent =
              kStaticDstRangeRelationToSrcRange<MaxExponentPromotion<Lhs, Rhs>,
                                                Lhs> ==
                  NumericRangeRepresentation::kContained &&
              kStaticDstRangeRelationToSrcRange<MaxExponentPromotion<Lhs, Rhs>,
                                                Rhs> ==
                  NumericRangeRepresentation::kContained>
struct BigEnoughPromotionImpl {
  using type = MaxExponentPromotion<Lhs, Rhs>;
  static constexpr bool kContained = true;
};

// We can use a twice wider type to fit.
template <typename Lhs, typename Rhs>
struct BigEnoughPromotionImpl<Lhs, Rhs, false, false> {
  using type =
      TwiceWiderInteger<MaxExponentPromotion<Lhs, Rhs>,
                        std::is_signed_v<Lhs> || std::is_signed_v<Rhs>>;
  static constexpr bool kContained = true;
};

// No type is large enough.
template <typename Lhs, typename Rhs>
struct BigEnoughPromotionImpl<Lhs, Rhs, true, false> {
  using type = MaxExponentPromotion<Lhs, Rhs>;
  static constexpr bool kContained = false;
};

template <typename Lhs, typename Rhs>
using BigEnoughPromotion = BigEnoughPromotionImpl<Lhs, Rhs>::type;

template <typename Lhs, typename Rhs>
inline constexpr bool kIsBigEnoughPromotionContained =
    BigEnoughPromotionImpl<Lhs, Rhs>::kContained;

// We can statically check if operations on the provided types can wrap, so we
// can skip the checked operations if they're not needed. So, for an integer we
// care if the destination type preserves the sign and is twice the width of
// the source.
template <typename T, typename Lhs, typename Rhs = Lhs>
inline constexpr bool kIsIntegerArithmeticSafe =
    !std::is_floating_point_v<T> && !std::is_floating_point_v<Lhs> &&
    !std::is_floating_point_v<Rhs> &&
    std::is_signed_v<T> >= std::is_signed_v<Lhs> &&
    kIntegerBitsPlusSign<T> >=
        (2 * kIntegerBitsPlusSign<Lhs>)&&std::is_signed_v<T> >=
        std::is_signed_v<Rhs> &&
    kIntegerBitsPlusSign<T> >= (2 * kIntegerBitsPlusSign<Rhs>);

// Promotes to a type that can represent any possible result of a binary
// arithmetic operation with the source types.
template <typename Lhs, typename Rhs>
struct FastIntegerArithmeticPromotionImpl {
  using type = BigEnoughPromotion<Lhs, Rhs>;
  static constexpr bool kContained = false;
};

template <typename Lhs, typename Rhs>
  requires(kIsIntegerArithmeticSafe<
           std::conditional_t<std::is_signed_v<Lhs> || std::is_signed_v<Rhs>,
                              intmax_t,
                              uintmax_t>,
           MaxExponentPromotion<Lhs, Rhs>>)
struct FastIntegerArithmeticPromotionImpl<Lhs, Rhs> {
  using type =
      TwiceWiderInteger<MaxExponentPromotion<Lhs, Rhs>,
                        std::is_signed_v<Lhs> || std::is_signed_v<Rhs>>;
  static_assert(kIsIntegerArithmeticSafe<type, Lhs, Rhs>);
  static constexpr bool kContained = true;
};

template <typename Lhs, typename Rhs>
using FastIntegerArithmeticPromotion =
    FastIntegerArithmeticPromotionImpl<Lhs, Rhs>::type;

template <typename Lhs, typename Rhs>
inline constexpr bool kIsFastIntegerArithmeticPromotionContained =
    FastIntegerArithmeticPromotionImpl<Lhs, Rhs>::kContained;

// Extracts the underlying type from an enum.
template <typename T>
using ArithmeticOrUnderlyingEnum =
    typename std::conditional_t<std::is_enum_v<T>,
                                std::underlying_type<T>,
                                std::type_identity<T>>::type;

// The following are helper templates used in the CheckedNumeric class.
template <typename T>
  requires std::is_arithmetic_v<T>
class CheckedNumeric;

template <typename T>
  requires std::is_arithmetic_v<T>
class ClampedNumeric;

template <typename T>
  requires std::is_arithmetic_v<T>
class StrictNumeric;

// Used to treat CheckedNumeric and arithmetic underlying types the same.
template <typename T>
struct UnderlyingType {
  using type = ArithmeticOrUnderlyingEnum<T>;
  static constexpr bool is_numeric = std::is_arithmetic_v<type>;
  static constexpr bool is_checked = false;
  static constexpr bool is_clamped = false;
  static constexpr bool is_strict = false;
};

template <typename T>
struct UnderlyingType<CheckedNumeric<T>> {
  using type = T;
  static constexpr bool is_numeric = true;
  static constexpr bool is_checked = true;
  static constexpr bool is_clamped = false;
  static constexpr bool is_strict = false;
};

template <typename T>
struct UnderlyingType<ClampedNumeric<T>> {
  using type = T;
  static constexpr bool is_numeric = true;
  static constexpr bool is_checked = false;
  static constexpr bool is_clamped = true;
  static constexpr bool is_strict = false;
};

template <typename T>
struct UnderlyingType<StrictNumeric<T>> {
  using type = T;
  static constexpr bool is_numeric = true;
  static constexpr bool is_checked = false;
  static constexpr bool is_clamped = false;
  static constexpr bool is_strict = true;
};

template <typename L, typename R>
inline constexpr bool kIsCheckedOp =
    UnderlyingType<L>::is_numeric && UnderlyingType<R>::is_numeric &&
    (UnderlyingType<L>::is_checked || UnderlyingType<R>::is_checked);

template <typename L, typename R>
inline constexpr bool kIsClampedOp =
    UnderlyingType<L>::is_numeric && UnderlyingType<R>::is_numeric &&
    (UnderlyingType<L>::is_clamped || UnderlyingType<R>::is_clamped) &&
    !(UnderlyingType<L>::is_checked || UnderlyingType<R>::is_checked);

template <typename L, typename R>
inline constexpr bool kIsStrictOp =
    UnderlyingType<L>::is_numeric && UnderlyingType<R>::is_numeric &&
    (UnderlyingType<L>::is_strict || UnderlyingType<R>::is_strict) &&
    !(UnderlyingType<L>::is_checked || UnderlyingType<R>::is_checked) &&
    !(UnderlyingType<L>::is_clamped || UnderlyingType<R>::is_clamped);

// as_signed<> returns the supplied integral value (or integral castable
// Numeric template) cast as a signed integral of equivalent precision.
// I.e. it's mostly an alias for: static_cast<std::make_signed<T>::type>(t)
template <typename Src,
          typename Dst = std::make_signed_t<
              typename base::internal::UnderlyingType<Src>::type>>
  requires std::integral<Dst>
constexpr auto as_signed(Src value) {
  return static_cast<Dst>(value);
}

// as_unsigned<> returns the supplied integral value (or integral castable
// Numeric template) cast as an unsigned integral of equivalent precision.
// I.e. it's mostly an alias for: static_cast<std::make_unsigned<T>::type>(t)
template <typename Src,
          typename Dst = std::make_unsigned_t<
              typename base::internal::UnderlyingType<Src>::type>>
  requires std::integral<Dst>
constexpr auto as_unsigned(Src value) {
  return static_cast<Dst>(value);
}

template <typename L, typename R>
constexpr bool IsLessImpl(const L lhs,
                          const R rhs,
                          const RangeCheck l_range,
                          const RangeCheck r_range) {
  return l_range.IsUnderflow() || r_range.IsOverflow() ||
         (l_range == r_range && static_cast<decltype(lhs + rhs)>(lhs) <
                                    static_cast<decltype(lhs + rhs)>(rhs));
}

template <typename L, typename R>
  requires std::is_arithmetic_v<L> && std::is_arithmetic_v<R>
struct IsLess {
  static constexpr bool Test(const L lhs, const R rhs) {
    return IsLessImpl(lhs, rhs, DstRangeRelationToSrcRange<R>(lhs),
                      DstRangeRelationToSrcRange<L>(rhs));
  }
};

template <typename L, typename R>
constexpr bool IsLessOrEqualImpl(const L lhs,
                                 const R rhs,
                                 const RangeCheck l_range,
                                 const RangeCheck r_range) {
  return l_range.IsUnderflow() || r_range.IsOverflow() ||
         (l_range == r_range && static_cast<decltype(lhs + rhs)>(lhs) <=
                                    static_cast<decltype(lhs + rhs)>(rhs));
}

template <typename L, typename R>
  requires std::is_arithmetic_v<L> && std::is_arithmetic_v<R>
struct IsLessOrEqual {
  static constexpr bool Test(const L lhs, const R rhs) {
    return IsLessOrEqualImpl(lhs, rhs, DstRangeRelationToSrcRange<R>(lhs),
                             DstRangeRelationToSrcRange<L>(rhs));
  }
};

template <typename L, typename R>
constexpr bool IsGreaterImpl(const L lhs,
                             const R rhs,
                             const RangeCheck l_range,
                             const RangeCheck r_range) {
  return l_range.IsOverflow() || r_range.IsUnderflow() ||
         (l_range == r_range && static_cast<decltype(lhs + rhs)>(lhs) >
                                    static_cast<decltype(lhs + rhs)>(rhs));
}

template <typename L, typename R>
  requires std::is_arithmetic_v<L> && std::is_arithmetic_v<R>
struct IsGreater {
  static constexpr bool Test(const L lhs, const R rhs) {
    return IsGreaterImpl(lhs, rhs, DstRangeRelationToSrcRange<R>(lhs),
                         DstRangeRelationToSrcRange<L>(rhs));
  }
};

template <typename L, typename R>
constexpr bool IsGreaterOrEqualImpl(const L lhs,
                                    const R rhs,
                                    const RangeCheck l_range,
                                    const RangeCheck r_range) {
  return l_range.IsOverflow() || r_range.IsUnderflow() ||
         (l_range == r_range && static_cast<decltype(lhs + rhs)>(lhs) >=
                                    static_cast<decltype(lhs + rhs)>(rhs));
}

template <typename L, typename R>
  requires std::is_arithmetic_v<L> && std::is_arithmetic_v<R>
struct IsGreaterOrEqual {
  static constexpr bool Test(const L lhs, const R rhs) {
    return IsGreaterOrEqualImpl(lhs, rhs, DstRangeRelationToSrcRange<R>(lhs),
                                DstRangeRelationToSrcRange<L>(rhs));
  }
};

template <typename L, typename R>
  requires std::is_arithmetic_v<L> && std::is_arithmetic_v<R>
struct IsEqual {
  static constexpr bool Test(const L lhs, const R rhs) {
    return DstRangeRelationToSrcRange<R>(lhs) ==
               DstRangeRelationToSrcRange<L>(rhs) &&
           static_cast<decltype(lhs + rhs)>(lhs) ==
               static_cast<decltype(lhs + rhs)>(rhs);
  }
};

template <typename L, typename R>
  requires std::is_arithmetic_v<L> && std::is_arithmetic_v<R>
struct IsNotEqual {
  static constexpr bool Test(const L lhs, const R rhs) {
    return DstRangeRelationToSrcRange<R>(lhs) !=
               DstRangeRelationToSrcRange<L>(rhs) ||
           static_cast<decltype(lhs + rhs)>(lhs) !=
               static_cast<decltype(lhs + rhs)>(rhs);
  }
};

// These perform the actual math operations on the CheckedNumerics.
// Binary arithmetic operations.
template <template <typename, typename> class C, typename L, typename R>
  requires std::is_arithmetic_v<L> && std::is_arithmetic_v<R>
constexpr bool SafeCompare(const L lhs, const R rhs) {
  using BigType = BigEnoughPromotion<L, R>;
  return kIsBigEnoughPromotionContained<L, R>
             // Force to a larger type for speed if both are contained.
             ? C<BigType, BigType>::Test(static_cast<BigType>(lhs),
                                         static_cast<BigType>(rhs))
             // Let the template functions figure it out for mixed types.
             : C<L, R>::Test(lhs, rhs);
}

template <typename Dst, typename Src>
inline constexpr bool kIsMaxInRangeForNumericType =
    IsGreaterOrEqual<Dst, Src>::Test(std::numeric_limits<Dst>::max(),
                                     std::numeric_limits<Src>::max());

template <typename Dst, typename Src>
inline constexpr bool kIsMinInRangeForNumericType =
    IsLessOrEqual<Dst, Src>::Test(std::numeric_limits<Dst>::lowest(),
                                  std::numeric_limits<Src>::lowest());

template <typename Dst, typename Src>
inline constexpr Dst kCommonMax =
    kIsMaxInRangeForNumericType<Dst, Src>
        ? static_cast<Dst>(std::numeric_limits<Src>::max())
        : std::numeric_limits<Dst>::max();

template <typename Dst, typename Src>
inline constexpr Dst kCommonMin =
    kIsMinInRangeForNumericType<Dst, Src>
        ? static_cast<Dst>(std::numeric_limits<Src>::lowest())
        : std::numeric_limits<Dst>::lowest();

// This is a wrapper to generate return the max or min for a supplied type.
// If the argument is false, the returned value is the maximum. If true the
// returned value is the minimum.
template <typename Dst, typename Src = Dst>
constexpr Dst CommonMaxOrMin(bool is_min) {
  return is_min ? kCommonMin<Dst, Src> : kCommonMax<Dst, Src>;
}

}  // namespace base::internal

#endif  // BASE_NUMERICS_SAFE_CONVERSIONS_IMPL_H_
