#ifndef QCHAR_SCANNER_HPP
#define QCHAR_SCANNER_HPP

#include <cstdint>
#include <iterator>
#include <optional>
#include <stdexcept>

#include <gsl/gsl>

#include "flat_hash_set.hpp"

namespace qattern {

// What a single lookup produced, plus how far the caller may advance before
// calling again. When a byte is out of range, every window that includes it is
// invalid, so the caller can skip past the rightmost out-of-range unit instead
// of re-checking positions that cannot match.
template <typename QCharType> struct SearchResult {
    std::optional<QCharType> match;
    size_t skip;
};

// Stateless qchar detector. Each call reads a full window from the buffer at the
// given offset, checks every unit against the gate, and either probes the
// dictionary or returns a skip that jumps past the rightmost out-of-range unit.
// There is no rolling state, so a caller that skips positions never desyncs.
template <typename Traits> class QcharScanner {
public:
    using QCharType = Traits::QCharType;
    using UnitType = Traits::UnitType;

    explicit QcharScanner(gsl::span<const QCharType> keys);

    void set_buffer(gsl::span<const uint8_t> buffer);

    // Primary interface: probe the window ending at offset, or tell the caller
    // how many bytes it can skip because a unit was out of range.
    [[nodiscard]] SearchResult<QCharType> search_at(size_t offset);

    // Adaptor for callers that drive the scan with span iterators.
    [[nodiscard]] SearchResult<QCharType> search(gsl::span<const uint8_t>::iterator it);

    // Convenience: does a dictionary window end here? Used by the self tests.
    [[nodiscard]] bool contains_window_at(size_t offset) const noexcept;

    [[nodiscard]] const FlatHashSet<QCharType>& dictionary() const noexcept
    {
        return dictionary_;
    }

private:
    static constexpr size_t kByteWidth = sizeof(UnitType);
    static constexpr size_t kWindowBytes = Traits::kUnits * kByteWidth;

    FlatHashSet<QCharType> dictionary_;
    gsl::span<const uint8_t> buffer_{};

    [[nodiscard]] UnitType unit_at(size_t offset) const noexcept;
};

template <typename Traits>
QcharScanner<Traits>::QcharScanner(gsl::span<const QCharType> keys) : dictionary_(keys)
{
}

template <typename Traits> void QcharScanner<Traits>::set_buffer(gsl::span<const uint8_t> buffer)
{
    buffer_ = buffer;
}

template <typename Traits>
QcharScanner<Traits>::UnitType QcharScanner<Traits>::unit_at(const size_t offset) const noexcept
{
    return Traits::read_unit(buffer_, offset);
}

template <typename Traits>
bool QcharScanner<Traits>::contains_window_at(const size_t offset) const noexcept
{
    if (offset + 1 < kWindowBytes) {
        return false;
    }
    if constexpr (kByteWidth > 1) {
        if (offset % kByteWidth != kByteWidth - 1) {
            return false;
        }
    }
    auto window = QCharType{0};
    for (size_t u = Traits::kUnits; u > 0; --u) {
        const auto unit = unit_at(offset - (u - 1) * kByteWidth);
        if (!Traits::is_allowed(unit)) {
            return false;
        }
        window = (window << (8 * kByteWidth)) | Traits::fold(unit);
    }
    return dictionary_.contains(window);
}

template <typename Traits>
SearchResult<typename Traits::QCharType>
QcharScanner<Traits>::search(gsl::span<const uint8_t>::iterator it)
{
    const auto offset = static_cast<size_t>(std::distance(buffer_.begin(), it));
    if (offset + 1 > buffer_.size()) {
        throw std::out_of_range("QcharScanner: iterator outside buffer");
    }
    return search_at(offset);
}

template <typename Traits>
SearchResult<typename Traits::QCharType> QcharScanner<Traits>::search_at(const size_t offset)
{
    if (offset + 1 < kWindowBytes) {
        return {std::nullopt, 1};
    }

    if constexpr (kByteWidth > 1) {
        if (offset % kByteWidth != kByteWidth - 1) {
            return {std::nullopt, 1};
        }
    }

    // One back-to-front pass: check is_allowed and fold in the same loop. The
    // latest unit goes to the lowest shift, the earliest to the highest, so the
    // window matches pack_units' MSB-first convention. If any unit fails the
    // gate, return immediately — the first bad unit from the back is the
    // rightmost, so the skip distance is known without scanning further.
    auto window = QCharType{0};
    for (size_t p = Traits::kUnits; p > 0; --p) {
        const auto unit_offset = offset - (Traits::kUnits - p) * kByteWidth;
        const auto unit = unit_at(unit_offset);
        if (!Traits::is_allowed(unit)) {
            return {std::nullopt, p * kByteWidth};
        }
        window |= static_cast<QCharType>(Traits::fold(unit))
                  << ((Traits::kUnits - p) * 8 * kByteWidth);
    }

    if (dictionary_.contains(window)) {
        return {window, 1};
    }
    return {std::nullopt, 1};
}

} // namespace qattern

#endif // QCHAR_SCANNER_HPP
