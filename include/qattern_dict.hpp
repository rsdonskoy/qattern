#ifndef QATTERN_DICT_HPP
#define QATTERN_DICT_HPP

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include <gsl/gsl>

#include "qchar_traits.hpp"

namespace qattern {

// Dictionary container: a fixed header followed by the qchar keys in ascending
// order, each stored little-endian at its natural width.
//
// Keys are stored already folded, so a dictionary built from a corpus that mixes
// cases holds one entry per matchable window rather than carrying entries no
// scan could ever report. The generator and the in-process builder therefore have
// to fold the same way, which is why both go through pack_units.
//
// Only header and bytes are shared between the generated C++ array and a file on
// disk, so one parse path validates either source.

constexpr uint32_t kDictMagic = 0x444E5451u; // 'Q','T','N','D'
constexpr uint16_t kDictVersion = 1;
constexpr size_t kDictHeaderBytes = 16;

// On-disk layout, as byte offsets from the start of the blob. Every multi-byte
// field is little-endian, same as the key area, so the header does not depend on
// the host's byte order.
namespace detail::layout {
inline constexpr size_t kMagic = 0;      // uint32
inline constexpr size_t kVersion = 4;    // uint16
inline constexpr size_t kKeyBytes = 6;   // uint8
inline constexpr size_t kEncoding = 7;   // uint8
inline constexpr size_t kEntryCount = 8; // uint32
inline constexpr size_t kUnitBytes = 12; // uint8
inline constexpr size_t kUnits = 13;     // uint8
inline constexpr size_t kReserved = 14;  // uint16
} // namespace detail::layout

// A parsed header: an ordinary value type, not a memory layout. Fields are read
// and written by explicit offset, so no packed struct is overlaid on the bytes.
struct DictHeader {
    uint16_t version{0};
    uint8_t key_bytes{0};
    uint8_t encoding{0};
    uint32_t entry_count{0};
    uint8_t unit_bytes{0};
    uint8_t units{0};
    uint16_t reserved{0};
};

namespace detail {

constexpr uint16_t read_u16(gsl::span<const uint8_t> bytes, const size_t offset) noexcept
{
    return static_cast<uint16_t>(bytes[offset] | (bytes[offset + 1] << 8));
}

constexpr uint32_t read_u32(gsl::span<const uint8_t> bytes, const size_t offset) noexcept
{
    return static_cast<uint32_t>(bytes[offset]) | (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<uint32_t>(bytes[offset + 3]) << 24);
}

inline void append_u16(std::vector<uint8_t>& out, const uint16_t value)
{
    out.push_back(static_cast<uint8_t>(value & 0xFF));
    out.push_back(static_cast<uint8_t>(value >> 8));
}

inline void append_u32(std::vector<uint8_t>& out, const uint32_t value)
{
    out.push_back(static_cast<uint8_t>(value & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>(value >> 24));
}

} // namespace detail

// Packs character values into a key: earliest first, most significant unit first.
// The value is folded so upper-case corpus entries collapse onto what a scan can
// actually report.
template <typename Traits>
Traits::QCharType pack_units(const std::vector<typename Traits::UnitType>& units)
{
    auto key = static_cast<Traits::QCharType>(0);
    for (const auto unit : units) {
        key = static_cast<Traits::QCharType>((key << (8 * sizeof(typename Traits::UnitType))) |
                                             Traits::fold(unit));
    }
    return key;
}

// Turns corpus entries into the sorted, unique key list a dictionary holds.
template <typename Traits>
std::vector<typename Traits::QCharType>
build_dictionary(const std::vector<std::vector<typename Traits::UnitType>>& entries)
{
    auto keys = std::vector<typename Traits::QCharType>{};
    keys.reserve(entries.size());
    for (const auto& units : entries) {
        if (units.size() == Traits::kUnits) {
            keys.push_back(pack_units<Traits>(units));
        }
    }
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    return keys;
}

// Inverse of Traits::read_unit: writes character values into a buffer the way this
// encoding lays them out in memory. Lives beside kLowByteFirst so the byte order
// is decided in exactly one place.
template <typename Traits>
void append_units(std::vector<uint8_t>& buf, const std::vector<typename Traits::UnitType>& units)
{
    for (const auto unit : units) {
        if constexpr (sizeof(typename Traits::UnitType) == 1) {
            buf.push_back(static_cast<uint8_t>(unit));
        } else if constexpr (Traits::kLowByteFirst) {
            buf.push_back(static_cast<uint8_t>(unit & 0xFF));
            buf.push_back(static_cast<uint8_t>(unit >> 8));
        } else {
            buf.push_back(static_cast<uint8_t>(unit >> 8));
            buf.push_back(static_cast<uint8_t>(unit & 0xFF));
        }
    }
}

template <typename Traits>
std::vector<uint8_t> write_dictionary(const std::vector<typename Traits::QCharType>& keys)
{
    using KeyType = Traits::QCharType;

    const auto header = DictHeader{
        .version = kDictVersion,
        .key_bytes = static_cast<uint8_t>(sizeof(KeyType)),
        .encoding = static_cast<uint8_t>(Traits::kEncoding),
        .entry_count = static_cast<uint32_t>(keys.size()),
        .unit_bytes = static_cast<uint8_t>(sizeof(typename Traits::UnitType)),
        .units = static_cast<uint8_t>(Traits::kUnits),
        .reserved = 0,
    };

    auto out = std::vector<uint8_t>{};
    out.reserve(kDictHeaderBytes + keys.size() * sizeof(KeyType));

    namespace layout = detail::layout;
    detail::append_u32(out, kDictMagic);
    detail::append_u16(out, header.version);
    out.push_back(header.key_bytes);
    out.push_back(header.encoding);
    detail::append_u32(out, header.entry_count);
    out.push_back(header.unit_bytes);
    out.push_back(header.units);
    detail::append_u16(out, header.reserved);
    static_assert(layout::kReserved + 2 == kDictHeaderBytes);

    for (const auto key : keys) {
        for (size_t byte = 0; byte < sizeof(KeyType); ++byte) {
            out.push_back(static_cast<uint8_t>(key >> (8 * byte)));
        }
    }
    return out;
}

enum class DictError : std::uint8_t {
    kTooSmall,
    kBadMagic,
    kUnsupportedVersion,
    kReservedBitsSet,
    kKeyWidthMismatch,
    kEncodingMismatch,
    kUnitShapeMismatch,
    kSizeMismatch,
    kNotStrictlyAscending,
    kTooManyEntries,
    kEmpty,
};

[[nodiscard]]
inline const char* dict_message(const DictError e) noexcept
{
    switch (e) {
    case DictError::kTooSmall:
        return "dictionary shorter than its header";
    case DictError::kBadMagic:
        return "dictionary magic mismatch";
    case DictError::kUnsupportedVersion:
        return "unsupported dictionary version";
    case DictError::kReservedBitsSet:
        return "reserved header bits are set";
    case DictError::kKeyWidthMismatch:
        return "dictionary key width differs from the scanner's";
    case DictError::kEncodingMismatch:
        return "dictionary is for a different encoding";
    case DictError::kUnitShapeMismatch:
        return "dictionary unit size or count differs from the scanner's";
    case DictError::kSizeMismatch:
        return "dictionary size does not match its entry count";
    case DictError::kNotStrictlyAscending:
        return "dictionary keys are not sorted and unique";
    case DictError::kTooManyEntries:
        return "dictionary entry count is not representable";
    case DictError::kEmpty:
        return "dictionary holds no keys";
    }
    return "unknown dictionary error";
}

class DictException : public std::runtime_error {
public:
    explicit DictException(const DictError e)
        : std::runtime_error(std::string("qattern dictionary: ") + dict_message(e)), error_(e)
    {
    }

    [[nodiscard]]
    DictError error() const noexcept
    {
        return error_;
    }

private:
    DictError error_;
};

// Reads and validates a dictionary blob. Every check here exists because the
// consequence of skipping it is either a crash or silently scanning against the
// wrong corpus, which shows up only as wrong detection results much later.
template <typename Traits>
std::vector<typename Traits::QCharType> read_dictionary(gsl::span<const uint8_t> blob)
{
    using KeyType = Traits::QCharType;

    if (blob.size() < kDictHeaderBytes) {
        throw DictException(DictError::kTooSmall);
    }

    namespace layout = detail::layout;
    if (detail::read_u32(blob, layout::kMagic) != kDictMagic) {
        throw DictException(DictError::kBadMagic);
    }

    const auto header = DictHeader{
        .version = detail::read_u16(blob, layout::kVersion),
        .key_bytes = blob[layout::kKeyBytes],
        .encoding = blob[layout::kEncoding],
        .entry_count = detail::read_u32(blob, layout::kEntryCount),
        .unit_bytes = blob[layout::kUnitBytes],
        .units = blob[layout::kUnits],
        .reserved = detail::read_u16(blob, layout::kReserved),
    };

    if (header.version != kDictVersion) {
        throw DictException(DictError::kUnsupportedVersion);
    }
    if (header.reserved != 0) {
        throw DictException(DictError::kReservedBitsSet);
    }
    if (header.key_bytes != sizeof(KeyType)) {
        throw DictException(DictError::kKeyWidthMismatch);
    }
    if (static_cast<Encoding>(header.encoding) != Traits::kEncoding) {
        throw DictException(DictError::kEncodingMismatch);
    }
    if (header.unit_bytes != sizeof(typename Traits::UnitType) || header.units != Traits::kUnits) {
        throw DictException(DictError::kUnitShapeMismatch);
    }

    const auto body = blob.subspan(kDictHeaderBytes);
    if (header.entry_count > body.size() / sizeof(KeyType)) {
        throw DictException(DictError::kTooManyEntries);
    }
    if (body.size() != static_cast<size_t>(header.entry_count) * sizeof(KeyType)) {
        throw DictException(DictError::kSizeMismatch);
    }
    if (header.entry_count == 0) {
        throw DictException(DictError::kEmpty);
    }

    auto keys = std::vector<KeyType>{};
    keys.reserve(header.entry_count);
    for (size_t i = 0; i < header.entry_count; ++i) {
        auto key = KeyType{0};
        for (size_t byte = 0; byte < sizeof(KeyType); ++byte) {
            key |= static_cast<KeyType>(body[i * sizeof(KeyType) + byte]) << (8 * byte);
        }
        keys.push_back(key);
    }

    // Strictly ascending rejects unsorted and duplicated in one pass; FlatHashSet
    // would throw on a duplicate anyway, and a scanner over an unsorted list would
    // silently miss entries.
    for (size_t i = 1; i < keys.size(); ++i) {
        if (keys[i] <= keys[i - 1]) {
            throw DictException(DictError::kNotStrictlyAscending);
        }
    }
    return keys;
}

} // namespace qattern

#endif // QATTERN_DICT_HPP
