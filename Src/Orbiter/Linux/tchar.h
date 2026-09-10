// Linux <tchar.h> — MSVC's generic-text mappings.
//
// On Windows this switches the _t* names between the ANSI and wide CRT
// depending on _UNICODE. Orbiter is built without _UNICODE, so every mapping
// resolves to the plain char version, which is what these do.

#ifndef ORBITER_LINUX_TCHAR_H
#define ORBITER_LINUX_TCHAR_H

#ifdef _WIN32
#error "This header is for non-Windows builds only."
#endif

#include <windows.h>

typedef char _TCHAR;

#define _tcslen   strlen
#define _tcscpy   strcpy
#define _tcsncpy  strncpy
#define _tcscat   strcat
#define _tcscmp   strcmp
#define _tcsicmp  strcasecmp
#define _tcsnicmp strncasecmp
#define _tcsstr   strstr
#define _tcschr   strchr
#define _tcsrchr  strrchr
#define _tcstok   strtok
#define _tprintf  printf
#define _stprintf sprintf
#define _sntprintf snprintf
#define _tfopen   fopen
#define _ttoi     atoi
#define _ttof     atof

#endif // ORBITER_LINUX_TCHAR_H
