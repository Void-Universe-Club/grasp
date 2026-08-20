#ifndef GRASP_OS_H
#define GRASP_OS_H

// Cross-platform OS abstraction: every platform-specific call (Windows API /
// POSIX) lives behind this header, so the rest of grasp stays portable.

#include <cstdio>
#include <string>
#include <vector>

namespace os {

// current timestamp (milliseconds since epoch)
long now_ms();

// run a shell command and return the merged stdout+stderr; kill and throw
// std::runtime_error after timeout_secs (60s default when <= 0)
std::string run_shell(const std::string& cmd, long timeout_secs);

// quote a string for the platform shell (spaces/quotes cannot break the command)
std::string shell_quote(const std::string& s);

// shell expression that expands to the env var at runtime: %NAME% on Windows,
// $NAME on POSIX (the raw value never lands in argv)
std::string env_var_ref(const std::string& name);

// ---------- file system ----------

bool file_exists(const std::string& path);
void ensure_dir(const std::string& dir);           // throw std::runtime_error on failure
void commit_file(const std::string& tmp, const std::string& path);  // atomic replace
std::vector<std::string> list_dir(const std::string& dir);          // entry names, no prefix

// RAII temp file: write_all() then hand path() to a child process; auto-deleted
class TempFile {
public:
    TempFile();
    ~TempFile();
    void write_all(const std::string& data);
    const std::string& path() const { return path_; }

private:
    TempFile(const TempFile&);             // non-copyable
    TempFile& operator=(const TempFile&);
#ifdef _WIN32
    std::FILE* fp_;
#else
    int fd_;
#endif
    std::string path_;
};

#ifdef _WIN32
// wmain entry: UTF-16 command line -> UTF-8 strings (MSVC main() would decode
// argv with the system ACP, breaking Chinese args on zh-CN)
std::vector<std::string> wargv_to_utf8(int argc, wchar_t** argv);
#endif

}  // namespace os

#endif  // GRASP_OS_H
