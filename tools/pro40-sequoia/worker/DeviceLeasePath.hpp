// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>
#include <cerrno>
#include <cstdlib>
#include <optional>
#include <pwd.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
namespace Pro40 {
inline std::optional<std::string> deviceLeaseDirectory() {
#ifdef PRO40_WORKER_TEST_LOCKS
    // Compiled only into hardware-free fixtures; never overrides real workers.
    const char *path=std::getenv("PRO40_WORKER_TEST_LOCK_DIR");
    return path && *path?std::optional<std::string>(path):std::nullopt;
#else
    std::array<char,16384> storage{};
    passwd entry{},*found=nullptr;
    if(getpwuid_r(geteuid(),&entry,storage.data(),storage.size(),&found)!=0 ||
       !found || !entry.pw_dir || entry.pw_dir[0]!='/')return std::nullopt;
    const std::string path=std::string(entry.pw_dir)+"/Library/Application Support/ASFW-Pro40-Workers";
    if(mkdir(path.c_str(),0700)<0 && errno!=EEXIST)return std::nullopt;
    return path; // DeviceLease checks ownership/type/mode without following symlinks.
#endif
}
}
