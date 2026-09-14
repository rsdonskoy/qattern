// Under the same raw dword keys + |mask folding + alphabet gating, compare three indexes:
//   set    -- the old implementation's std::set<uint32_t> (red-black tree, per-element allocation)
//   bsearch-- constexpr ascending array + branch-free binary search (zero construction, 77KB)
//   hash   -- flat open addressing (requires construction, 512KB)
// All three must report exactly the same hit count on the same buffer, otherwise stop before
// comparing performance.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <fstream>
#include <iterator>
#include <random>
#include <set>
#include <string>
#include <vector>

#include <flat_hash_set.hpp>

using namespace qattern;

using Byte = uint8_t;

constexpr uint32_t kFoldMask = 0x20202020u;
constexpr size_t kWindowBytes = 4;

// ---- dictionary and gate table ----

std::vector<uint32_t> load_dict(const std::string& path)
{
    auto vals = std::vector<uint32_t>{};
    auto in = std::ifstream(path);
    if (!in) {
        std::fprintf(stderr, "cannot open %s\n", path.c_str());
        std::exit(2);
    }
    auto line = std::string{};
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.size() != kWindowBytes) {
            continue;
        }
        auto v = uint32_t{0};
        for (const auto c : line) {
            v = (v << 8) | static_cast<unsigned char>(c);
        }
        vals.push_back(v);
    }
    std::sort(vals.begin(), vals.end());
    vals.erase(std::unique(vals.begin(), vals.end()), vals.end());
    return vals;
}

// Alphabet gating: accept only . 0-9 a-z A-Z. Without it, 0x0E/0x10-0x19 would be OR-folded into
// '.'/digits.
std::array<bool, 256> build_gate()
{
    auto gate = std::array<bool, 256>{};
    gate['.'] = true;
    for (char c = '0'; c <= '9'; ++c) {
        gate[static_cast<unsigned char>(c)] = true;
    }
    for (char c = 'a'; c <= 'z'; ++c) {
        gate[static_cast<unsigned char>(c)] = true;
    }
    for (char c = 'A'; c <= 'Z'; ++c) {
        gate[static_cast<unsigned char>(c)] = true;
    }
    return gate;
}

// ---- three indexes, one probe interface ----

struct SetIndex {
    std::set<uint32_t> keys;
    explicit SetIndex(const std::vector<uint32_t>& d)
    {
        keys.insert(d.begin(), d.end());
    }
    bool probe(uint32_t k) const
    {
        return keys.find(k) != keys.end();
    }
    size_t bytes() const
    {
        return 0;
    } // on the heap, reported separately
};

struct SortedIndex {
    const std::vector<uint32_t>* keys;
    explicit SortedIndex(const std::vector<uint32_t>& d) : keys(&d)
    {
    }
    bool probe(uint32_t k) const
    {
        return std::binary_search(keys->cbegin(), keys->cend(), k);
    }
    size_t bytes() const
    {
        return keys->size() * sizeof(uint32_t);
    }
};

// Directly wraps the repo's shipped FlatHashSet, ensuring the test exercises the implementation
// that ships.
struct HashIndex {
    FlatHashSet<uint32_t> index;

    explicit HashIndex(const std::vector<uint32_t>& d) : index(gsl::span<const uint32_t>(d))
    {
    }

    [[nodiscard]]
    bool probe(uint32_t k) const
    {
        return index.contains(k);
    }

    [[nodiscard]]
    size_t bytes() const
    {
        return index.bytes();
    }
};

// ---- scan: byte-by-byte feed (same shape as libaveng's pull contract) ----

template <typename Index>
size_t scan(const Index& idx, const std::vector<Byte>& buf, const std::array<bool, 256>& gate)
{
    auto window = uint32_t{0};
    auto run = size_t{0};
    auto hits = size_t{0};
    for (const auto b : buf) {
        window = (window << 8) | b;
        run = gate[b] ? run + 1 : 0;
        if (run >= kWindowBytes && idx.probe(window | kFoldMask)) {
            ++hits;
        }
    }
    return hits;
}

// ---- workloads ----

// Assemble ~1MB of text by randomly concatenating corpus quadgrams.
// Every source quadgram is a dictionary hit; overlapping windows at offsets
// 1-3 within each quadgram may or may not form another valid entry.
std::vector<Byte> make_prose_from_corpus(const std::string& path, std::mt19937& rng)
{
    auto in = std::ifstream(path);
    auto lines = std::vector<std::string>{};
    auto line = std::string{};
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.size() == kWindowBytes) {
            lines.push_back(std::move(line));
        }
    }

    auto out = std::vector<Byte>{};
    out.reserve(1u << 20);
    auto pick = std::uniform_int_distribution<size_t>{0, lines.size() - 1};
    while (out.size() < (1u << 20)) {
        const auto& quad = lines[pick(rng)];
        out.insert(out.end(), quad.begin(), quad.end());
    }
    return out;
}

std::vector<Byte> make_random(size_t n, int mode, std::mt19937& rng)
{
    auto out = std::vector<Byte>{};
    out.reserve(n);
    auto raw = std::uniform_int_distribution<int>{0, 255};
    auto low = std::uniform_int_distribution<int>{0, 25};
    auto ctl = std::uniform_int_distribution<int>{0x08, 0x1F}; // includes 0x0E and 0x10-0x19
    for (size_t i = 0; i < n; ++i) {
        if (mode == 0) {
            out.push_back(static_cast<Byte>(raw(rng)));
        } else if (mode == 1) {
            out.push_back(static_cast<Byte>('a' + low(rng)));
        } else {
            out.push_back(static_cast<Byte>(ctl(rng)));
        }
    }
    return out;
}

template <typename F> double time_ns(F&& f, int reps)
{
    using Clock = std::chrono::steady_clock;
    auto best = 1e30;
    for (int r = 0; r < reps; ++r) {
        const auto t0 = Clock::now();
        const auto sink = f();
        const auto t1 = Clock::now();
        if (static_cast<size_t>(sink) == SIZE_MAX) {
            std::fprintf(stderr, "sentinel\n");
        }
        best = std::min(best, std::chrono::duration<double, std::nano>(t1 - t0).count());
    }
    return best;
}

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
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <qchars.txt>\n", argv[0]);
        return 2;
    }
    const auto dict = load_dict(argv[1]);
    const auto gate = build_gate();
    std::printf("dict=%zu\n", dict.size());

    auto rng = std::mt19937{20260911u};
    const auto prose = make_prose_from_corpus(argv[1], rng);
    const auto workloads = std::vector<std::pair<const char*, std::vector<Byte>>>{
        {"random", make_random(1u << 20, 0, rng)},
        {"letters", make_random(1u << 20, 1, rng)},
        {"prose", prose},
        {"control", make_random(1u << 20, 2, rng)}, // only the gate prevents false positives here
    };

    const auto t_set = time_ns(
        [&] {
            SetIndex idx(dict);
            return idx.probe(0);
        },
        1);
    const auto t_hash = time_ns(
        [&] {
            HashIndex idx(dict);
            return idx.probe(0);
        },
        3);
    std::printf("construct setNs=%.0f hashNs=%.0f sortedNs=0\n", t_set, t_hash);

    for (const auto& [name, buf] : workloads) {
        SetIndex si(dict);
        SortedIndex bi(dict);
        HashIndex hi(dict);
        const auto hs = scan(si, buf, gate);
        const auto hb = scan(bi, buf, gate);
        const auto hh = scan(hi, buf, gate);
        if (hs != hb || hs != hh) {
            std::fprintf(stderr, "MISMATCH on %s: set=%zu sorted=%zu hash=%zu\n", name, hs, hb, hh);
            return 1;
        }
        const double n = static_cast<double>(buf.size());
        const auto ns = time_ns([&] { return scan(si, buf, gate); }, 5) / n;
        const auto nb = time_ns([&] { return scan(bi, buf, gate); }, 5) / n;
        const auto nh = time_ns([&] { return scan(hi, buf, gate); }, 5) / n;
        std::printf("%-8s bytes=%zu hits=%zu | set=%.2f sorted=%.2f hash=%.2f ns/B | "
                    "sorted/hash=%.2fx set/hash=%.2fx\n",
                    name, buf.size(), hs, ns, nb, nh, ns / nb, ns / nh);
    }
    std::printf("footprint sorted=%zuKB hash=%zuKB\n", dict.size() * 4 / 1024,
                HashIndex(dict).bytes() / 1024);

    // Is the gate truly necessary? Remove it and see whether the control-byte workload lights up.
    auto ungated = size_t{0};
    {
        auto window = uint32_t{0};
        HashIndex hi(dict);
        for (const auto& [name, buf] : workloads) {
            if (std::string(name) != "control") {
                continue;
            }
            for (const auto b : buf) {
                window = (window << 8) | b;
                if (hi.probe(window | kFoldMask)) {
                    ++ungated;
                }
            }
        }
    }
    std::printf("hits_without_gate_on_control_load=%zu (gated=%zu)\n", ungated,
                scan(HashIndex(dict), workloads[3].second, gate));
    return 0;
}
