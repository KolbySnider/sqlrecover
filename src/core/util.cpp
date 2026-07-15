#include "core/util.hpp"
#include <fstream>

namespace sqlrecover {

std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f)
        throw ParseError("cannot open file: " + path);
    std::streamsize n = f.tellg();
    if (n < 0)
        throw ParseError("cannot size file: " + path);
    f.seekg(0);
    std::vector<uint8_t> buf(static_cast<size_t>(n));
    if (n > 0 && !f.read(reinterpret_cast<char*>(buf.data()), n))
        throw ParseError("read error: " + path);
    return buf;
}

bool looks_like_text(const std::string& s) {
    if (s.empty()) return false;
    size_t printable = 0;
    for (unsigned char c : s)
        if (c == '\t' || c == '\n' || (c >= 0x20 && c < 0x7f) || c >= 0x80)
            ++printable;
    return printable * 100 >= s.size() * 90;
}

} // namespace sqlrecover
