// Linux <direct.h> — MSVC's directory-manipulation header.
//
// Included by Src/Orbiter/Orbiter.cpp, Src/Orbiter/TabScenario.cpp and
// Src/Module/LuaScript/LuaInline/LuaInline.cpp. On Windows this declares the
// underscore-prefixed directory functions; on POSIX the equivalents live in
// <sys/stat.h> and <unistd.h>, so this maps between them.
//
// _mkdir takes no mode argument on Windows. 0755 is used here: the caller's
// umask still applies, and Orbiter creates ordinary user-owned directories.

#ifndef ORBITER_LINUX_DIRECT_H
#define ORBITER_LINUX_DIRECT_H

#ifdef _WIN32
#error "This header is for non-Windows builds only."
#endif

#include <windows.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

inline int _mkdir(const char *path) {
    return mkdir(path, 0755);
}
inline int _rmdir(const char *path) {
    return rmdir(path);
}
inline int _chdir(const char *path) {
    return chdir(path);
}
inline char *_getcwd(char *buf, int len) {
    return getcwd(buf, (size_t)len);
}

#endif // ORBITER_LINUX_DIRECT_H
