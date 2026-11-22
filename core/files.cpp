// iris::files (core/files.h says the law).
#include "core/files.h"

#include <atomic>
#include <exception>
#include <fstream>
#include <system_error>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <unistd.h>
#define IRIS_FILES_POSIX 1
#elif defined(_WIN32)
#include <process.h>
#endif

namespace iris {
namespace files {

std::filesystem::path path(std::string_view utf8)
{
    return std::filesystem::u8path(utf8.begin(), utf8.end());
}

std::string utf8(const std::filesystem::path &p)
{
    const auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

std::optional<std::string> readAll(const std::filesystem::path &p)
{
    // Only a regular file has a length: a directory's seek-to-end answers 2^63-1 on ext4.
    std::error_code ec;
    if (!std::filesystem::is_regular_file(p, ec)) return std::nullopt;
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f) return std::nullopt;
    const std::streamoff n = f.tellg();
    if (n < 0) return std::nullopt;
    std::string out;
    try {
        out.resize(static_cast<std::size_t>(n));
    } catch (const std::exception &) {   // longer than the memory this process may take (bad_alloc, length_error)
        return std::nullopt;
    }
    f.seekg(0);
    if (n && !f.read(out.data(), n)) return std::nullopt;
    return out;
}

bool makeDirs(const std::filesystem::path &dir)
{
    if (dir.empty()) return false;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return std::filesystem::is_directory(dir, ec);
}

static long processId()
{
#if defined(IRIS_FILES_POSIX)
    return static_cast<long>(::getpid());
#elif defined(_WIN32)
    return static_cast<long>(::_getpid());
#else
    return 0;
#endif
}

std::filesystem::path stagingTempPath(const std::filesystem::path &target)
{
    static std::atomic<unsigned> sSerial{ 0 };
    std::filesystem::path tmp = target;
    tmp += ".tmp-" + std::to_string(processId()) + "-"
           + std::to_string(sSerial.fetch_add(1, std::memory_order_relaxed));
    return tmp;
}

void fsyncPath(const std::filesystem::path &p)
{
#if defined(IRIS_FILES_POSIX)
    const int fd = ::open(p.c_str(), O_RDONLY);
    if (fd >= 0) {
        ::fsync(fd);
        ::close(fd);
    }
#else
    (void)p;
#endif
}

bool atomicRename(const std::filesystem::path &from, const std::filesystem::path &to, std::string *error)
{
    std::error_code ec;
    std::filesystem::rename(from, to, ec);
    if (ec) {
        std::error_code ignored;
        std::filesystem::remove(from, ignored);
        if (error) *error = "could not place " + utf8(to) + ": " + ec.message();
        return false;
    }
    return true;
}

bool writeAtomic(const std::filesystem::path &target, std::string_view bytes, Durability durability, std::string *error)
{
    const std::filesystem::path dir = target.parent_path();
    if (!dir.empty() && !makeDirs(dir)) {
        if (error) *error = "cannot create " + utf8(dir);
        return false;
    }
    const std::filesystem::path tmp = stagingTempPath(target);
    std::error_code ignored;
    std::filesystem::remove(tmp, ignored);   // a leftover of a dead run that had our pid
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (out) out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (out) out.flush();
        if (!out) {
            out.close();
            std::filesystem::remove(tmp, ignored);
            if (error) *error = "cannot write " + utf8(target);
            return false;
        }
    }
    if (durability == Durability::Flush) fsyncPath(tmp);
    return atomicRename(tmp, target, error);
}

}   // namespace files
}   // namespace iris
