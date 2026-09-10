// =================================================================================================================================
// The MIT Lisence:
//
// Copyright (C) 2012-2026 Jarmo Nikkanen
//
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation 
// files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, 
// modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software 
// is furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
// OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
// LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR
// IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
// =================================================================================================================================
//
// CONVERTED FROM OVP/D3D9Client/Log.h, read end to end (73 lines), against
// Log.cpp read end to end (490 lines).
//
// THIS FILE CONTAINS NO DIRECT3D AT ALL, and neither does its .cpp. The only
// changes here are the D3D9 -> Vulkan renames the tree-wide rule requires:
// D3D9Time, D3D9DebugLog, D3D9DebugLogVec, D3D9InitLog, D3D9CloseLog,
// D3D9GetTime, D3D9SetTime and D3D9DebugQueue.
//
// Everything the implementation touches that looks like Win32 is already in
// Src/Orbiter/Linux/windows.h -- read there rather than assumed:
// CRITICAL_SECTION with all four of its functions (:2424-2456),
// QueryPerformanceFrequency/Counter over LARGE_INTEGER (:2465-2493),
// GetCurrentThreadId (:1167), DebugBreak as raise(SIGTRAP) (:2508),
// _vsnprintf_s (:2519), fprintf_s (:2527), fopen_s (:1255), sprintf_s,
// strcpy_s and strcat_s in both the explicit-size and array-deducing forms,
// ARRAYSIZE (:1565), lstrlen (:1213), MessageBoxA (:671) and __int64 (:160).
// So the logger crosses as written.
// =================================================================================================================================
#ifndef __LOGGING_H
#define __LOGGING_H

#include <stdio.h>
#include <queue>
#include <string>
#include "DrawAPI.h"
#define WIN32_LEAN_AND_MEAN
#include <Windows.h> // DWORD, LPCSTR
#undef WIN32_LEAN_AND_MEAN

typedef struct {
	double time;
	double count;
	double peak;
} VulkanTime;

extern int uEnableLog;  // This value is controlling log operation ( Config->DebugLvl )
extern int iEnableLog;
extern int EnableLogStack[16];
extern std::queue<std::string> VulkanDebugQueue;

#define _PUSHLOG EnableLogStack[iEnableLog++] = uEnableLog;
#define _SETLOG(x) { EnableLogStack[iEnableLog++] = uEnableLog; if (uEnableLog>0) uEnableLog=x; } 
#define _POPLOG  uEnableLog = EnableLogStack[--iEnableLog];

#define _UNDEBUGED LogWrn("[Undebuged/Unfinished code section reached in %s (File %s, Line %d)]",__FUNCTION__,__FILE__,__LINE__);
//#define _UNDEBUGED

void   RuntimeError(const char* File, const char* Fnc, UINT Line);
void   VulkanDebugLog(const char *format, ...);
void   VulkanDebugLogVec(const char* lbl, oapi::FVECTOR4 &v);
void   VulkanInitLog(const char *file);
void   VulkanCloseLog();
void   LogTrace(const char *format, ...);
void   LogErr(const char *format, ...);
void   LogWrn(const char *format, ...);
void   LogOk (const char *format, ...);
void   LogBreak(const char* format, ...);
void   LogBlu(const char *format, ...);
void   LogOapi(const char *format, ...);
void   LogAlw(const char *format, ...);
void   LogDbg(const char *color, const char *format, ...);
void   LogClr(const char *color, const char *format, ...);

double VulkanGetTime();
void   VulkanSetTime(VulkanTime &inout, double ref);

void   MissingRuntimeError();
void   FailedDeviceError();
void   LogAttribs(DWORD attrib, DWORD w, DWORD h, LPCSTR origin);

#define HALT() { RuntimeError(__FILE__,__FUNCTION__,__LINE__); }

#endif
