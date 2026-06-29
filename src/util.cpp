#include "util.hpp"
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

} // namespace sqlrecover
