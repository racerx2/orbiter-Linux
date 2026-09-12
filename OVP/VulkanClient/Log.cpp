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
// max() is a macro from MSVC's <windows.h> and the Windows file relies on it.
// The Linux shim deliberately does not define it -- that would break
// <algorithm> for everything including the shim -- so the one call site names
// std::max.
//
// DWORD is 'unsigned long' on MSVC but uint32_t (unsigned int) on Linux, so
// the thread id is cast at every "%lX". Both are 32 bits, so the number
// printed is right either way, but the varargs mismatch is undefined
// behaviour that -Wformat reports.
// =================================================================================================================================

#include <Windows.h>
#include <algorithm>
#include "Log.h"
#include "VulkanUtil.h"
#include "VulkanConfig.h"
#include "VulkanClient.h"

FILE *vulkanclient_log = NULL;

#define LOG_MAX_LINES 100000
#define ERRBUF 8000
#define OPRBUF 512
#define TIMEBUF 63

extern class VulkanClient* g_client;

char ErrBuf[ERRBUF+1];
char OprBuf[OPRBUF+1];
char TimeBuf[TIMEBUF+1];

time_t ltime;
int uEnableLog = 1;     // This value is controlling log opeation ( Config->DebugLvl )
int iEnableLog = 0;     // Index into EnableLogStack
int EnableLogStack[16];
int iLine = 0;          // Line number counter (iLine <= LOG_MAX_LINES)

__int64 qpcFrq = 0;     // Performance counter frequency
__int64 qpcRef = 0;     // Performance counter reference value (for "delta t")
__int64 qpcStart = 0;   // Performance counter start value ("zero")

std::queue<std::string> VulkanDebugQueue;

CRITICAL_SECTION LogCrit;


//-------------------------------------------------------------------------------------------
//
void MissingRuntimeError()
{
	MessageBoxA(NULL,
		"A Vulkan driver may be missing or too old. Vulkan 1.2 or newer is required.",
		"VulkanClient Initialization Failed", MB_OK);
}

//-------------------------------------------------------------------------------------------
//
void FailedDeviceError()
{
	MessageBoxA(NULL,
		"The Vulkan device could not be created. See Orbiter.log for details.",
		"VulkanClient Initialization Failed", MB_OK);
}

//-------------------------------------------------------------------------------------------
//
void RuntimeError(const char* File, const char* Fnc, UINT Line)
{
	if (Config->DebugLvl == 0) return;
	char buf[256];
	sprintf_s(buf, 256, "[%s] [%s] Line: %u See Orbiter.log for details.", File, Fnc, Line);
	MessageBoxA(g_client->GetRenderWindow(), buf, "Critical Error:", MB_OK);
	DebugBreak();
}

//-------------------------------------------------------------------------------------------
// Log OAPISURFACE_xxx attributes
void LogAttribs(DWORD attrib, DWORD w, DWORD h, LPCSTR origin)
{
	char buf[512];
	sprintf_s(buf, 512, "%s (%d,%d)[0x%X]: ", origin, w, h, attrib);
	if (attrib&OAPISURFACE_TEXTURE)		 strcat_s(buf, 512, "OAPISURFACE_TEXTURE ");
	if (attrib&OAPISURFACE_RENDERTARGET) strcat_s(buf, 512, "OAPISURFACE_RENDERTARGET ");
	if (attrib&OAPISURFACE_GDI)			 strcat_s(buf, 512, "OAPISURFACE_GDI ");
	if (attrib&OAPISURFACE_SKETCHPAD)	 strcat_s(buf, 512, "OAPISURFACE_SKETCHPAD ");
	if (attrib&OAPISURFACE_MIPMAPS)		 strcat_s(buf, 512, "OAPISURFACE_MIPMAPS ");
	if (attrib&OAPISURFACE_NOMIPMAPS)	 strcat_s(buf, 512, "OAPISURFACE_NOMIPMAPS ");
	if (attrib&OAPISURFACE_ALPHA)		 strcat_s(buf, 512, "OAPISURFACE_ALPHA ");
	if (attrib&OAPISURFACE_NOALPHA)		 strcat_s(buf, 512, "OAPISURFACE_NOALPHA ");
	if (attrib&OAPISURFACE_UNCOMPRESS)	 strcat_s(buf, 512, "OAPISURFACE_UNCOMPRESS ");
	if (attrib&OAPISURFACE_SYSMEM)		 strcat_s(buf, 512, "OAPISURFACE_SYSMEM ");
	// Windows passed buf straight in as the format string: a '%' in the
	// caller-supplied 'origin' would send LogDbg reading absent arguments.
	LogDbg("BlueViolet", "%s", buf);
}

//-------------------------------------------------------------------------------------------
//
void VulkanDebugLog(const char *format, ...)
{
	va_list args;
	va_start(args, format);
	_vsnprintf_s(ErrBuf, ERRBUF, ERRBUF, format, args);
	va_end(args);

	VulkanDebugQueue.push(std::string(ErrBuf));
}

//-------------------------------------------------------------------------------------------
//
void VulkanDebugLogVec(const char* lbl, oapi::FVECTOR4 &v)
{
	sprintf_s(ErrBuf, ERRBUF, "%s = [%f, %f, %f, %f]", lbl, v.x, v.y, v.z, v.w);
	VulkanDebugQueue.push(std::string(ErrBuf));
}

//-------------------------------------------------------------------------------------------
//
void VulkanInitLog(const char *file)
{
	QueryPerformanceFrequency((LARGE_INTEGER*)&qpcFrq);
	QueryPerformanceCounter((LARGE_INTEGER*)&qpcStart);

	if (fopen_s(&vulkanclient_log,file,"w+")) { vulkanclient_log=NULL; } // Failed
	else {
		QueryPerformanceCounter((LARGE_INTEGER*)&qpcRef);
		InitializeCriticalSectionAndSpinCount(&LogCrit, 256);
		fprintf_s(vulkanclient_log,"<!DOCTYPE html><html><head><title>VulkanClient Log</title></head><body bgcolor=black text=white>");
		fprintf_s(vulkanclient_log,"<center><h2>VulkanClient Log</h2><br>");
		fprintf_s(vulkanclient_log,"</center><hr><br><br>");
	}
}

//-------------------------------------------------------------------------------------------
//
void VulkanCloseLog()
{
	if (vulkanclient_log) {
		fprintf(vulkanclient_log,"</body></html>");
		fclose(vulkanclient_log);
		vulkanclient_log = NULL;
		DeleteCriticalSection(&LogCrit);
	}
}

//-------------------------------------------------------------------------------------------
//
double VulkanGetTime()
{
	__int64 qpcCurrent;
	QueryPerformanceCounter((LARGE_INTEGER*)&qpcCurrent);
	return double(qpcCurrent) * 1e6 / double(qpcFrq);
}

//-------------------------------------------------------------------------------------------
//
void VulkanSetTime(VulkanTime &inout, double ref)
{
	__int64 qpcCurrent;
	QueryPerformanceCounter((LARGE_INTEGER*)&qpcCurrent);
	double time = double(qpcCurrent) * 1e6 / double(qpcFrq);
	inout.time += (time - ref);
	inout.count += 1.0;
	inout.peak = std::max((time - ref), inout.peak);
}

//-------------------------------------------------------------------------------------------
//
char *my_ctime()
{
	__int64 qpcCurrent;
	QueryPerformanceCounter((LARGE_INTEGER*)&qpcCurrent);
	double time = double(qpcCurrent-qpcRef) * 1e3 / double(qpcFrq);
	double start = double(qpcCurrent-qpcStart) / double(qpcFrq);
	sprintf_s(OprBuf,OPRBUF,"%d: %.1fs %05.2fms", iLine++, start, time);
	qpcRef = qpcCurrent;
	return OprBuf;
}

//-------------------------------------------------------------------------------------------
//
void escape_ErrBuf () {
	std::string buf(ErrBuf);
	size_t n = 0;
	n += replace_all(buf, "&", "&amp;");
	n += replace_all(buf, "<", "&lt;");
	n += replace_all(buf, ">", "&gt;");
	if (n) {
		strcpy_s(ErrBuf, ARRAYSIZE(ErrBuf), buf.c_str());
	}
}

//-------------------------------------------------------------------------------------------
//
void LogTrace(const char *format, ...)
{
	if (vulkanclient_log==NULL) return;
	if (iLine>LOG_MAX_LINES) return;
	if (uEnableLog>3) {
		EnterCriticalSection(&LogCrit);
		DWORD th = GetCurrentThreadId();
		fprintf(vulkanclient_log, "<font color=Gray>(%s)(0x%lX)</font><font color=DarkGrey> ", my_ctime(), (unsigned long)th);

		va_list args;
		va_start(args, format);
		_vsnprintf_s(ErrBuf, ERRBUF, ERRBUF, format, args);
		va_end(args);

		escape_ErrBuf();
		fputs(ErrBuf,vulkanclient_log);
		fputs("</font><br>\n",vulkanclient_log);
		fflush(vulkanclient_log);
		LeaveCriticalSection(&LogCrit);
	}
}

//-------------------------------------------------------------------------------------------
//
void LogAlw(const char *format, ...)
{
	if (vulkanclient_log==NULL) return;
	if (iLine>LOG_MAX_LINES) return;
	if (uEnableLog>0) {
		EnterCriticalSection(&LogCrit);
		DWORD th = GetCurrentThreadId();
		fprintf(vulkanclient_log, "<font color=Gray>(%s)(0x%lX)</font><font color=Olive> ", my_ctime(), (unsigned long)th);

		va_list args;
		va_start(args, format);

		_vsnprintf_s(ErrBuf, ERRBUF, ERRBUF, format, args);

		va_end(args);

		escape_ErrBuf();
		fputs(ErrBuf,vulkanclient_log);
		fputs("</font><br>\n",vulkanclient_log);
		fflush(vulkanclient_log);
		LeaveCriticalSection(&LogCrit);
	}
}

//-------------------------------------------------------------------------------------------
//
void LogDbg(const char *color, const char *format, ...)
{
	if (vulkanclient_log == NULL) return;
	if (iLine>LOG_MAX_LINES) return;
	if (uEnableLog>2) {
		EnterCriticalSection(&LogCrit);

		DWORD th = GetCurrentThreadId();
		fprintf(vulkanclient_log, "<font color=Gray>(%s)(0x%lX)</font><font color=%s> ", my_ctime(), (unsigned long)th, color);

		va_list args;
		va_start(args, format);

		_vsnprintf_s(ErrBuf, ERRBUF, ERRBUF, format, args);

		va_end(args);

		escape_ErrBuf();
		fputs(ErrBuf, vulkanclient_log);
		fputs("</font><br>\n", vulkanclient_log);
		fflush(vulkanclient_log);

		LeaveCriticalSection(&LogCrit);
	}
}

//-------------------------------------------------------------------------------------------
//
void LogClr(const char *color, const char *format, ...)
{
	if (vulkanclient_log == NULL) return;
	if (iLine>LOG_MAX_LINES) return;
	if (uEnableLog>1) {
		EnterCriticalSection(&LogCrit);

		DWORD th = GetCurrentThreadId();
		fprintf(vulkanclient_log, "<font color=Gray>(%s)(0x%lX)</font><font color=%s> ", my_ctime(), (unsigned long)th, color);

		va_list args;
		va_start(args, format);

		_vsnprintf_s(ErrBuf, ERRBUF, ERRBUF, format, args);

		va_end(args);

		escape_ErrBuf();
		fputs(ErrBuf, vulkanclient_log);
		fputs("</font><br>\n", vulkanclient_log);
		fflush(vulkanclient_log);

		LeaveCriticalSection(&LogCrit);
	}
}

//-------------------------------------------------------------------------------------------
//
void LogOapi(const char *format, ...)
{

	if (vulkanclient_log==NULL) return;
	if (iLine>LOG_MAX_LINES) return;
	if (uEnableLog>0) {
		EnterCriticalSection(&LogCrit);
		DWORD th = GetCurrentThreadId();
		fprintf(vulkanclient_log, "<font color=Gray>(%s)(0x%lX)</font><font color=Olive> ", my_ctime(), (unsigned long)th);

		va_list args;
		va_start(args, format);
		_vsnprintf_s(ErrBuf, ERRBUF, ERRBUF, format, args);
		va_end(args);

		oapiWriteLogV("Vulkan: %s", ErrBuf);

		escape_ErrBuf();
		fputs(ErrBuf,vulkanclient_log);
		fputs("</font><br>\n",vulkanclient_log);
		fflush(vulkanclient_log);
		LeaveCriticalSection(&LogCrit);
	}
}

// ---------------------------------------------------
//
void LogErr(const char *format, ...)
{
	if (vulkanclient_log==NULL) return;
	if (iLine>LOG_MAX_LINES) return;
	if (uEnableLog>0) {
		EnterCriticalSection(&LogCrit);
		DWORD th = GetCurrentThreadId();
		fprintf(vulkanclient_log,"<font color=Gray>(%s)(0x%lX)</font><font color=Red> [ERROR] ", my_ctime(), (unsigned long)th);

		va_list args;
		va_start(args, format);
		_vsnprintf_s(ErrBuf, ERRBUF, ERRBUF, format, args);
		va_end(args);

		oapiWriteLogV("VulkanERROR: %s", ErrBuf);

		escape_ErrBuf();
		fputs(ErrBuf,vulkanclient_log);
		fputs("</font><br>\n",vulkanclient_log);
		fflush(vulkanclient_log);
		LeaveCriticalSection(&LogCrit);
	}
}

// ---------------------------------------------------
//
void LogBlu(const char *format, ...)
{
	if (vulkanclient_log==NULL) return;
	if (iLine>LOG_MAX_LINES) return;
	if (uEnableLog>1) {
		EnterCriticalSection(&LogCrit);
		DWORD th = GetCurrentThreadId();
		fprintf(vulkanclient_log,"<font color=Gray>(%s)(0x%lX)</font><font color=#1E90FF> ", my_ctime(), (unsigned long)th);

		va_list args;
		va_start(args, format);
		_vsnprintf_s(ErrBuf, ERRBUF, ERRBUF, format, args);
		va_end(args);

		escape_ErrBuf();
		fputs(ErrBuf,vulkanclient_log);
		fputs("</font><br>\n",vulkanclient_log);
		fflush(vulkanclient_log);
		LeaveCriticalSection(&LogCrit);
	}
}

// ---------------------------------------------------
//
void LogWrn(const char *format, ...)
{
	if (vulkanclient_log==NULL) return;
	if (iLine>LOG_MAX_LINES) return;
	if (uEnableLog>1) {
		EnterCriticalSection(&LogCrit);
		DWORD th = GetCurrentThreadId();
		fprintf(vulkanclient_log,"<font color=Gray>(%s)(0x%lX)</font><font color=Yellow> [WARNING] ", my_ctime(), (unsigned long)th);

		va_list args;
		va_start(args, format);
		_vsnprintf_s(ErrBuf, ERRBUF, ERRBUF, format, args);
		va_end(args);

		escape_ErrBuf();
		fputs(ErrBuf,vulkanclient_log);
		fputs("</font><br>\n",vulkanclient_log);
		fflush(vulkanclient_log);
		oapiWriteLogV("VulkanInfo: %s", ErrBuf);
		LeaveCriticalSection(&LogCrit);
	}
}

// ---------------------------------------------------
//
void LogBreak(const char* format, ...)
{
	if (vulkanclient_log == NULL) return;
	if (iLine > LOG_MAX_LINES) return;
	if (uEnableLog > 1) {

		EnterCriticalSection(&LogCrit);
		DWORD th = GetCurrentThreadId();
		fprintf(vulkanclient_log, "<font color=Gray>(%s)(0x%lX)</font><font color=Yellow> [WARNING] ", my_ctime(), (unsigned long)th);

		va_list args;
		va_start(args, format);
		_vsnprintf_s(ErrBuf, ERRBUF, ERRBUF, format, args);
		va_end(args);

		escape_ErrBuf();
		fputs(ErrBuf, vulkanclient_log);
		fputs("</font><br>\n", vulkanclient_log);
		fflush(vulkanclient_log);
		oapiWriteLogV("VulkanDebug: %s", ErrBuf);
		LeaveCriticalSection(&LogCrit);

		if (Config->DebugBreak) DebugBreak();
	}
}

// ---------------------------------------------------
//
void LogOk(const char *format, ...)
{
	/*if (vulkanclient_log==NULL) return;
	if (iLine>LOG_MAX_LINES) return;
	if (uEnableLog>2) {
		EnterCriticalSection(&LogCrit);
		DWORD th = GetCurrentThreadId();
		fprintf(vulkanclient_log,"<font color=Gray>(%s)(0x%lX)</font><font color=#00FF00> ", my_ctime(), (unsigned long)th);

		va_list args;
		va_start(args, format);
        _vsnprintf_s(ErrBuf, ERRBUF, ERRBUF, format, args);
        va_end(args);

		escape_ErrBuf();
		fputs(ErrBuf,vulkanclient_log);
		fputs("</font><br>\n",vulkanclient_log);
		fflush(vulkanclient_log);
		LeaveCriticalSection(&LogCrit);
	}*/
}
