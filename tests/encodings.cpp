// Per-encoding self-check and timing.
//
// The main invariant is round-trip: every key in the dictionary, written into a buffer in that
// encoding's byte order, must be scanned out. Missing any key signals a problem with unit assembly,
// folding, or phase — that is how key-ordering errors on the Chinese side were caught.
//
// Two more things are covered: case folding must let uppercase input hit a lowercase dictionary;
// and the per-byte cost of the UTF-16LE(uint64) path, which has never been measured end to end.
#include <qattern_dict.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <random>
#include <string>
#include <vector>

#include "corpus.hpp"

using namespace qattern;

namespace {

// ---- round-trip self-check ----

template <typename Traits>
int check_round_trip(const std::vector<std::vector<typename Traits::UnitType>>& entries)
{
    const auto keys = build_dictionary<Traits>(entries);
    if (keys.empty()) {
        std::printf("  %-11s NO ENTRIES (skipped)\n", Traits::kName);
        return 0;
    }

    auto scanner = QcharScanner<Traits>(gsl::span<const typename Traits::QCharType>(keys));
    auto missed = size_t{0};
    auto reported = 0;

    for (const auto& units : entries) {
        const auto want = pack_units<Traits>(units);

        // Pad two illegal bytes before and after, confirming that phase and gating do not depend on
        // buffer boundaries.
        auto buf = std::vector<uint8_t>{0xFF, 0xFF};
        append_units<Traits>(buf, units);
        buf.push_back(0xFF);
        buf.push_back(0xFF);

        scanner.set_buffer(gsl::span<const uint8_t>(buf));
        auto found = false;
        for (size_t o = 0; o < buf.size(); ++o) {
            if (scanner.search_at(o).match == want) {
                found = true;
            }
        }
        if (!found && reported < 3) {
            ++reported;
            std::printf("    %s missed:", Traits::kName);
            for (const auto u : units) {
                std::printf(" %04X", static_cast<unsigned>(u));
            }
            std::printf("\n");
        }
        missed += found ? 0 : 1;
    }

    std::printf("  %-11s entries=%-6zu keys=%-6zu missed=%zu %s\n", Traits::kName, entries.size(),
                keys.size(), missed, missed == 0 ? "OK" : "FAIL");
    return missed == 0 ? 0 : 1;
}

// ---- case folding: uppercase input must hit a lowercase dictionary ----

template <typename Traits> int check_case_fold(const std::vector<typename Traits::UnitType>& upper)
{
    auto lower = upper;
    for (auto& u : lower) {
        u = static_cast<typename Traits::UnitType>(u + 0x20);
    }

    // The dictionary holds only lowercase entries while the buffer is fed uppercase input; both
    // must fold to the same key.
    const auto keys = build_dictionary<Traits>({lower});
    auto scanner = QcharScanner<Traits>(gsl::span<const typename Traits::QCharType>(keys));

    auto buf = std::vector<uint8_t>{};
    append_units<Traits>(buf, upper);
    scanner.set_buffer(gsl::span<const uint8_t>(buf));

    auto ok = false;
    for (size_t o = 0; o < buf.size(); ++o) {
        if (scanner.search_at(o).match.has_value()) {
            ok = true;
        }
    }
    std::printf("  %-11s uppercase input hits lowercase dict: %s\n", Traits::kName,
                ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}

// ---- timing ----

template <typename Traits>
double time_scan(const std::vector<typename Traits::QCharType>& keys,
                 const std::vector<uint8_t>& buf)
{
    auto scanner = QcharScanner<Traits>(gsl::span<const typename Traits::QCharType>(keys));
    using Clock = std::chrono::steady_clock;
    auto best = 1e30;
    auto hits = size_t{0};
    for (int r = 0; r < 5; ++r) {
        scanner.set_buffer(gsl::span<const uint8_t>(buf));
        const auto t0 = Clock::now();
        for (size_t o = 0; o < buf.size(); ++o) {
            if (scanner.search_at(o).match.has_value()) {
                ++hits;
            }
        }
        const auto t1 = Clock::now();
        best = std::min(best, std::chrono::duration<double, std::nano>(t1 - t0).count());
    }
    if (hits == 0) {
        std::fprintf(stderr, "  %s: zero hits, timing is vacuous\n", Traits::kName);
    }
    return best / static_cast<double>(buf.size());
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

    const auto ansi = corpus::load<AnsiTraits>(ansi_path);
    const auto le = corpus::load<U16LeTraits>(ansi_path);
    const auto be = corpus::load<U16BeTraits>(ansi_path);
    const auto cjk = corpus::load<CnU16LeTraits>(cjk_path);
    const auto gb = corpus::load<Gb2312Traits>(cjk_path);
    std::printf("corpus: ansi=%zu cjk=%zu gb2312=%zu (outside GB2312=%zu)\n", ansi.entries.size(),
                cjk.entries.size(), gb.entries.size(), gb.skipped);

    auto failures = 0;
    std::printf("round-trip per encoding:\n");
    failures += check_round_trip<AnsiTraits>(ansi.entries);
    failures += check_round_trip<U16LeTraits>(le.entries);
    failures += check_round_trip<U16BeTraits>(be.entries);
    failures += check_round_trip<CnU16LeTraits>(cjk.entries);
    failures += check_round_trip<Gb2312Traits>(gb.entries);

    std::printf("case folding:\n");
    failures += check_case_fold<AnsiTraits>(std::vector<uint8_t>{'D', 'A', 'T', 'A'});
    failures += check_case_fold<U16LeTraits>(std::vector<uint16_t>{'D', 'A', 'T', 'A'});

    std::printf("timing (1MB, ns/byte):\n");
    {
        auto rng = std::mt19937{20260911u};
        auto pick = std::uniform_int_distribution<int>{0, 25};
        auto letters = std::vector<uint8_t>{};
        letters.reserve(1u << 20);
        for (size_t i = 0; i < (1u << 20); ++i) {
            letters.push_back(static_cast<uint8_t>('a' + pick(rng)));
        }

        // The CJK buffer must contain real Han characters, otherwise it scans ASCII and the timing
        // is meaningless.
        auto cjk_buf = std::vector<uint8_t>{};
        for (const auto& e : cjk.entries) {
            append_units<CnU16LeTraits>(cjk_buf, e);
        }
        auto le_buf = std::vector<uint8_t>{};
        for (const auto b : letters) {
            le_buf.push_back(b);
            le_buf.push_back(0);
        }
        auto be_buf = std::vector<uint8_t>{};
        for (const auto b : letters) {
            be_buf.push_back(0);
            be_buf.push_back(b);
        }

        const auto ansi_keys = build_dictionary<AnsiTraits>(ansi.entries);
        const auto le_keys = build_dictionary<U16LeTraits>(le.entries);
        const auto be_keys = build_dictionary<U16BeTraits>(be.entries);
        const auto cn_keys = build_dictionary<CnU16LeTraits>(cjk.entries);

        std::printf("  ansi        %6.2f  uint32 = 4 units x 8 bit\n",
                    time_scan<AnsiTraits>(ansi_keys, letters));
        std::printf("  utf16le     %6.2f  uint64 = 4 units x 16 bit\n",
                    time_scan<U16LeTraits>(le_keys, le_buf));
        std::printf("  utf16be     %6.2f  uint64, BE byte order over the same dictionary\n",
                    time_scan<U16BeTraits>(be_keys, be_buf));
        std::printf("  cn-utf16le  %6.2f  uint32 = 2 units x 16 bit\n",
                    time_scan<CnU16LeTraits>(cn_keys, cjk_buf));
        std::printf("  LE/BE share one dictionary: %s\n", le_keys == be_keys ? "yes" : "no");
    }

    std::printf("\n%s\n", failures == 0 ? "ALL ENCODINGS PASS" : "kEncoding FAILURES PRESENT");
    return failures == 0 ? 0 : 1;
}
