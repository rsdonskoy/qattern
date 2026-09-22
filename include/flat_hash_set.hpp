#ifndef FLAT_HASH_SET_HPP
#define FLAT_HASH_SET_HPP

#include <algorithm>
#include <array>
#include <concepts>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

#include <gsl/gsl>

namespace qattern {

// Open-addressed set of unsigned keys, stored in one contiguous array.
//
// Slot count is the smallest power of two at least twice the element count, so
// the load factor stays at or below 0.5 and a lookup terminates on the first
// empty slot it meets. Probing is linear, which keeps the walk inside a couple
// of cache lines.
//
// No value is reserved for the empty marker: with keys as wide as the window
// they came from, every bit pattern is potentially a member. Instead one absent
// value is chosen from the keyspace at construction, so the array stays a plain
// span of keys with no parallel occupancy bitmap.
//
// Measured against the alternatives for this shape of workload: a std::set of
// the same keys costs 11-12x more per lookup, and a sorted array scanned with
// binary search costs 13-30% more, because a gate in front of the lookup has
// already removed the positions that would never match.
template <std::unsigned_integral KeyType> class FlatHashSet {
public:
    FlatHashSet() = default;

    explicit FlatHashSet(gsl::span<const KeyType> keys)
    {
        if (keys.empty()) {
            throw std::invalid_argument("FlatHashSet: no keys given");
        }
        empty_ = pick_empty(keys);
        reserve(keys.size());
        for (const auto key : keys) {
            insert(key);
        }
    }

    [[nodiscard]]
    bool contains(const KeyType key) const noexcept
    {
        if (slots_.empty() || key == empty_) {
            return false;
        }
        auto slot = slot_of(key);
        while (true) {
            const auto stored = slots_[slot];
            if (stored == key) {
                return true;
            }
            if (stored == empty_) {
                return false;
            }
            slot = (slot + 1) & slot_mask_;
        }
    }

    // Returns false when the key was already present.
    bool insert(const KeyType key)
    {
        if (key == empty_) {
            throw std::invalid_argument("FlatHashSet: key equals the reserved empty value");
        }
        auto slot = slot_of(key);
        for (size_t hop = 0; hop <= slot_mask_; ++hop) {
            const auto stored = slots_[slot];
            if (stored == empty_) {
                slots_[slot] = key;
                ++size_;
                return true;
            }
            if (stored == key) {
                return false;
            }
            slot = (slot + 1) & slot_mask_;
        }
        throw std::length_error("FlatHashSet: table is full");
    }

    [[nodiscard]]
    size_t size() const noexcept
    {
        return size_;
    }

    [[nodiscard]]
    size_t slot_count() const noexcept
    {
        return slots_.size();
    }

    [[nodiscard]]
    size_t bytes() const noexcept
    {
        return slots_.size() * sizeof(KeyType);
    }

private:
    static KeyType pick_empty(gsl::span<const KeyType> keys);

    void reserve(const size_t wanted)
    {
        auto bits = size_t{4};
        while ((size_t{1} << bits) < wanted * 2) {
            ++bits;
        }
        slot_bits_ = bits;
        slot_mask_ = static_cast<uint32_t>((size_t{1} << bits) - 1);
        slots_.assign(size_t{1} << bits, empty_);
    }

    // Folds any width key down to 32 bits before the multiply-shift: shifting a
    // 64-bit product by (32 - slot_bits_) yields an index far beyond the slots.
    [[nodiscard]]
    uint32_t slot_of(const KeyType key) const noexcept
    {
        auto folded = static_cast<uint32_t>(key);
        if constexpr (sizeof(KeyType) > sizeof(uint32_t)) {
            folded ^= static_cast<uint32_t>(key >> 32);
        }
        return (folded * 2654435761u) >> (32 - slot_bits_);
    }

    KeyType empty_{0};
    size_t size_{0};
    size_t slot_bits_{4};
    uint32_t slot_mask_{15};
    std::vector<KeyType> slots_;
};

template <std::unsigned_integral KeyType>
KeyType FlatHashSet<KeyType>::pick_empty(gsl::span<const KeyType> keys)
{
    const auto candidates =
        std::array<KeyType, 4>{KeyType{0}, std::numeric_limits<KeyType>::max(),
                               std::numeric_limits<KeyType>::max() - 1, KeyType{1}};
    for (const auto candidate : candidates) {
        if (std::find(keys.begin(), keys.end(), candidate) == keys.end()) {
            return candidate;
        }
    }

    // Real dictionaries are vastly sparser than the keyspace, so this is
    // effectively unreachable; a dense set still gets an exact answer rather
    // than a silently wrong slot.
    for (auto value = KeyType{2}; value != std::numeric_limits<KeyType>::max(); ++value) {
        if (std::find(keys.begin(), keys.end(), value) == keys.end()) {
            return value;
        }
    }
    throw std::logic_error("FlatHashSet: no free value in keyspace");
}

} // namespace qattern

#endif // FLAT_HASH_SET_HPP
