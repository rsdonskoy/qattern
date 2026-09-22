# qattern

A header-only C++20 library that detects known n-gram patterns (qchars) in memory regions. It is designed for security tools that scan process memory, shellcode, or PE files for strings of interest — API names, file paths, protocol fragments, IP addresses — by anchoring on short subsequences from a curated dictionary.

## How It Works

Each call to `search_at(offset)` reads a full window of `kUnits` code units ending at `offset`, checks every unit against an alphabet gate, and either probes a flat hash set or returns a **skip hint** telling the caller how many bytes it can jump past. The scanner is stateless — no rolling window, no failure links, no per-byte state accumulation — so a caller that skips positions never desyncs.

The gate checks from the **latest unit backward** and breaks on the first out-of-range byte. On binary data (the dominant workload), the latest byte is almost always non-printable, so the loop exits after one comparison and returns `skip = kWindowBytes`. This reduces scanner calls by up to 4× compared to checking every position.

## Performance

Measured on 1MB workloads (`benchmark/index_compare.cpp`, all three indices run in the same process with match counts forced equal):

| Workload | `std::set` | Sorted binary search | `FlatHashSet` |
|---|---|---|---|
| Random letters | 112.06 | 87.39 | **9.59** |
| Corpus-assembled prose | 151.32 | 98.91 | **14.00** |
| Random bytes (0-255) | 3.95 | 4.30 | 3.82 |
| Control bytes (0x08-0x1F) | 0.67 | 0.67 | 0.67 |
| Build dictionary | 2.21 ms | 0 | **116 µs** |

Units: ns/byte. The last two rows tie because the gate rejects nearly every byte before probing. With the skip hint, binary data scans at ~2.4 ns/byte (2.8× faster than without skip).

## Design

### Encoding strategies (`qchar_traits.hpp`)

Five encodings, each providing four things the scanner cannot infer:

| Strategy | Key type | Units | Alphabet gate | Fold |
|---|---|---|---|---|
| `AnsiTraits` | uint32 | 4 × 8 bit | `[a-zA-Z0-9.]` | `\| 0x20` |
| `U16LeTraits` | uint64 | 4 × 16 bit | ASCII letters/digits/dot | A-Z → a-z |
| `U16BeTraits` | uint64 | 4 × 16 bit | same as LE | same as LE |
| `CnU16LeTraits` | uint32 | 2 × 16 bit | CJK ideographs | identity |
| `Gb2312Traits` | uint32 | 2 × 16 bit | lead 0xA1-0xF7, trail 0xA1-0xFE | identity |

UTF-16LE and UTF-16BE share the same dictionary — only `ReadUnit` (byte order) differs.

### Hash set (`flat_hash_set.hpp`)

Open addressing with linear probing. Slot count is the smallest power of two ≥ 2× the key count (load factor ≤ 0.5). Misses terminate at the first empty slot. An empty-slot sentinel is chosen from outside the key space at construction time, since the key width exactly fills the integer type.

### Scanner (`qchar_scanner.hpp`)

Stateless. One back-to-front loop checks `is_allowed` and folds each unit into the window simultaneously. The latest unit goes to the lowest shift, the earliest to the highest, matching `pack_units`'s MSB-first convention. A bad unit returns immediately with a skip distance — no need to scan the rest.

### Dictionary format (`qattern_dict.hpp`)

16-byte header + little-endian keys, strictly ascending and unique. Two containers share the same byte layout: `.bin` files and generated C++ headers (`std::array<uint8_t, N>`). Both go through one `read_dictionary` parse path.

| Offset | Size | Field | Constraint |
|---|---|---|---|
| 0 | 4 | magic | `'Q','T','N','D'` |
| 4 | 2 | version | must be 1 |
| 6 | 1 | key_bytes | must equal `sizeof(qchar_type)` |
| 7 | 1 | encoding | must equal `Traits::kEncoding` |
| 8 | 4 | entry_count | must match body length, > 0 |
| 12 | 1 | unit_bytes | must equal `sizeof(unit_type)` |
| 13 | 1 | units | must equal `Traits::kUnits` |
| 14 | 2 | reserved | must be 0 |

The encoding tag is mandatory: `Gb2312Traits` and `CnU16LeTraits` have identical key width and unit shape, so feeding a GB2312 dictionary to the CJK strategy would pass all width checks and silently find nothing. The tag turns this into a load-time failure.

## Two Pitfalls

**Whole-window `| 0x20` folding requires an alphabet gate.** OR-ing 0x20 maps `0x0E` → `.` and `0x10`–`0x19` → `0`–`9`. Without gating, 1MB of control bytes produces 7,926 false anchors. The gate is correctness, not optimization.

**Byte-wise folding breaks CJK.** A UTF-16LE ideograph's high bytes (`0x4E`–`0x5A`) overlap ASCII uppercase, so `| 0x20` rewrites one ideograph as another. `CnU16LeTraits::Fold` is the identity function. GB2312 is unaffected (all bytes ≥ 0xA1).

## Corpus

`data/qchars.txt` contains 19,279 English quadgrams, split into:
- `qchars_letters.txt` — 16,777 pure `[a-z]` entries (English word fragments)
- `qchars_numeric.txt` — 2,502 pure `[0-9.]` entries (IP addresses and version numbers)

`data/qchars_cn_utf8.txt` contains 57,881 Chinese bigrams (2 code units = 4 bytes per pattern).

## Build

```
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH=<GSL-install-dir>
cmake --build build && ctest --test-dir build
build/index_compare data/qchars.txt
```

Package: `qattern`, target: `qattern::qattern`, public header: `qattern.hpp`.

```cmake
find_package(qattern REQUIRED)
target_link_libraries(my_app PRIVATE qattern::qattern)
```

## Directory

| Path | Purpose |
|---|---|
| `include/qattern.hpp` | Public entry point |
| `include/qchar_traits.hpp` | Encoding strategies (Ansi, U16Le, U16Be, CnU16Le, Gb2312) |
| `include/qchar_scanner.hpp` | `QcharScanner<Traits>` — stateless scanner with skip |
| `include/flat_hash_set.hpp` | `FlatHashSet<T>` — open-addressing set |
| `include/qattern_dict.hpp` | Dictionary format: pack, write, read, validate |
| `tools/gen_dict.cpp` | Dictionary generator (`.bin` or `.hpp` output) |
| `tools/corpus.hpp` | Corpus reading and per-encoding unitization |
| `tests/encodings.cpp` | Five-encoding round-trip + case folding + timing |
| `tests/dictionary.cpp` | Dictionary round-trip + corruption rejection |
| `benchmark/index_compare.cpp` | Three-way index comparison + gate demonstration |
| `data/` | Corpus files |

## Status

- Five encodings verified: all dictionary keys round-trip (19,279 + 57,881).
- clang-tidy: 0 diagnostics (with live header filter, verified by negative control).
- g++ and MSVC: zero warnings.
- Dictionary artifacts: 848,688 bytes total across five encodings.

## License

Apache 2.0
