// Linux <Strsafe.h> — the bounded string helpers XRSound uses.
//
// Microsoft's "safe string" header. XRSoundImpl.cpp includes it for
// StringCchPrintf, which is snprintf with the buffer size expressed in
// CHARACTERS rather than bytes and an HRESULT return.
//
// The distinction matters for the return value, not the size: for a char
// buffer a character is a byte, so the count passes through unchanged, but
// StringCchPrintf reports STRSAFE_E_INSUFFICIENT_BUFFER on truncation where
// snprintf returns the length it would have written. Callers here only check
// for failure, and both spellings agree that a negative HRESULT is failure.
//
// Truncation is the documented behaviour of these functions -- they always
// null-terminate -- which snprintf also guarantees, so the buffer contents
// match.

#ifndef ORBITER_LINUX_STRSAFE_H
#define ORBITER_LINUX_STRSAFE_H

#ifdef _WIN32
#error "This header is for non-Windows builds only."
#endif

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#define STRSAFE_E_INSUFFICIENT_BUFFER  ((HRESULT)0x8007007AL)
#define STRSAFE_E_INVALID_PARAMETER    ((HRESULT)0x80070057L)
#define STRSAFE_MAX_CCH                2147483647

#ifdef __cplusplus
extern "C++" {
#endif

static inline HRESULT StringCchVPrintfA(char *dst, size_t cchDest,
                                        const char *fmt, va_list ap)
{
    if (!dst || cchDest == 0 || !fmt) return STRSAFE_E_INVALID_PARAMETER;
    const int n = vsnprintf(dst, cchDest, fmt, ap);
    if (n < 0) return STRSAFE_E_INVALID_PARAMETER;
    return ((size_t)n >= cchDest) ? STRSAFE_E_INSUFFICIENT_BUFFER : S_OK;
}

static inline HRESULT StringCchPrintfA(char *dst, size_t cchDest,
                                       const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    const HRESULT hr = StringCchVPrintfA(dst, cchDest, fmt, ap);
    va_end(ap);
    return hr;
}

static inline HRESULT StringCchCopyA(char *dst, size_t cchDest, const char *src)
{
    if (!dst || cchDest == 0 || !src) return STRSAFE_E_INVALID_PARAMETER;
    const size_t len = strlen(src);
    const size_t n = (len < cchDest - 1) ? len : cchDest - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
    return (len >= cchDest) ? STRSAFE_E_INSUFFICIENT_BUFFER : S_OK;
}

static inline HRESULT StringCchCatA(char *dst, size_t cchDest, const char *src)
{
    if (!dst || cchDest == 0 || !src) return STRSAFE_E_INVALID_PARAMETER;
    const size_t have = strnlen(dst, cchDest);
    if (have >= cchDest) return STRSAFE_E_INSUFFICIENT_BUFFER;
    return StringCchCopyA(dst + have, cchDest - have, src);
}

static inline HRESULT StringCchLengthA(const char *src, size_t cchMax,
                                       size_t *pcch)
{
    if (!src) return STRSAFE_E_INVALID_PARAMETER;
    const size_t n = strnlen(src, cchMax);
    if (pcch) *pcch = n;
    return (n >= cchMax) ? STRSAFE_E_INVALID_PARAMETER : S_OK;
}

#ifdef __cplusplus
}
#endif

// Orbiter is built without UNICODE, so the unsuffixed names are the ANSI ones.
#define StringCchPrintf   StringCchPrintfA
#define StringCchVPrintf  StringCchVPrintfA
#define StringCchCopy     StringCchCopyA
#define StringCchCat      StringCchCatA
#define StringCchLength   StringCchLengthA

#endif // ORBITER_LINUX_STRSAFE_H
