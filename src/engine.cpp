#include "engine.h"

#include <sys/wait.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <csignal>
#include <cstring>
#include <stdexcept>
#include <chrono>

long now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(
               system_clock::now().time_since_epoch()).count();
}

  // ---------- RAII: pipe fd pair ----------
namespace {

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

std::string run_shell(const std::string& cmd, long timeout_secs) {
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
  // match grasp: strip trailing whitespace from output
    while (!out.empty() && (out[out.size() - 1] == '\n' ||
                            out[out.size() - 1] == '\r' ||
                            out[out.size() - 1] == ' ' ||
                            out[out.size() - 1] == '\t')) {
        out.erase(out.size() - 1);
    }
    return out;
}
