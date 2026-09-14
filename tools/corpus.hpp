#ifndef QATTERN_TOOLS_CORPUS_HPP
#define QATTERN_TOOLS_CORPUS_HPP

// Reading source corpora into per-encoding character units.
//
// This lives in tools/ because it is a build-time concern: the shipped artefact is
// a dictionary blob, and a scanner never parses corpora at runtime. The generator
// and the self tests share it so there is exactly one place that decides how a
// corpus line becomes units -- getting that wrong (byte order, in particular) is
// a silent mismatch between a dictionary and the scanner that reads it.

#include <cstdint>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#include <qattern_dict.hpp>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

using namespace qattern;
#endif

namespace corpus {

inline std::string read_text(const std::string& path)
{
    auto in = std::ifstream(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot open " + path);
    }
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// Splits on LF and drops a trailing CR, since the corpora are stored LF but a
// working tree checked out with core.autocrlf=true presents CRLF.
inline std::vector<std::string> lines(const std::string& text)
{
    auto out = std::vector<std::string>{};
    auto start = size_t{0};
    for (size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == '\n') {
            auto len = i - start;
            if (len > 0 && text[start + len - 1] == '\r') {
                --len;
            }
            if (len > 0) {
                out.emplace_back(text.substr(start, len));
            }
            start = i + 1;
        }
    }
    return out;
}

inline size_t utf8_len(unsigned char lead)
{
    if (lead < 0x80) {
        return 1;
    }
    if ((lead & 0xE0) == 0xC0) {
        return 2;
    }
    if ((lead & 0xF0) == 0xE0) {
        return 3;
    }
    return 4;
}

// Decodes BMP code points only; the corpora are ideographs and ASCII.
inline uint16_t utf8_decode(const std::string& s, size_t& i)
{
    const auto lead = static_cast<unsigned char>(s[i]);
    const auto len = utf8_len(lead);
    if (i + len > s.size()) {
        i = s.size();
        return 0xFFFD;
    }
    auto unit = static_cast<uint32_t>(lead & (0xFFu >> (len + 1)));
    for (size_t k = 1; k < len; ++k) {
        unit = (unit << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3Fu);
    }
    i += len;
    return static_cast<uint16_t>(unit);
}

inline std::vector<uint16_t> decode_units(const std::string& s)
{
    auto out = std::vector<uint16_t>{};
    for (size_t i = 0; i < s.size();) {
        out.push_back(utf8_decode(s, i));
    }
    return out;
}

// CP936 is GBK, a superset of GB2312: characters outside GB2312 encode to lead
// bytes below 0xA1, which the GB2312 gate rejects. Such entries are reported as
// unrepresentable rather than smuggled into the dictionary.
inline std::vector<uint16_t> to_gb2312(const std::vector<uint16_t>& codePoints)
{
#ifdef _WIN32
    const auto wide = std::wstring(codePoints.begin(), codePoints.end());
    auto bytes = std::vector<char>(codePoints.size() * 2, '\0');
    const auto written =
        WideCharToMultiByte(936, WC_NO_BEST_FIT_CHARS, wide.c_str(), static_cast<int>(wide.size()),
                            bytes.data(), static_cast<int>(bytes.size()), nullptr, nullptr);
    if (written != static_cast<int>(codePoints.size() * 2)) {
        return {};
    }
    auto units = std::vector<uint16_t>{};
    for (size_t i = 0; i + 1 < bytes.size(); i += 2) {
        const auto lead = static_cast<unsigned char>(bytes[i]);
        const auto trail = static_cast<unsigned char>(bytes[i + 1]);
        if (lead < 0xA1 || lead > 0xF7 || trail < 0xA1 || trail > 0xFE) {
            return {};
        }
        units.push_back(static_cast<uint16_t>((lead << 8) | trail));
    }
    return units;
#else
    return {};
#endif
}

template <typename Traits> struct Loaded {
    std::vector<std::vector<typename Traits::UnitType>> entries;
    size_t skipped{0};
};

template <typename Traits> Loaded<Traits> load(const std::string& path)
{
    auto result = Loaded<Traits>{};
    for (const auto& line : lines(read_text(path))) {
        if constexpr (std::is_same_v<typename Traits::UnitType, uint8_t>) {
            if (line.size() == Traits::kUnits) {
                result.entries.emplace_back(line.begin(), line.end());
            } else {
                ++result.skipped;
            }
            continue;
        }

        if constexpr (Traits::kEncoding == Encoding::kUtf16Le ||
                      Traits::kEncoding == Encoding::kUtf16Be) {
            // Same ASCII corpus as the ANSI dictionary, widened one char per unit.
            if (line.size() == Traits::kUnits) {
                auto units = std::vector<typename Traits::UnitType>{};
                for (const auto c : line) {
                    units.push_back(
                        static_cast<typename Traits::UnitType>(static_cast<unsigned char>(c)));
                }
                result.entries.push_back(std::move(units));
            } else {
                ++result.skipped;
            }
            continue;
        }

        auto codePoints = decode_units(line);
        if (codePoints.size() != Traits::kUnits) {
            ++result.skipped;
            continue;
        }

        auto source = std::vector<uint16_t>{};
        if constexpr (Traits::kEncoding == Encoding::kGb2312) {
            source = to_gb2312(codePoints);
            if (source.size() != Traits::kUnits) {
                ++result.skipped;
                continue;
            }
        } else {
            source = std::move(codePoints);
        }

        // Explicitly convert to this policy's unit type: non-dependent expressions in discarded
        // branches are still type-checked, so pushing a vector<uint16_t> directly would fail to
        // compile for a uint8_t-unit policy.
        auto units = std::vector<typename Traits::UnitType>{};
        for (const auto value : source) {
            units.push_back(static_cast<typename Traits::UnitType>(value));
        }
        result.entries.push_back(std::move(units));
    }
    return result;
}

} // namespace corpus

#endif // QATTERN_TOOLS_CORPUS_HPP
