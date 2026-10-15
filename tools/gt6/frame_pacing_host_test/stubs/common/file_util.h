// Host-test stand-in for common/file_util.h: only the IOFile surface perf_log.cpp uses.
#pragma once
#include <cstdio>
#include <string>
#include <string_view>

namespace FileUtil {
class IOFile {
public:
    IOFile(const std::string& filename, const char openmode[], int = 0) {
        f = std::fopen(filename.c_str(), openmode);
    }
    ~IOFile() {
        Close();
    }
    bool IsGood() const {
        return f != nullptr && !std::ferror(f);
    }
    std::size_t WriteString(std::string_view s) {
        return f ? std::fwrite(s.data(), 1, s.size(), f) : 0;
    }
    bool Flush() {
        return f && std::fflush(f) == 0;
    }
    bool Close() {
        if (f) {
            std::fclose(f);
            f = nullptr;
        }
        return true;
    }

private:
    std::FILE* f = nullptr;
};
} // namespace FileUtil
