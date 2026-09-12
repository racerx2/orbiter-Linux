// The shim's HANDLE object: its lifetime, the last-error store, and the Win32
// file and directory-enumeration API that hands handles out.
//
// Nothing here depends on anything outside libc and pthreads, so a standalone
// utility can link this translation unit on its own. See Handles.h.

#include <windows.h>
#include "Handles.h"

#include <dirent.h>
#include <fnmatch.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

// ===========================================================================
// Last error
// ===========================================================================

namespace {
// Thread-local, mirroring Win32's per-thread GetLastError.
thread_local DWORD g_lastError = 0;
} // namespace

extern "C" {

void orb_SetLastError(DWORD e) { g_lastError = e; }

DWORD orb_Win32ErrorFromErrno(int e)
{
    switch (e) {
    case 0:       return 0;
    case ENOENT:  return 2;   // ERROR_FILE_NOT_FOUND
    case ENOTDIR: return 3;   // ERROR_PATH_NOT_FOUND
    case EACCES:
    case EPERM:   return 5;   // ERROR_ACCESS_DENIED
    case EBADF:   return 6;   // ERROR_INVALID_HANDLE
    case ENOMEM:  return 8;   // ERROR_NOT_ENOUGH_MEMORY
    case EINVAL:  return 87;  // ERROR_INVALID_PARAMETER
    case EEXIST:  return 183; // ERROR_ALREADY_EXISTS
    default:      return (DWORD)e;
    }
}

DWORD GetLastError(void) { return g_lastError; }

} // extern "C"

// ===========================================================================
// Handle lifetime
// ===========================================================================

namespace {

// The teardown shared by CloseHandle and FindClose.
void destroyHandle(OrbHandle *h)
{
    switch (h->kind) {
    case OrbHandle::Thread:
        if (!h->joined) pthread_detach(h->thread);
        break;
    case OrbHandle::Mutex:
        pthread_mutex_destroy(&h->mutex);
        break;
    case OrbHandle::Event:
        pthread_cond_destroy(&h->cond);
        pthread_mutex_destroy(&h->evLock);
        break;
    case OrbHandle::Watch:
        if (h->fd >= 0) close(h->fd);
        break;
    case OrbHandle::File:
        if (h->fd >= 0) close(h->fd);
        break;
    case OrbHandle::Find:
        if (h->dirp) closedir((DIR *)h->dirp);
        break;
    default:
        break;
    }
    delete h;
}

} // namespace

extern "C" BOOL CloseHandle(HANDLE obj)
{
    OrbHandle *h = (OrbHandle *)obj;
    // INVALID_HANDLE_VALUE is (HANDLE)-1, not null, and reaches here from any
    // caller that closes the result of a failed CreateFile without testing it.
    // Windows rejects it; dereferencing it would fault.
    if (!h || obj == INVALID_HANDLE_VALUE) { orb_SetLastError(6); return FALSE; }

    // The fixed pseudo-handles are not owned by the caller and must survive.
    if (h->pinned) return TRUE;

    destroyHandle(h);
    return TRUE;
}

// ===========================================================================
// Files
//
// The Win32 file API in the shape texpack uses it: open for reading, ask the
// size, read the whole thing, close. The share mode, security attributes and
// template handle are ignored -- they have no counterpart here, and amount to
// nothing for a single-process tool reading its own files.
// ===========================================================================

extern "C" {

HANDLE CreateFileA(LPCSTR name, DWORD access, DWORD, void *,
                   DWORD disposition, DWORD, HANDLE)
{
    if (!name) { orb_SetLastError(87); return INVALID_HANDLE_VALUE; }

    // Win32 allows neither access bit set, meaning "query attributes only";
    // O_RDONLY is the closest equivalent and is harmless.
    int flags;
    const bool wantRead  = (access & GENERIC_READ)  != 0;
    const bool wantWrite = (access & GENERIC_WRITE) != 0;
    if      (wantRead && wantWrite) flags = O_RDWR;
    else if (wantWrite)             flags = O_WRONLY;
    else                            flags = O_RDONLY;

    switch (disposition) {
    case CREATE_NEW:        flags |= O_CREAT | O_EXCL;  break;
    case CREATE_ALWAYS:     flags |= O_CREAT | O_TRUNC; break;
    case OPEN_EXISTING:                                 break;
    case OPEN_ALWAYS:       flags |= O_CREAT;           break;
    case TRUNCATE_EXISTING: flags |= O_TRUNC;           break;
    default: orb_SetLastError(87); return INVALID_HANDLE_VALUE;
    }

    const int fd = ::open(name, flags | O_CLOEXEC, 0644);
    if (fd < 0) {
        orb_SetLastError(orb_Win32ErrorFromErrno(errno));
        return INVALID_HANDLE_VALUE;
    }

    OrbHandle *h = new OrbHandle{ OrbHandle::File };
    h->fd   = fd;
    h->path = name;
    return (HANDLE)h;
}

BOOL ReadFile(HANDLE obj, LPVOID buf, DWORD toRead, LPDWORD read, void *)
{
    OrbHandle *h = (OrbHandle *)obj;
    if (!h || obj == INVALID_HANDLE_VALUE || h->kind != OrbHandle::File || !buf) {
        orb_SetLastError(6);
        return FALSE;
    }

    // Loop until it is all read. A single ::read may return fewer bytes than
    // asked for and still be a success; ReadFile on a file does not, and its
    // callers do not loop -- texpack asks for the whole file in one call and
    // treats a short count as truncation. Only end of file stops the loop short,
    // which is the one case Windows reports the same way.
    char *p = (char *)buf;
    DWORD done = 0;
    while (done < toRead) {
        const ssize_t n = ::read(h->fd, p + done, toRead - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            orb_SetLastError(orb_Win32ErrorFromErrno(errno));
            if (read) *read = done;
            return FALSE;
        }
        if (n == 0) break;          // end of file
        done += (DWORD)n;
    }
    if (read) *read = done;
    return TRUE;
}

BOOL GetFileSizeEx(HANDLE obj, PLARGE_INTEGER size)
{
    OrbHandle *h = (OrbHandle *)obj;
    if (!h || obj == INVALID_HANDLE_VALUE || h->kind != OrbHandle::File || !size) {
        orb_SetLastError(6);
        return FALSE;
    }

    struct stat st;
    if (fstat(h->fd, &st) != 0) {
        orb_SetLastError(orb_Win32ErrorFromErrno(errno));
        return FALSE;
    }
    size->QuadPart = (LONGLONG)st.st_size;
    return TRUE;
}

} // extern "C"

// ===========================================================================
// Directory enumeration
//
// FindFirstFile takes a path with a wildcard in it -- "Surf/04/000001/*.dds" --
// not a directory, so the argument is split at its last separator and the tail
// is matched against each entry with fnmatch. Two Win32 behaviours are
// reproduced rather than tidied away:
//
//   * Matching is case-insensitive. NTFS is, ext4 is not, and callers write the
//     pattern in whatever case the format uses -- texpack asks for "*.dds"
//     against tiles that may be named .DDS -- so FNM_CASEFOLD is what keeps a
//     tree packed on Windows and unpacked here finding its own files.
//
//   * "." and ".." are returned, as Windows returns them for a "*" pattern, and
//     callers are written knowing it: texpack's level scan accepts a name only
//     if it is six digits long, which rejects both. Skipping them would be a
//     silent behaviour change for any caller that counts entries. FNM_PERIOD is
//     deliberately not set, so "*" matches a leading dot.
// ===========================================================================

namespace {

void fillFindData(int dfd, const char *name, LPWIN32_FIND_DATAA data)
{
    memset(data, 0, sizeof(*data));
    snprintf(data->cFileName, sizeof(data->cFileName), "%s", name);

    struct stat st;
    if (fstatat(dfd, name, &st, 0) == 0) {
        data->dwFileAttributes = S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY
                                                     : FILE_ATTRIBUTE_NORMAL;
        data->nFileSizeLow  = (DWORD)((uint64_t)st.st_size & 0xFFFFFFFFu);
        data->nFileSizeHigh = (DWORD)((uint64_t)st.st_size >> 32);
    } else {
        data->dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
    }
}

// Advance to the next entry matching the handle's pattern. FALSE at the end
// of the directory.
BOOL advanceFind(OrbHandle *h, LPWIN32_FIND_DATAA data)
{
    DIR *d = (DIR *)h->dirp;
    const int dfd = dirfd(d);
    for (;;) {
        errno = 0;
        const struct dirent *e = readdir(d);
        if (!e) {
            // 18 is ERROR_NO_MORE_FILES: what a caller distinguishing "done"
            // from "failed" at the end of a search looks for.
            orb_SetLastError(errno ? orb_Win32ErrorFromErrno(errno) : 18);
            return FALSE;
        }
        if (fnmatch(h->pattern.c_str(), e->d_name, FNM_CASEFOLD) != 0) continue;
        fillFindData(dfd, e->d_name, data);
        return TRUE;
    }
}

} // namespace

extern "C" {

HANDLE FindFirstFileA(LPCSTR spec, LPWIN32_FIND_DATAA data)
{
    if (!spec || !data) { orb_SetLastError(87); return INVALID_HANDLE_VALUE; }

    std::string s(spec);
    std::string dir, pat;
    const size_t slash = s.find_last_of('/');
    if (slash == std::string::npos) { dir = "."; pat = s; }
    else { dir = s.substr(0, slash); pat = s.substr(slash + 1); if (dir.empty()) dir = "/"; }
    if (pat.empty()) pat = "*";

    DIR *d = opendir(dir.c_str());
    if (!d) { orb_SetLastError(orb_Win32ErrorFromErrno(errno)); return INVALID_HANDLE_VALUE; }

    OrbHandle *h = new OrbHandle{ OrbHandle::Find };
    h->dirp    = d;
    h->path    = dir;
    h->pattern = pat;

    if (!advanceFind(h, data)) { destroyHandle(h); return INVALID_HANDLE_VALUE; }
    return (HANDLE)h;
}

BOOL FindNextFileA(HANDLE obj, LPWIN32_FIND_DATAA data)
{
    OrbHandle *h = (OrbHandle *)obj;
    if (!h || obj == INVALID_HANDLE_VALUE || h->kind != OrbHandle::Find || !data) {
        orb_SetLastError(6);
        return FALSE;
    }
    return advanceFind(h, data);
}

BOOL FindClose(HANDLE obj)
{
    OrbHandle *h = (OrbHandle *)obj;
    if (!h || obj == INVALID_HANDLE_VALUE || h->kind != OrbHandle::Find) {
        orb_SetLastError(6);
        return FALSE;
    }
    destroyHandle(h);
    return TRUE;
}

} // extern "C"
