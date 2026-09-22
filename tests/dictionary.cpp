// Round-trip and rejection tests for the on-disk dictionary format.
//
// Forward: corpus -> build_dictionary -> write_dictionary -> read_dictionary must match key for
// key, and a scanner built from the reloaded keys must hit at exactly the same positions as one
// built from the in-memory keys. Reverse: every corruption must throw, and throw the right error.
// Assert the error kind rather than merely "threw", because the whole point of the format is to
// turn silent errors like feeding the wrong dictionary into a loud failure.
#include <qattern_dict.hpp>
#include <qchar_scanner.hpp>

#include <cstdint>
#include <cstdio>
#include <exception>
#include <string>
#include <vector>

#include "corpus.hpp"

using namespace qattern;

namespace {

int failures = 0;

void expect(const bool ok, const char* what)
{
    if (!ok) {
        std::printf("  FAIL %s\n", what);
        ++failures;
    }
}

template <typename Traits> void check_round_trip(const std::string& path)
{
    using Key = Traits::QCharType;
    const std::string name = Traits::kName;
    const auto loaded = corpus::load<Traits>(path);
    const auto keys = build_dictionary<Traits>(loaded.entries);
    const auto blob = write_dictionary<Traits>(keys);

    const auto failures_before = failures;
    try {
        const auto read_back = read_dictionary<Traits>(gsl::span<const uint8_t>(blob));
        expect(read_back == keys, (name + ": round trip keys equal").c_str());

        // Scan-result consistency: build a scanner from the reloaded keys and compare hit positions
        // across the real corpus buffer.
        auto buf = std::vector<uint8_t>{};
        for (const auto& units : loaded.entries) {
            append_units<Traits>(buf, units);
        }
        const auto view = gsl::span<const uint8_t>(buf);

        auto memory = QcharScanner<Traits>(gsl::span<const Key>(keys));
        auto reloaded = QcharScanner<Traits>(gsl::span<const Key>(read_back));
        memory.set_buffer(view);
        reloaded.set_buffer(view);

        auto same = true;
        for (size_t i = 0; i < buf.size(); ++i) {
            if (memory.search_at(i).match != reloaded.search_at(i).match) {
                same = false;
            }
        }
        expect(same, (name + ": scanner from reloaded keys matches in-memory keys").c_str());
    } catch (const std::exception& e) {
        std::printf("  FAIL %s: unexpected throw: %s\n", name.c_str(), e.what());
        ++failures;
    }

    if (failures == failures_before) {
        std::printf("  ok   %-11s keys=%-6zu blob=%zu bytes\n", name.c_str(), keys.size(),
                    blob.size());
    }
}

template <typename Traits>
DictError expect_reject(const std::vector<uint8_t>& blob, DictError want, const char* label)
{
    try {
        static_cast<void>(read_dictionary<Traits>(gsl::span<const uint8_t>(blob)));
        std::printf("  FAIL %s: accepted\n", label);
        ++failures;
        return want;
    } catch (const DictException& e) {
        expect(e.error() == want, label);
        if (e.error() != want) {
            std::printf("       got: %s\n", dict_message(e.error()));
        }
        return e.error();
    } catch (const std::exception& e) {
        std::printf("  FAIL %s: wrong exception %s\n", label, e.what());
        ++failures;
        return want;
    }
}

} // namespace

static int run(int argc, char** argv);

// An exception escaping main turns a rejected dictionary into an opaque
// abnormal exit, hiding the reason behind an unusable status code.
int main(int argc, char** argv)
{
    try {
        return run(argc, argv);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "failed: %s\n", e.what());
        return 1;
    }
}

static int run(int argc, char** argv)
{
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s <qchars.txt> <qchars_cn_utf8.txt>\n", argv[0]);
        return 2;
    }
    const auto ansi_path = std::string(argv[1]);
    const auto cjk_path = std::string(argv[2]);

    std::printf("dictionary round trip:\n");
    check_round_trip<AnsiTraits>(ansi_path);
    check_round_trip<U16LeTraits>(ansi_path);
    check_round_trip<U16BeTraits>(ansi_path);
    check_round_trip<CnU16LeTraits>(cjk_path);
    check_round_trip<Gb2312Traits>(cjk_path);

    std::printf("malformed input rejection:\n");
    const auto ansi_keys =
        build_dictionary<AnsiTraits>(corpus::load<AnsiTraits>(ansi_path).entries);
    const auto cjk_keys =
        build_dictionary<CnU16LeTraits>(corpus::load<CnU16LeTraits>(cjk_path).entries);
    const auto good = write_dictionary<AnsiTraits>(ansi_keys);
    const auto cjk_blob = write_dictionary<CnU16LeTraits>(cjk_keys);

    expect(good.size() == kDictHeaderBytes + ansi_keys.size() * 4, "ansi blob size");

    // A round trip only proves write and read agree with each other; if both misread the same
    // layout, it still passes. The golden bytes pin offsets and field widths outside the
    // implementation. The entry count is expanded from the actual key count, so corpus changes do
    // not cause spurious failures.
    const auto count = static_cast<uint32_t>(ansi_keys.size());
    const std::vector<uint8_t> golden_header{
        0x51,
        0x54,
        0x4E,
        0x44, // 'Q','T','N','D'
        0x01,
        0x00,                               // version 1, little-endian
        0x04,                               // key_bytes = sizeof(uint32)
        0x00,                               // encoding = kAnsi
        static_cast<uint8_t>(count & 0xFF), // entry_count, little-endian
        static_cast<uint8_t>((count >> 8) & 0xFF),
        static_cast<uint8_t>((count >> 16) & 0xFF),
        static_cast<uint8_t>(count >> 24),
        0x01, // unit_bytes = sizeof(uint8)
        0x04, // units
        0x00,
        0x00 // reserved
    };
    expect(std::equal(golden_header.begin(), golden_header.end(), good.begin()),
           "header golden bytes");

    auto truncated = good;
    truncated.pop_back();
    // The entry-count check must run before the size check, otherwise it may allocate against a
    // forged count, so truncation hits this guard first.
    expect_reject<AnsiTraits>(truncated, DictError::kTooManyEntries, "truncated body");

    auto trailing = good;
    trailing.push_back(0);
    expect_reject<AnsiTraits>(trailing, DictError::kSizeMismatch, "one extra trailing byte");

    auto short_blob = std::vector<uint8_t>(8, 0);
    expect_reject<AnsiTraits>(short_blob, DictError::kTooSmall, "shorter than header");

    auto bad_magic = good;
    bad_magic[0] = static_cast<uint8_t>(bad_magic[0] ^ 0xFF);
    expect_reject<AnsiTraits>(bad_magic, DictError::kBadMagic, "bad magic");

    auto bad_version = good;
    bad_version[4] = 99;
    expect_reject<AnsiTraits>(bad_version, DictError::kUnsupportedVersion, "unknown version");

    auto reserved_set = good;
    reserved_set[14] = 1;
    expect_reject<AnsiTraits>(reserved_set, DictError::kReservedBitsSet, "reserved bits set");

    auto bad_key_width = good;
    bad_key_width[6] = 8;
    expect_reject<AnsiTraits>(bad_key_width, DictError::kKeyWidthMismatch,
                              "key width 8 for uint32");

    // The key case: the two have identical key width and unit shape, differing only in encoding
    // semantics, so a width check cannot tell them apart.
    expect_reject<CnU16LeTraits>(good, DictError::kEncodingMismatch, "ansi blob into cn-utf16le");
    expect_reject<Gb2312Traits>(cjk_blob, DictError::kEncodingMismatch,
                                "cn-utf16le blob into gb2312");

    auto bad_units = good;
    bad_units[13] = 3;
    expect_reject<AnsiTraits>(bad_units, DictError::kUnitShapeMismatch, "units field 3 != 4");

    auto swapped = good;
    const auto first = kDictHeaderBytes;
    std::ranges::swap_ranges(gsl::span<uint8_t>(swapped).subspan(first, 4),
                             gsl::span<uint8_t>(swapped).subspan(first + 4, 4));
    expect_reject<AnsiTraits>(swapped, DictError::kNotStrictlyAscending, "keys out of order");

    // Overwrite the second key in place to equal the first: entry count and byte count are
    // unchanged, only ordering is broken, so this case truly tests strict-ascending rejection of
    // duplicate keys, not size.
    auto duplicated = good;
    for (size_t b = 0; b < 4; ++b) {
        duplicated[first + 4 + b] = duplicated[first + b];
    }
    expect_reject<AnsiTraits>(duplicated, DictError::kNotStrictlyAscending, "duplicate keys");

    auto huge_count = good;
    huge_count[8] = 0xFF;
    huge_count[9] = 0xFF;
    huge_count[10] = 0xFF;
    huge_count[11] = 0xFF;
    expect_reject<AnsiTraits>(huge_count, DictError::kTooManyEntries, "entry count beyond body");

    std::printf("\n%s\n", failures == 0 ? "DICTIONARY FORMAT OK" : "DICTIONARY FAILURES");
    return failures == 0 ? 0 : 1;
}
