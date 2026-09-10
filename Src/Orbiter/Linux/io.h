// Linux <io.h> — MSVC's low-level I/O header.
//
// Included by Launchpad.cpp, DlgHelp.cpp, TabAbout.cpp and two vessel
// configurators. Across the whole tree only _access is used, nine times, to
// test for the presence of scenario and documentation files.
//
// The mode constants differ between the two platforms: MSVC uses 00/02/04/06
// for existence/write/read/read-write, POSIX uses F_OK/W_OK/R_OK. The values
// are mapped here so the call sites keep passing their Windows constants.

#ifndef ORBITER_LINUX_IO_H
#define ORBITER_LINUX_IO_H

#ifdef _WIN32
#error "This header is for non-Windows builds only."
#endif

#include <windows.h>
#include <unistd.h>

// MSVC _access modes
#define ORB_ACCESS_EXIST  0x00
#define ORB_ACCESS_WRITE  0x02
#define ORB_ACCESS_READ   0x04
#define ORB_ACCESS_RW     0x06

inline int _access(const char *path, int mode) {
    int m = F_OK;
    if (mode & ORB_ACCESS_WRITE) m |= W_OK;
    if (mode & ORB_ACCESS_READ)  m |= R_OK;
    return access(path, m);
}

inline int _unlink(const char *path) { return unlink(path); }
inline int _isatty(int fd)           { return isatty(fd); }

#endif // ORBITER_LINUX_IO_H
