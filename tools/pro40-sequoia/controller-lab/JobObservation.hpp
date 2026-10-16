// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <chrono>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <string>
#include <vector>

namespace Pro40Lab {
struct JobProcessResult {
    bool reaped = false, timedOut = false, overflow = false, ioError = false;
    int spawnError = 0, exit = -1;
    std::string out, err;
    [[nodiscard]] bool usable() const {
        return !spawnError && reaped && !timedOut && !overflow && !ioError && exit >= 0;
    }
};
// Owned direct child only, no shell/launch constraints, no inferred cleanup from
// PID disappearance. Deadlines prevent use of late data, not OS latency guarantees.
inline JobProcessResult observeProcess(const std::vector<std::string>& arguments,
                                      std::chrono::milliseconds budget = std::chrono::seconds(2),
                                      size_t outputLimit = 16384) {
    JobProcessResult r;
    if (arguments.empty() || !outputLimit || outputLimit>1048576) { r.spawnError = EINVAL; return r; }
    int out[2]{-1,-1}, err[2]{-1,-1};
    auto closeFD = [](int& fd) { if (fd >= 0) { close(fd); fd = -1; } };
    if (pipe(out) || pipe(err)) {
        r.spawnError = errno;
        for (int *p : {out, err}) { closeFD(p[0]); closeFD(p[1]); }
        return r;
    }
    for (int *p : {out, err}) {
        if (fcntl(p[0], F_SETFD, FD_CLOEXEC) || fcntl(p[1], F_SETFD, FD_CLOEXEC) ||
            fcntl(p[0], F_SETFL, O_NONBLOCK)) r.ioError = true;
    }
    posix_spawn_file_actions_t actions;
    int setup = posix_spawn_file_actions_init(&actions);
    if (setup) {
        r.spawnError = setup;
        for (int *p : {out, err}) { closeFD(p[0]); closeFD(p[1]); }
        return r;
    }
    if (posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0) ||
        posix_spawn_file_actions_adddup2(&actions, out[1], STDOUT_FILENO) ||
        posix_spawn_file_actions_adddup2(&actions, err[1], STDERR_FILENO)) r.ioError = true;
    for (int *p : {out, err}) for (int fd : {p[0],p[1]})
        if (posix_spawn_file_actions_addclose(&actions, fd)) r.ioError = true;
    std::vector<char *> argv;
    for (const auto& arg : arguments) argv.push_back(const_cast<char *>(arg.c_str()));
    argv.push_back(nullptr);
    // Fixed observation environment, without inherited user tool overrides.
    char locale[] = "LC_ALL=C", path[] = "PATH=/usr/bin:/bin";
    char *env[] = {locale, path, nullptr};
    pid_t pid = -1;
    posix_spawnattr_t attributes;
    int attributeError = posix_spawnattr_init(&attributes);
    if (!attributeError && posix_spawnattr_setflags(&attributes, POSIX_SPAWN_CLOEXEC_DEFAULT))
        r.ioError = true;
    const auto start = std::chrono::steady_clock::now();
    r.spawnError = attributeError ? attributeError : r.ioError ? EIO :
        posix_spawn(&pid, argv[0], &actions, &attributes, argv.data(), env);
    if (!attributeError) posix_spawnattr_destroy(&attributes);
    posix_spawn_file_actions_destroy(&actions);
    closeFD(out[1]); closeFD(err[1]);
    auto drain = [&](int& fd, std::string& target) {
        // Bound work per turn even if the child writes continuously.
        for (unsigned i=0; i<16 && fd>=0; ++i) {
            char bytes[1024];
            ssize_t n = read(fd, bytes, sizeof(bytes));
            if (n == 0) { closeFD(fd); break; }
            if (n < 0) {
                if (errno == EINTR) continue;
                if (errno != EAGAIN && errno != EWOULDBLOCK) r.ioError = true;
                break;
            }
            if (target.size()+static_cast<size_t>(n)>outputLimit) { r.overflow=true; break; }
            target.append(bytes, static_cast<size_t>(n));
        }
    };
    bool killed = false;
    auto deadline = start + budget;
    while (!r.spawnError) {
        drain(out[0],r.out); drain(err[0],r.err);
        if (!r.reaped) {
            int status = 0;
            pid_t found = waitpid(pid,&status,WNOHANG);
            if (found == pid) {
                r.reaped = true;
                if (WIFEXITED(status)) r.exit = WEXITSTATUS(status);
            } else if (found < 0 && errno != EINTR) { r.ioError=true; break; }
        }
        auto now = std::chrono::steady_clock::now();
        if (!killed && now >= deadline) r.timedOut = true;
        if (r.reaped && out[0]<0 && err[0]<0) break;
        if (!killed && (r.timedOut || r.overflow || r.ioError)) {
            // waitpid has not reaped this retained child: no PID reuse signaling.
            if (!r.reaped && kill(pid,SIGKILL)!=0 && errno!=ESRCH) r.ioError=true;
            killed=true; deadline=now+std::chrono::seconds(1);
        }
        if (killed && now>=deadline) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    closeFD(out[0]); closeFD(err[0]);
    return r;
}
}
