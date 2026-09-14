# qattern

A header-only library that finds known n-gram patterns in memory regions. It is designed for security tools that scan process memory, shellcode, or PE files for strings of interest — API names, file paths, protocol fragments, IP addresses — by anchoring on short subsequences (qchars) that appear in a curated dictionary.

The scanner is stateless and returns a skip hint: when a byte is outside the dictionary's alphabet, the caller can jump past the entire window instead of re-checking positions that cannot match. On binary data (the dominant workload), this reduces the number of scanner calls by up to 4x.

This project takes over the `qattern` package name: CMake package is `qattern`, target is `qattern::qattern`, public header is `qattern.hpp`. Downstream `find_package(qattern REQUIRED)` and `#include "qattern.hpp"` require no changes — only the type name changes (`AcAutomaton32` → `QcharScanner<AnsiTraits>`). What it replaces is the previous Aho-Corasick implementation: no trie, no failure links, no deserialization.

## Why Replace AC

Patterns are of equal length (one qchar is exactly `sizeof(qchar_type)` bytes), which invalidates both signature features of AC:
failure-link output merging is a no-op under equal length; trie prefix sharing is at most 3 levels deep, yet every input byte pays
1–4 state transitions. Furthermore, the vast majority of bytes in a memory region are not in the alphabet at all, and AC still pays a
transition cost for each of them.

Measured (1MB workload, byte-by-byte pull path, `benchmark/index_compare.cpp`, all three parties run in the same process with match
counts forced equal; the AC flattened version in the last column comes from the deprecated libqattern, measured separately):

| Workload | `std::set` | Ascending binary search | `FlatHashSet` (this implementation) | AC flattened version |
|---|---|---|---|---|
| Random legal alphabet | 94.13 | 73.51 | **7.47** | 49.2 |
| English prose | 19.10 | 15.33 | **1.66** | 22.7 |
| Random bytes | 3.37 | 3.85 | 3.51 | 15.6 |
| Pure control bytes | 0.61 | 0.61 | 0.61 | — |
| Build dictionary | 1.72 ms | 0 | 85 µs | 5.1 ms |

Units are ns/byte. The three-way tie in the last two rows is not because the indices are indistinguishable, but because those two
workloads are almost entirely gated out on the spot and never reach probing (1MB of random bytes yields only 63 matches). The rows
that actually compare indices are the first two.

Although ascending binary search has zero construction cost and a footprint of only 75KB, it is 1.25–1.28x slower than
`FlatHashSet`: the gate already eliminates positions that should not be probed, and running 15 layers of variable-time branching on
the remaining probe points does not pay off.

## Design

A qchar is a word assembled from several code units in "first-appearing occupies the high bits" order. The encoding strategy is
entirely concentrated in `include/qchar_traits.hpp`: each strategy provides only the three things the scanner cannot infer on its
own — **which windows are worth probing** (`IsAllowed`), **how to fold case** (`Fold`), and **how a code unit is assembled from
memory** (`ReadUnit` and `kLowByteFirst`, so UTF-16LE and UTF-16BE share the same dictionary).

The hash set is an encoding-agnostic generic component, placed separately in `include/flat_hash_set.hpp`: `FlatHashSet<T>`
performs open addressing with linear probing for unsigned keys, with the number of slots being the smallest power of two no less
than twice the element count, load factor ≤ 0.5, and misses terminating at the first empty slot. Because the key width exactly
equals the window width, no value can freely serve as the empty-slot marker, so during construction a value outside the
dictionary's key space is chosen. `include/qchar_scanner.hpp` is left with only the byte-by-byte rolling window.

## Two Pitfalls You Must Know

**Whole-window folding like `| 0x20202020` must be paired with alphabet gating.** OR-ing a 0x20 turns `0x0E` into `.`,
turns `0x10`–`0x19` into `0`–`9`. Without gating, 1MB of pure control-byte workload lights up **7,926** anchors, all falling
on IP-address / version-number dictionaries; with gating it is **0**. So `IsAllowed` is not an optimization — it is part of
correctness, and saving probing is merely a side benefit.

**Chinese text cannot be folded byte-wise.** `libqattern`'s `NormalizeByte` folds `0x41..0x5A`, and the high-byte range of UTF-16LE
Chinese characters `0x4E..0x9F` overlaps with it, so U+4E2D (the ideograph for "middle") gets rewritten to U+6E2D (a rare ideograph). In testing, 1,391 of 4,913
code units were rewritten, causing 118 node collisions out of 57,881 patterns, and **349** false anchors in the Chinese differential
test trace back to this. Chinese characters have no case, so `CnU16LeTraits::Fold` is the identity function. GB2312 is unaffected:
its bytes are all ≥ 0xA1, falling outside the folding range.

Wide encodings also require that windows be taken at code-unit phase. Byte-granular AC matches cross-character sequences like
`[trail, lead, trail, lead]`, 31 instances measured; this implementation only makes a determination at positions where a code unit
is completely bounded.

## Dictionary On-Disk Format

`include/qattern_dict.hpp` defines a 16-byte header + key region. Keys are a sequence of **folded** character values, stored in
little-endian, each at its natural width, strictly ascending and unique. The build artifacts come in two containers: `.bin` raw
files, and C++ headers generated by `gen_dict` (the content is the same byte string, wrapped in `std::array<uint8_t, N>`) — both
go through the same `ReadDictionary` parsing path, so there is no "the embedded version skips some validation" branch.

| Offset | Size | Field | Constraint |
|---|---|---|---|
| 0 | 4 | magic | `'Q','G','D','F'` |
| 4 | 2 | version | must be 1 |
| 6 | 1 | key_bytes | must equal `sizeof(qchar_type)` |
| 7 | 1 | encoding | must equal `Traits::kEncoding` |
| 8 | 4 | entry_count | must match body length exactly, and > 0 |
| 12 | 1 | unit_bytes | must equal `sizeof(unit_type)` |
| 13 | 1 | units | must equal `Traits::kUnits` |
| 14 | 2 | reserved | must be 0, reserved for future extension |

The header **does not overlay a packed struct**: fields are read and written by explicit little-endian at the named offsets above;
`DictHeader` is just a plain value type after parsing. An earlier version `memcpy`'d a `#pragma pack(1)` struct, which was
interpreted in host byte order, while the key region is explicitly little-endian — the two disagreed, so on big-endian platforms
keys would parse correctly while the header would be entirely wrong. Now the entire artifact has only one byte-order convention.

`ReadDictionary<Traits>` rejects rather than tolerates each field, not out of defensive-programming fastidiousness: **the encoding
tag is mandatory**. `Gb2312Traits` and `CnU16LeTraits` have exactly the same key width and unit shape (both `uint32 = 2 × 16bit`),
so feeding a GB2312 dictionary to the CJK UTF-16LE strategy passes all width checks, then silently finds nothing at runtime.
The tag turns this kind of error into a load-time failure.

Generation and loading:

```
gen_dict ansi        data/qchars.txt          out/qattern_dict_ansi.hpp
gen_dict cn-utf16le  data/qchars_cn_utf8.txt  out/qattern_dict_cn.bin --bin
```

Size comparison against the old library's serialized AC set (smaller is better; this is what goes into the injected DLL):

| Dictionary | This format | Old AC blob |
|---|---|---|
| ansi | 77,132 | 451,929 |
| utf16le | 154,248 | 903,837 |
| utf16be | 154,248 | — (old library has no such encoding) |
| cn-utf16le | 231,540 | 2,040,897 |
| gb2312 | 231,520 | 1,796,691 |
| Five artifacts total | 848,688 | 5,193,353 |

The **key contents of `utf16le` and `utf16be` are identical** (same ASCII corpus; the only difference is how units are assembled
in memory), but because the encoding tag is checked at load time, one artifact cannot serve both strategies — each must be
generated separately. This is the cost of the tag, not redundant waste: saving 154,248 bytes would simultaneously lose the
guarantee that "feeding the wrong dictionary always errors out."

Loading only needs to hand the key array to `QcharScanner`; constructing the hash table takes 85 µs measured, so there is no need
to pre-generate the hash table itself.

## Directory

- `include/qchar_traits.hpp` — five encoding strategies: `AnsiTraits`, `U16LeTraits`, `U16BeTraits`,
  `CnU16LeTraits`, `Gb2312Traits`. Each strategy provides `IsAllowed` (gating), `Fold` (folding),
  `ReadUnit` (assembling a unit from memory), and `kLowByteFirst` (byte order, reused by the buffering side).
- `include/qattern.hpp` — public entry point; consumers only need to include this
- `include/qattern_dict.hpp` — dictionary format: packing, writing, reading back, and validation
- `include/flat_hash_set.hpp` — `FlatHashSet<T>`, an encoding-agnostic generic open-addressing set
- `include/qchar_scanner.hpp` — `QcharScanner`
- `tools/corpus.hpp` — corpus reading and per-encoding unitization (generators and tests share one implementation)
- `tools/gen_dict.cpp` — dictionary generator
- `tests/encodings.cpp` — five-encoding round-trip self-check + case folding + per-encoding timing
- `tests/dictionary.cpp` — dictionary format round-trip, and each kind of corruption must throw the specific corresponding error
- `benchmark/index_compare.cpp` — timing of `std::set` / ascending binary search / `FlatHashSet` three-way, and demonstration of ungated consequences
- `data/` — corpus copies, `qchars.txt` and cleaned `qchars_cn_utf8.txt`

The original `tests/equivalence.cpp` (position-by-position differential against `AcAutomaton32`) was removed along with the
deprecation of libqattern. Its migration purpose was fulfilled at the time — it was exactly that differential test that caught the
out-of-bounds segfault on the uint64 path. The enduring invariant left behind is the five-encoding round-trip self-check, which
depends only on GSL and requires no old engine.

## Status

Verified (`ctest`, 2/2 passing):

| Encoding | Key width | Round-trip | ns/byte |
|---|---|---|---|
| `ansi` | uint32 = 4×8 | 19,279/19,279 | 7.97 |
| `utf16le` | uint64 = 4×16 | 19,279/19,279 | 18.80 |
| `utf16be` | uint64 = 4×16 | 19,279/19,279 | 20.00 |
| `cn-utf16le` | uint32 = 2×16 | 57,881/57,881 | 6.29 |
| `gb2312` | uint32 = 2×16 | 57,876/57,876 | — |

UTF-16LE and UTF-16BE **share the same dictionary** (differing only in `ReadUnit` byte order), as asserted by tests. There is
also a one-time migration verification record: before `tests/equivalence.cpp` was deleted, the ANSI path was position-by-position
identical to `AcAutomaton32` across four workloads — random / alphabet / control bytes / prose; that assertion no longer runs in
this repository.

The uint64 path is noticeably slower than uint32 (18.8 vs 7.97 ns/byte, and it probes only once every two bytes). Two candidate
causes have not yet been separated: cache pressure from the 512KB key table, and `U16LeTraits::Fold` being a branched range
check whereas ANSI is a single branchless OR. Reaching a conclusion requires separate measurement.

**Corpus coverage gap:** 3 of the 4,913 code units are outside strict GB2312 — U+5570, U+69C3,
U+7117; these are GBK extension characters, involving 5 bigrams. `libqattern`'s existing `cn_gb2312.data` was generated with
CP936 (i.e., GBK), so the lead bytes of those 5 patterns fall outside the range accepted by `libabstr`'s own `IsValidStringChar`.
When the new library generates with strict GB2312, these 5 are excluded.

**The old file `qchars_u16be_hex.txt` cannot be reused directly**: its first entry is `0x2E0030002E003000`, i.e., the form
obtained by assembling UTF-16LE memory bytes in MSB-first order (ASCII lands in the high byte of each unit), not the code-point
keys of real UTF-16BE text. The new library uniformly uses "unit = decoded character value", so BE and LE share the same
dictionary. To continue using that dictionary, its original semantic intent must first be confirmed.

Not yet done: dictionary on-disk format, libabstr consumer-side replacement.


## Build

```
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH=C:/devdrv/usr
cmake --build build && ctest --test-dir build
build/index_compare data/qchars.txt data/prose.txt
```

The package name is `qattern`, the CMake target is `qattern::qattern`, and the public header is `qattern.hpp`; all three are
**identical in name and path** to the deprecated Aho-Corasick library, so the install layout coincides verbatim:

```
cmake --install build --prefix C:/devdrv/usr
# -> include/qattern.hpp and lib/cmake/qattern/qatternConfig.cmake, directly overwriting the old library
```

This is the expected cost of taking over the identity, but there are two consequences to watch out for. First, on
`CMAKE_PREFIX_PATH` **whoever comes first takes effect**; when two prefixes both contain old artifacts, no error is reported —
it silently picks up the other set. In practice I wrote the temporary prefix in MSYS style `/c/Users/...`, which CMake ignored
outright, so it resolved to the old package in `C:/devdrv/usr` and the build failed — paths must be written in Windows style.
Second, the old library installs `qattern.lib`/`qatternd.lib` to the same location, while this library is header-only
(`INTERFACE`), so `.lib` files left over after an overwrite install are not cleaned up; when switching, `lib/qattern*.lib` in the
old prefix must also be deleted.

## The 2,502 Dotted-Decimal Entries in the Dictionary Are Not Impurities

The 2,502 pure `[0-9.]` (no letters, containing both digits and dots) quadruples in `data/qchars.txt` are intentionally added to
anchor IP addresses and version numbers; removing them would eliminate the ability to extract dotted-decimal strings from memory.
The criterion is "strings commonly used in programs", not "natural-language words".
