#ifndef QCHAR_TRAITS_HPP
#define QCHAR_TRAITS_HPP

#include <cstdint>

#include <gsl/gsl>

namespace qattern {

// Per-encoding policy for the qchar scanner.
//
// A qchar is a window of character values packed most-significant-unit-first. A
// unit is always the decoded character — a code point for UTF-16, the two-byte
// pair for GB2312 — never a raw byte pair, so case folding is defined on the same
// quantity the dictionary keys on. How a unit is assembled from memory is the one
// thing that legitimately differs between encodings, so it lives here too.
//
// The gate is load bearing, not an optimisation. Folding by OR-ing 0x20 into a
// byte maps 0x0E onto '.' and 0x10..0x19 onto '0'..'9', so an ungated scanner
// lights up the dotted-numeric half of the dictionary on runs of control bytes.

// Identity of an encoding, recorded in the dictionary file so a loader can reject
// a mismatched pairing. Key width alone cannot do this: Gb2312Traits and
// CnU16LeTraits are both uint32 = 2 units x 16 bits, yet their keys mean different
// things and are not interchangeable.
enum class Encoding : uint8_t {
    kAnsi = 0,
    kUtf16Le = 1,
    kUtf16Be = 2,
    kCnUtf16Le = 3,
    kGb2312 = 4,
};

struct AnsiTraits {
    using ValueType = char;
    using UnitType = uint8_t;
    using QCharType = uint32_t;

    static constexpr size_t kUnits = 4;
    static constexpr const char* kName = "ansi";
    static constexpr Encoding kEncoding = Encoding::kAnsi;
    static constexpr bool kLowByteFirst = true; // Single-byte encoding; this value has no effect.

    static constexpr bool is_allowed(const UnitType unit) noexcept
    {
        return (unit >= 'a' && unit <= 'z') || (unit >= 'A' && unit <= 'Z') ||
               (unit >= '0' && unit <= '9') || unit == '.';
    }

    // Boundary of the extracted string: any printable ASCII, wider than
    // is_allowed. Used by the consumer for left/right expansion, not by the
    // scanner's gate.
    static constexpr bool is_printable(const UnitType unit) noexcept
    {
        return unit >= ' ' && unit <= '~';
    }

    // Within the gated alphabet this is exactly case folding, and it is a single
    // OR because digits and '.' already have bit 5 set.
    static constexpr UnitType fold(const UnitType unit) noexcept
    {
        return static_cast<UnitType>(unit | 0x20);
    }

    // end_offset is the index of the unit's last byte.
    static constexpr UnitType read_unit(gsl::span<const uint8_t> buf,
                                        const size_t end_offset) noexcept
    {
        return buf[end_offset];
    }
};

struct U16LeTraits {
    using ValueType = char16_t;
    using UnitType = uint16_t;
    using QCharType = uint64_t;

    static constexpr size_t kUnits = 4;
    static constexpr const char* kName = "utf16le";
    static constexpr Encoding kEncoding = Encoding::kUtf16Le;
    static constexpr bool kLowByteFirst = true;

    static constexpr bool is_allowed(const UnitType unit) noexcept
    {
        return (unit >= u'a' && unit <= u'z') || (unit >= u'A' && unit <= u'Z') ||
               (unit >= u'0' && unit <= u'9') || unit == u'.';
    }

    static constexpr bool is_printable(const UnitType unit) noexcept
    {
        return unit >= u' ' && unit <= u'~';
    }

    // Not "unit | 0x20": a whole-window OR would also reach the zero high bytes of
    // BMP characters, and outside the gated alphabet folding has no meaning.
    static constexpr UnitType fold(const UnitType unit) noexcept
    {
        return unit >= u'A' && unit <= u'Z' ? static_cast<UnitType>(unit + 0x20) : unit;
    }

    static constexpr UnitType read_unit(gsl::span<const uint8_t> buf,
                                        const size_t end_offset) noexcept
    {
        return static_cast<UnitType>(buf[end_offset - 1] | (buf[end_offset] << 8));
    }
};

// Genuine UTF-16BE text. The character values — and therefore the dictionary —
// are identical to U16LeTraits and only the byte order differs, so one corpus
// serves both. Note that is *not* the same key as packing the raw memory bytes
// most-significant-byte-first: that form leaves the ASCII character in a unit's
// high byte and needs its own dictionary.
struct U16BeTraits {
    using ValueType = char16_t;
    using UnitType = uint16_t;
    using QCharType = uint64_t;

    static constexpr size_t kUnits = 4;
    static constexpr const char* kName = "utf16be";
    static constexpr Encoding kEncoding = Encoding::kUtf16Be;
    static constexpr bool kLowByteFirst = false;

    static constexpr bool is_allowed(const UnitType unit) noexcept
    {
        return U16LeTraits::is_allowed(unit);
    }

    static constexpr bool is_printable(const UnitType unit) noexcept
    {
        return U16LeTraits::is_printable(unit);
    }

    static constexpr UnitType fold(const UnitType unit) noexcept
    {
        return U16LeTraits::fold(unit);
    }

    static constexpr UnitType read_unit(gsl::span<const uint8_t> buf,
                                        const size_t end_offset) noexcept
    {
        return static_cast<UnitType>(buf[end_offset - 1] << 8 | buf[end_offset]);
    }
};

// A unit is one Chinese character: the lead byte sits at the lower address and
// becomes the unit's high byte, so a unit value is the raw pair in address order
// and the qchar equals the memory bytes packed most-significant-byte-first.
struct Gb2312Traits {
    using ValueType = char;
    using UnitType = uint16_t;
    using QCharType = uint32_t;

    static constexpr size_t kUnits = 2;
    static constexpr const char* kName = "gb2312";
    static constexpr Encoding kEncoding = Encoding::kGb2312;
    static constexpr bool kLowByteFirst = false;

    static constexpr bool is_allowed(const UnitType unit) noexcept
    {
        const auto lead = static_cast<uint8_t>(unit >> 8);
        const auto trail = static_cast<uint8_t>(unit & 0xFF);
        return lead >= 0xA1 && lead <= 0xF7 && trail >= 0xA1 && trail <= 0xFE;
    }

    static constexpr bool is_printable(const UnitType unit) noexcept
    {
        return is_allowed(unit);
    }

    // Han has no case. Folding bytes — what a per-byte normaliser does — rewrites
    // lead and trail alike and silently identifies one character as another.
    static constexpr UnitType fold(const UnitType unit) noexcept
    {
        return unit;
    }

    static constexpr UnitType read_unit(gsl::span<const uint8_t> buf,
                                        const size_t end_offset) noexcept
    {
        return static_cast<UnitType>(buf[end_offset - 1] << 8 | buf[end_offset]);
    }
};

// UTF-16LE Chinese: same unit rule as U16LeTraits, two characters per window.
struct CnU16LeTraits {
    using ValueType = char16_t;
    using UnitType = uint16_t;
    using QCharType = uint32_t;

    static constexpr size_t kUnits = 2;
    static constexpr const char* kName = "cn-utf16le";
    static constexpr Encoding kEncoding = Encoding::kCnUtf16Le;
    static constexpr bool kLowByteFirst = true;

    static constexpr bool is_allowed(const UnitType unit) noexcept
    {
        return (unit >= 0x4E00 && unit <= 0x9FFF)     // CJK Unified Ideographs
               || (unit >= 0x3000 && unit <= 0x303F)  // CJK punctuation
               || (unit >= 0xFF00 && unit <= 0xFFEF)  // full-width forms
               || (unit >= 0xFE30 && unit <= 0xFE4F); // CJK compatibility forms
    }

    static constexpr bool is_printable(const UnitType unit) noexcept
    {
        return is_allowed(unit) || (unit >= u' ' && unit <= u'~');
    }

    static constexpr UnitType fold(const UnitType unit) noexcept
    {
        return unit;
    }

    static constexpr UnitType read_unit(gsl::span<const uint8_t> buf,
                                        const size_t end_offset) noexcept
    {
        return U16LeTraits::read_unit(buf, end_offset);
    }
};

// The window must fill the key type exactly. That is what lets the rolling window
// go without a mask, and it is what forces the index to choose a free sentinel.
static_assert(AnsiTraits::kUnits * sizeof(AnsiTraits::UnitType) == sizeof(AnsiTraits::QCharType));
static_assert(U16LeTraits::kUnits * sizeof(U16LeTraits::UnitType) ==
              sizeof(U16LeTraits::QCharType));
static_assert(U16BeTraits::kUnits * sizeof(U16BeTraits::UnitType) ==
              sizeof(U16BeTraits::QCharType));
static_assert(Gb2312Traits::kUnits * sizeof(Gb2312Traits::UnitType) ==
              sizeof(Gb2312Traits::QCharType));
static_assert(CnU16LeTraits::kUnits * sizeof(CnU16LeTraits::UnitType) ==
              sizeof(CnU16LeTraits::QCharType));

} // namespace qattern

#endif // QCHAR_TRAITS_HPP
