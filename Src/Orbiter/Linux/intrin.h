// Linux <intrin.h> — MSVC's compiler-intrinsics header.
//
// Included by OVP/D3D9Client/samples/DrawOrbits/Tools.cpp, which does not
// actually call any intrinsic: the include is left over from code that once
// did. GCC and Clang provide the intrinsics they support through <x86intrin.h>
// and their own builtins, so this forwards there when the target has it and is
// otherwise empty.
//
// Nothing is emulated. If a source in this tree ever calls a genuine MSVC
// intrinsic (__cpuid, _BitScanForward and so on), it will fail to compile
// here rather than silently do the wrong thing, which is the intended
// behaviour -- adding a wrong implementation would be worse than not having
// one.

#ifndef ORBITER_LINUX_INTRIN_H
#define ORBITER_LINUX_INTRIN_H

#ifdef _WIN32
#error "This header is for non-Windows builds only."
#endif

#if defined(__i386__) || defined(__x86_64__)
#include <x86intrin.h>
#endif

#endif // ORBITER_LINUX_INTRIN_H
