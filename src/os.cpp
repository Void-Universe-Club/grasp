#include "os.h"

#include <chrono>
#include <cstdlib>
#include <stdexcept>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#else
#include <csignal>
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace os {

// ---------- time ----------

long now_ms() {
    using namespace std::chrono;
    return static_cast<long>(duration_cast<milliseconds>(
        system_clock::now().time_since_epoch()).count());
}

// ---------- shell ----------

#ifdef _WIN32

namespace {

  // drain whatever is currently available (PeekNamedPipe is non-blocking)
void win_drain(HANDLE rd, std::string& out) {
    char buf[4096];
    DWORD avail = 0;
    while (PeekNamedPipe(rd, NULL, 0, NULL, &avail, NULL) && avail > 0) {
        DWORD n = 0;
        if (!ReadFile(rd, buf, sizeof(buf), &n, NULL) || n == 0) break;
        out.append(buf, n);
    }
}

}  // namespace

std::string win_run_shell(const std::string& cmd, long timeout_secs) {
    if (timeout_secs <= 0) timeout_secs = 60;

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;
    HANDLE rd, wr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) {
        throw std::runtime_error("pipe failed: " + cmd);
    }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);  // read end stays in the parent

    STARTUPINFOA si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

  // cmd.exe /d /s /c: /s strips the outer quotes we add, inner quotes reach the child intact
    std::string cmdline = "cmd.exe /d /s /c \"" + cmd + "\"";
    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));
    BOOL ok = CreateProcessA(NULL, &cmdline[0], NULL, NULL, TRUE,
                             CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    CloseHandle(wr);  // parent: close the write end
    if (!ok) {
        CloseHandle(rd);
        throw std::runtime_error("CreateProcess failed: " + cmd);
    }

    long deadline = now_ms() + timeout_secs * 1000;
    std::string out;
    for (;;) {
  // 1. read currently available data
        win_drain(rd, out);
  // 2. check whether the child exited
        if (WaitForSingleObject(pi.hProcess, 20) == WAIT_OBJECT_0) {
            DWORD code = 0;
            GetExitCodeProcess(pi.hProcess, &code);
  // 3. drain the remaining pipe content
            for (;;) {
                size_t before = out.size();
                win_drain(rd, out);
                if (out.size() == before) break;
                Sleep(10);
            }
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            CloseHandle(rd);
            if (code != 0) {
                out += "\nERROR: command exit code " + std::to_string(code) +
                       " (cmd: " + cmd + ")";
            }
            return out;
        }
  // 3. timeout kill
        if (now_ms() > deadline) {
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, INFINITE);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            CloseHandle(rd);
            throw std::runtime_error("command timeout (" +
                                     std::to_string(timeout_secs) + "s): " + cmd);
        }
    }
}

#else  // !_WIN32

namespace {

  // ---------- RAII: pipe fd pair ----------
struct PipeGuard {
    int fds[2];
    explicit PipeGuard(int f[2]) {
        fds[0] = f[0];
        fds[1] = f[1];
    }
    ~PipeGuard() {
        if (fds[0] >= 0) ::close(fds[0]);
        if (fds[1] >= 0) ::close(fds[1]);
    }
    void close_one(int fd) {
  // parent closes the write end early; a second close in the destructor is a harmless EBADF
        if (fd >= 0) ::close(fd);
    }
};

  // ---------- drain all currently available pipe data (non-blocking) ----------
std::string drain_pipe(int fd) {
    std::string out;
    char buf[4096];
    for (;;) {
        ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n > 0) {
            out.append(buf, static_cast<size_t>(n));
        } else if (n == 0) {
            break;  // EOF
        } else {
            if (errno == EAGAIN || errno == EINTR) break;
            break;  // other errors treated as end of data
        }
    }
    return out;
}

}  // namespace

std::string posix_run_shell(const std::string& cmd, long timeout_secs) {
    if (timeout_secs <= 0) timeout_secs = 60;

    int fds[2];
    if (::pipe(fds) != 0) {
        throw std::runtime_error("pipe failed: " + cmd);
    }
    PipeGuard pipe_guard(fds);

  // read end non-blocking, for polling
    int flags = fcntl(fds[0], F_GETFL, 0);
    fcntl(fds[0], F_SETFL, flags | O_NONBLOCK);

    pid_t pid = ::fork();
    if (pid < 0) {
        throw std::runtime_error("fork failed: " + cmd);
    }
    if (pid == 0) {
  // child: route stdout/stderr to the pipe write end
        ::dup2(fds[1], STDOUT_FILENO);
        ::dup2(fds[1], STDERR_FILENO);
        pipe_guard.close_one(fds[0]);
        pipe_guard.close_one(fds[1]);
        execl("/bin/sh", "sh", "-c", cmd.c_str(), (char*)NULL);
        _exit(127);
    }

  // parent: close the write end
    pipe_guard.close_one(fds[1]);

    long deadline = now_ms() + timeout_secs * 1000;
    std::string out;
    int status = 0;

    for (;;) {
  // 1. read currently available data
        out += drain_pipe(fds[0]);
  // 2. check whether the child exited
        pid_t r = ::waitpid(pid, &status, WNOHANG);
        if (r == pid) {
  // 3. drain the remaining pipe content
            for (;;) {
                std::string tail = drain_pipe(fds[0]);
                if (tail.empty()) break;
                out += tail;
                usleep(10000);
            }
            break;
        }
  // 3. timeout kill
        if (now_ms() > deadline) {
            ::kill(pid, SIGKILL);
            ::waitpid(pid, &status, 0);
            throw std::runtime_error("command timeout (" +
                                     std::to_string(timeout_secs) + "s): " + cmd);
        }
        usleep(20000);
    }

    if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
        out += "\nERROR: command exit code " +
               std::to_string(WEXITSTATUS(status)) + " (cmd: " + cmd + ")";
    }
    return out;
}

#endif  // _WIN32

std::string run_shell(const std::string& cmd, long timeout_secs) {
#ifdef _WIN32
    std::string out = win_run_shell(cmd, timeout_secs);
#else
    std::string out = posix_run_shell(cmd, timeout_secs);
#endif
  // strip trailing whitespace from output
    while (!out.empty() && (out[out.size() - 1] == '\n' || out[out.size() - 1] == '\r' ||
                            out[out.size() - 1] == ' ' || out[out.size() - 1] == '\t')) {
        out.erase(out.size() - 1);
    }
    return out;
}

std::string shell_quote(const std::string& s) {
#ifdef _WIN32
  // cmd.exe: double quotes around the whole argument
    return "\"" + s + "\"";
#else
  // sh: single quotes; embedded ' becomes '\'' so quotes cannot break the command
    std::string out = "'";
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\'') {
            out += "'\\''";
        } else {
            out += s[i];
        }
    }
    out += "'";
    return out;
#endif
}

std::string env_var_ref(const std::string& name) {
#ifdef _WIN32
    return "%" + name + "%";   // expanded by cmd.exe
#else
    return "$" + name;         // expanded by sh
#endif
}

// ---------- file system ----------

bool file_exists(const std::string& path) {
#ifdef _WIN32
    struct _stat st;
    return _stat(path.c_str(), &st) == 0;
#else
    struct stat st;
    return stat(path.c_str(), &st) == 0;
#endif
}

void ensure_dir(const std::string& dir) {
#ifdef _WIN32
    if (_mkdir(dir.c_str()) != 0 && !file_exists(dir)) {
        throw std::runtime_error("cannot create directory '" + dir + "'");
    }
#else
    if (mkdir(dir.c_str(), 0755) != 0 && !file_exists(dir)) {
        throw std::runtime_error("cannot create directory '" + dir + "'");
    }
#endif
}

void commit_file(const std::string& tmp, const std::string& path) {
#ifdef _WIN32
  // MSVC rename() fails when the target exists; MoveFileEx with REPLACE keeps the atomic commit
    if (!MoveFileExA(tmp.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        throw std::runtime_error("cannot commit file '" + path + "'");
    }
#else
    if (rename(tmp.c_str(), path.c_str()) != 0) {
        throw std::runtime_error("cannot commit file '" + path + "'");
    }
#endif
}

std::vector<std::string> list_dir(const std::string& dir) {
    std::vector<std::string> out;
#ifdef _WIN32
    std::string pattern = dir + "\\*";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        std::string name = fd.cFileName;
        if (name == "." || name == "..") continue;
        out.push_back(name);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR* d = opendir(dir.c_str());
    if (d == NULL) return out;
    struct dirent* ent;
    while ((ent = readdir(d)) != NULL) {
        std::string name = ent->d_name;
        if (name == "." || name == "..") continue;
        out.push_back(name);
    }
    closedir(d);
#endif
    return out;
}

// ---------- temp file ----------

#ifdef _WIN32

TempFile::TempFile() : fp_(NULL) {
    char dir[MAX_PATH], fname[MAX_PATH];
    GetTempPathA(MAX_PATH, dir);
    GetTempFileNameA(dir, "grasp-llm-", 0, fname);
    fp_ = std::fopen(fname, "wb");
    if (fp_ == NULL) {
        throw std::runtime_error("cannot create temp file");
    }
    path_ = fname;
}

TempFile::~TempFile() {
    if (fp_ != NULL) std::fclose(fp_);
    if (!path_.empty()) std::remove(path_.c_str());
}

void TempFile::write_all(const std::string& data) {
    if (std::fwrite(data.data(), 1, data.size(), fp_) != data.size()) {
        throw std::runtime_error("write temp file failed");
    }
    std::fflush(fp_);
}

#else

TempFile::TempFile() : fd_(-1) {
    char tmpl[] = "/tmp/grasp-llm-XXXXXX";
    fd_ = ::mkstemp(tmpl);
    if (fd_ < 0) {
        throw std::runtime_error("mkstemp failed for llm payload");
    }
    path_ = tmpl;
}

TempFile::~TempFile() {
    if (fd_ >= 0) ::close(fd_);
    if (!path_.empty()) ::unlink(path_.c_str());
}

void TempFile::write_all(const std::string& data) {
    size_t off = 0;
    while (off < data.size()) {
        ssize_t n = ::write(fd_, data.data() + off, data.size() - off);
        if (n <= 0) {
            throw std::runtime_error("write llm payload failed");
        }
        off += static_cast<size_t>(n);
    }
    ::fsync(fd_);
}

#endif  // _WIN32

#ifdef _WIN32

std::vector<std::string> wargv_to_utf8(int argc, wchar_t** argv) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        int len = WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, NULL, 0, NULL, NULL);
        std::string s(len > 1 ? len - 1 : 0, '\0');
        if (len > 1) {
            WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, &s[0], len, NULL, NULL);
        }
        args.push_back(s);
    }
    return args;
}

#endif  // _WIN32

}  // namespace os
