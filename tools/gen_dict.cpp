// Turns a source corpus into a qattern dictionary artefact.
//
//   gen_dict <encoding> <corpus-file> <output-file> [--bin] [--name SYMBOL]
//
// encoding is one of: ansi, utf16le, utf16be, cn-utf16le, gb2312
// Default output is a C++ header holding the exact bytes a loader accepts, so a
// consumer can embed the dictionary and keep shipping no data files. --bin writes
// the same bytes as a plain file.
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <fstream>
#include <iomanip>
#include <stdexcept>
#include <string>
#include <vector>

#include "corpus.hpp"
#include <qattern_dict.hpp>

using namespace qattern;

namespace {

std::string upper(std::string s)
{
    for (auto& c : s) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    for (auto& c : s) {
        if (c == '-') {
            c = '_';
        }
    }
    return s;
}

void write_bytes(const std::string& path, const std::vector<uint8_t>& bytes)
{
    auto out = std::ofstream(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("cannot write " + path);
    }
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
}

void write_header_file(const std::string& path, const std::string& symbol, const size_t entry_count,
                       const std::vector<uint8_t>& bytes)
{
    auto out = std::ofstream(path, std::ios::trunc);
    if (!out) {
        throw std::runtime_error("cannot write " + path);
    }
    const auto guard = symbol + "_H";
    out << "/* auto-generated - do not edit by hand\n"
        << " * qattern dictionary: " << symbol << ", " << entry_count << " keys, " << bytes.size()
        << " bytes\n */\n\n"
        << "#ifndef " << guard << "\n"
        << "#define " << guard << "\n\n"
        << "#include <array>\n"
        << "#include <cstddef>\n\n"
        << "// The byte content is identical to the .bin artefact and is parsed and validated by "
           "read_dictionary,\n"
        << "// so embedding and the shipped file share the same read path.\n"
        << "alignas(64) inline constexpr std::array<uint8_t, " << bytes.size() << "> " << symbol
        << " = {\n    ";
    for (size_t i = 0; i < bytes.size(); ++i) {
        out << "0x" << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<unsigned>(bytes[i]) << std::dec << ",";
        if ((i + 1) % 16 == 0) {
            out << "\n    ";
        } else {
            out << ' ';
        }
    }
    out << "\n};\n\n#endif // " << guard << "\n";
}

template <typename Traits>
int generate(const std::string& corpus_path, const std::string& out_path, const bool as_binary,
             const std::string& symbol)
{
    const auto loaded = corpus::load<Traits>(corpus_path);
    if (loaded.entries.empty()) {
        std::fprintf(stderr, "no usable entries for %s\n", symbol.c_str());
        return 1;
    }
    const auto keys = build_dictionary<Traits>(loaded.entries);
    const auto bytes = write_dictionary<Traits>(keys);

    if (as_binary) {
        write_bytes(out_path, bytes);
    } else {
        write_header_file(out_path, symbol, keys.size(), bytes);
    }
    std::printf("%-12s entries=%-6zu skipped=%-4zu keys=%-6zu bytes=%zu -> %s\n", symbol.c_str(),
                loaded.entries.size(), loaded.skipped, keys.size(), bytes.size(), out_path.c_str());
    return 0;
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

static int run(const int argc, char** argv)
{
    const auto args = std::vector<std::string>(argv + 1, argv + argc);
    auto as_binary = false;
    auto symbol = std::string{};
    auto positional = std::vector<std::string>{};
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--bin") {
            as_binary = true;
        } else if (args[i] == "--name" && i + 1 < args.size()) {
            symbol = args[++i];
        } else {
            positional.push_back(args[i]);
        }
    }
    if (positional.size() != 3) {
        std::fprintf(stderr,
                     "usage: %s <encoding> <corpus-file> <output-file> [--bin] [--name SYMBOL]\n",
                     argv[0]);
        return 2;
    }
    const auto& encoding = positional[0];
    if (symbol.empty()) {
        symbol = "QATTERN_DICT_" + upper(encoding);
    }

    try {
        if (encoding == "ansi") {
            return generate<AnsiTraits>(positional[1], positional[2], as_binary, symbol);
        }
        if (encoding == "utf16le") {
            return generate<U16LeTraits>(positional[1], positional[2], as_binary, symbol);
        }
        if (encoding == "utf16be") {
            return generate<U16BeTraits>(positional[1], positional[2], as_binary, symbol);
        }
        if (encoding == "cn-utf16le") {
            return generate<CnU16LeTraits>(positional[1], positional[2], as_binary, symbol);
        }
        if (encoding == "gb2312") {
            return generate<Gb2312Traits>(positional[1], positional[2], as_binary, symbol);
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "gen_dict failed: %s\n", e.what());
        return 1;
    }

    std::fprintf(stderr, "unknown encoding '%s'\n", encoding.c_str());
    return 2;
}
