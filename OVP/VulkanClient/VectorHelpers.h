// =================================================================================================================================
// The MIT Lisence:
//
// Copyright (C) 2013-2026 Jarmo Nikkanen
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
// D3DXVECTOR3/4 become oapi::FVECTOR3/FVECTOR4 -- Orbiter's own SDK types from
// DrawAPI.h, which DrawAPI.h's own comment describes as compatible with the
// D3DXVECTOR types. They already define most of the operator helpers this file
// used to supply, and a second definition of one is an ambiguity error rather
// than a harmless duplicate, so those helpers are gone rather than renamed.
//
// The _MSC_VER block that supplied exp2/log2/log1p for Visual Studio before
// 2015 is gone; <cmath> has all three. _constexpr_ keeps its name so that
// nothing which spells it has to change. <algorithm> is included explicitly
// for std::max/std::min, which MSVC pulled in through another header.
// =================================================================================================================================


#include "OrbiterAPI.h"
#include "DrawAPI.h"
#include <math.h>
#include <algorithm>


#ifndef __VECTORHELPERS_H
#define __VECTORHELPERS_H

using oapi::FVECTOR3;
using oapi::FVECTOR4;

#define  _constexpr_ constexpr

template <typename T> inline _constexpr_ T sign (T val)
{
	return val < T(0) ? T(-1) : T(1);
}

template <typename T> inline _constexpr_ T lerp (T a, T b, T x)
{
	return a + (b - a)*x;
}
// ...slightly faster than the above, but not available in Visual Studio 2012 (I think)?
//template <typename T> inline _constexpr_ T lerp(T v0, T v1, T t) {
//	return fma(t, v1, fma(-t, v0, v0));
//}

template <typename T> inline _constexpr_ T saturate (T val)
{
	return (val > T(1)) ? T(1)
		 : (val < T(0)) ? T(0)
		 : val;
}

template <typename T> inline _constexpr_ T clamp(T x, T a, T b)
{
	return x > b ? b
		 : x < a ? a
		 : x;
}

template <typename T> inline _constexpr_ T ilerp(T a, T b, T x)
{
	return saturate((x - a) / (b - a));
}

template <typename T> inline _constexpr_ T sqr(T a)
{
	return a * a;
}

template <typename T> inline _constexpr_ T hermite(T a)
{
	return a * a * (T(3) - T(2)*a);
}

	
// VECTOR3 Helpers ==================================================================
//
inline VECTOR3 &operator+= (VECTOR3 &v, double d)
{
	v.x+=d; v.y+=d;	v.z+=d;
	return v;
} 

inline VECTOR3 &operator-= (VECTOR3 &v, double d)
{
	v.x-=d; v.y-=d;	v.z-=d;
	return v;
} 

inline VECTOR3 operator+ (const VECTOR3 &v, double d)
{
	return _V(v.x+d, v.y+d, v.z+d);
}

inline VECTOR3 operator- (const VECTOR3 &v, double d)
{
	return _V(v.x-d, v.y-d, v.z-d);
}

inline VECTOR3 operator- (double d, const VECTOR3 &v)
{
	return _V(d-v.x, d-v.y, d-v.z);
}

inline VECTOR3 exp2(const VECTOR3 &v)
{
	return _V(exp2(v.x), exp2(v.y), exp2(v.z));
}

inline VECTOR3 rcp(const VECTOR3 &v)
{
	return _V(1.0/v.x, 1.0/v.y, 1.0/v.z);
}

inline VECTOR3 vmax(const VECTOR3 &v, const VECTOR3 &w)
{
	return _V(std::max(v.x, w.x), std::max(v.y, w.y), std::max(v.z, w.z));
}

inline VECTOR3 vmin(const VECTOR3 &v, const VECTOR3 &w)
{
	return _V(std::min(v.x, w.x), std::min(v.y, w.y), std::min(v.z, w.z));
}


// VECTOR4 Helpers ==================================================================
//
//
inline VECTOR4 &operator+= (VECTOR4 &v, double d)
{
	v.x+=d; v.y+=d;	v.z+=d; v.w+=d;
	return v;
} 

inline VECTOR4 &operator-= (VECTOR4 &v, double d)
{
	v.x-=d; v.y-=d;	v.z-=d; v.w-=d;
	return v;
} 

inline VECTOR4 operator+ (const VECTOR4 &v, double d)
{
	return _V(v.x+d, v.y+d, v.z+d, v.w+d);
}

inline VECTOR4 operator- (const VECTOR4 &v, double d)
{
	return _V(v.x-d, v.y-d, v.z-d, v.w-d);
}

inline VECTOR4 operator- (double d, const VECTOR4 &v)
{
	return _V(d-v.x, d-v.y, d-v.z, d-v.w);
}

inline VECTOR4 &operator*= (VECTOR4 &v, double d)
{
	v.x*=d; v.y*=d;	v.z*=d; v.w*=d;
	return v;
} 

inline VECTOR4 &operator/= (VECTOR4 &v, double d)
{
	d=1.0/d; v.x*=d; v.y*=d; v.z*=d; v.w*=d;
	return v;
} 

inline VECTOR4 operator* (const VECTOR4 &v, double d)
{
	return _V(v.x*d, v.y*d, v.z*d, v.w*d);
}

inline VECTOR4 operator/ (const VECTOR4 &v, double d)
{
	d=1.0/d; return _V(v.x*d, v.y*d, v.z*d, v.w*d);
}

inline double dotp(const VECTOR4 &v, const VECTOR4 &w)
{
	return v.x*w.x + v.y*w.y + v.z*w.z + v.w*w.w;
}

inline double length(const VECTOR4 &v)
{
	return sqrt(dotp(v,v));
}

inline VECTOR4 normalize(const VECTOR4 &v)
{
	return v*(1.0/sqrt(dotp(v,v)));
}

inline VECTOR4 exp2(const VECTOR4 &v)
{
	return _V(exp2(v.x), exp2(v.y), exp2(v.z), exp2(v.w));
}

inline VECTOR4 rcp(const VECTOR4 &v)
{
	return _V(1.0/v.x, 1.0/v.y, 1.0/v.z, 1.0/v.w);
}

inline VECTOR4 vmax(const VECTOR4 &v, const VECTOR4 &w)
{
	return _V(std::max(v.x, w.x), std::max(v.y, w.y), std::max(v.z, w.z), std::max(v.w, w.w));
}

inline VECTOR4 vmin(const VECTOR4 &v, const VECTOR4 &w)
{
	return _V(std::min(v.x, w.x), std::min(v.y, w.y), std::min(v.z, w.z), std::min(v.w, w.w));
}


// FVECTOR3 Helpers =================================================================
//
// Was the D3DXVECTOR3 section. The operator helpers that stood here are gone:
// FVECTOR3 defines all of them itself, and redefining one is an ambiguity
// error rather than a duplicate.
//
inline FVECTOR3 exp2(const FVECTOR3 &v)
{
	return FVECTOR3(exp2(v.x), exp2(v.y), exp2(v.z));
}

// Was _D3DXVECTOR3(). FVECTOR3 has a VECTOR3 constructor of its own, so the
// first of these is redundant; kept so the call sites need a rename rather
// than a rewrite.
inline FVECTOR3 _FVECTOR3(const VECTOR3 &v)
{
	return FVECTOR3(float(v.x), float(v.y), float(v.z));
}

inline FVECTOR3 _FVECTOR3(double x, double y, double z)
{
	return FVECTOR3(float(x), float(y), float(z));
}

inline FVECTOR3 rcp(const FVECTOR3 &v)
{
	return FVECTOR3(1.0f/v.x, 1.0f/v.y, 1.0f/v.z);
}

inline FVECTOR3 vmax(const FVECTOR3 &v, const FVECTOR3 &w)
{
	return FVECTOR3(std::max(v.x, w.x), std::max(v.y, w.y), std::max(v.z, w.z));
}

inline FVECTOR3 vmin(const FVECTOR3 &v, const FVECTOR3 &w)
{
	return FVECTOR3(std::min(v.x, w.x), std::min(v.y, w.y), std::min(v.z, w.z));
}

// The Windows file's lerp(D3DXVECTOR3,...) is not carried over: the SDK already
// has lerp for FVECTOR2/3/4, and because FVECTOR3 is an oapi type,
// argument-dependent lookup finds it at every call site whether the site says
// so or not. A second one in the global namespace is an ambiguity error rather
// than an override.



// FVECTOR4 Helpers =================================================================
//
// Was the D3DXVECTOR4 section. operator- (v,float) and operator+ (v,float) are
// gone -- FVECTOR4 has both. operator*= (v,v) stays, because FVECTOR4 has only
// the float form of *= and nothing supplies the per-component one.
//
// The three signatures below keep the Windows file's non-const references, so
// that a call which compiles there compiles here.
//
inline FVECTOR4 abs(FVECTOR4 &a)
{
	// fabsf, not abs: with libstdc++ a bare abs() on a float can bind to the
	// int overload from <stdlib.h> and truncate. MSVC picked the float one.
	return FVECTOR4(fabsf(a.x), fabsf(a.y), fabsf(a.z), fabsf(a.w));
}

inline FVECTOR4 sign(FVECTOR4 &a)
{
	return FVECTOR4(sign(a.x), sign(a.y), sign(a.z), sign(a.w));
}

inline FVECTOR4 pow(float x, FVECTOR4 &y)
{
	return FVECTOR4(powf(x, y.x), powf(x, y.y), powf(x, y.z), powf(x, y.w));
}

inline FVECTOR4 &operator*= (FVECTOR4 &v, const FVECTOR4 &d)
{
	v.x*=d.x; v.y*=d.y;	v.z*=d.z; v.w*=d.w;
	return v;
}


// D3DXIntersectTri, written out =====================================================
//
// Added, not converted: D3DX supplied it and Orbiter's SDK has no ray/triangle
// test of any kind. It lives in the math header because there are two callers
// -- Tile::Pick in Tilemgr2.cpp and VulkanMesh::Pick in Mesh.cpp -- and a copy
// per file is how two of them end up disagreeing.
//
// Moller-Trumbore. The output convention is the part that matters, because
// getting it wrong gives a hit at the wrong point rather than no hit at all:
// u and v are barycentric against the FIRST vertex, so the hit point is
// p0 + u*(p1-p0) + v*(p2-p0), and dist is in units of the ray direction's own
// length -- the direction is not normalised by either version. Both callers
// pin that down: each calls with (_c, _b, _a) and reconstructs the point as
// _b*u + _a*v + _c*(1-u-v).
//
// This is the two-sided variant where D3DX's is one-sided, but both callers
// have already established the facing with their own dot product before
// calling.

inline bool IntersectTri(const oapi::FVECTOR3 &p0, const oapi::FVECTOR3 &p1, const oapi::FVECTOR3 &p2,
						 const oapi::FVECTOR3 &org, const oapi::FVECTOR3 &dir,
						 float *pU, float *pV, float *pDist)
{
	const float eps = 1e-9f;

	oapi::FVECTOR3 e1 = p1 - p0;
	oapi::FVECTOR3 e2 = p2 - p0;
	oapi::FVECTOR3 pv = cross(dir, e2);

	float det = dot(e1, pv);
	if (fabs(det) < eps) return false;		// ray parallel to the triangle

	float inv = 1.0f / det;

	oapi::FVECTOR3 tv = org - p0;
	float u = dot(tv, pv) * inv;
	if (u < 0.0f || u > 1.0f) return false;

	oapi::FVECTOR3 qv = cross(tv, e1);
	float v = dot(dir, qv) * inv;
	if (v < 0.0f || u + v > 1.0f) return false;

	float t = dot(e2, qv) * inv;
	if (t < 0.0f) return false;				// behind the ray origin

	*pU = u; *pV = v; *pDist = t;
	return true;
}


// D3DX matrix functions with no SDK counterpart ====================================
//
// Added, not converted: D3DXMatrixInverse has call sites all over the Windows
// client, and Orbiter's SDK stops short of an inverse of any kind -- DrawAPI.h
// has mul, tmul, TransformCoord, TransformNormal, dot, cross, length,
// normalize and unit, and nothing else.
//
// FMATRIX4 stores its sixteen floats as m11..m44, row by row, and data[16] is
// the same storage. The algorithm below treats data[i*4+j] as element (i,j),
// so it returns the inverse in exactly the layout it was given and never has
// to know which convention that is.

inline bool MatrixInverse(oapi::FMATRIX4 &out, const oapi::FMATRIX4 &in, float *pDet = nullptr)
{
	const float *m = in.data;
	float inv[16];

	inv[0]  =  m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15]
	         + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
	inv[4]  = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15]
	         - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
	inv[8]  =  m[4]*m[9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15]
	         + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
	inv[12] = -m[4]*m[9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14]
	         - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];

	inv[1]  = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15]
	         - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
	inv[5]  =  m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15]
	         + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
	inv[9]  = -m[0]*m[9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15]
	         - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
	inv[13] =  m[0]*m[9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14]
	         + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];

	inv[2]  =  m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15]
	         + m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6];
	inv[6]  = -m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15]
	         - m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6];
	inv[10] =  m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15]
	         + m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5];
	inv[14] = -m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14]
	         - m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5];

	inv[3]  = -m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11]
	         - m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6];
	inv[7]  =  m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11]
	         + m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6];
	inv[11] = -m[0]*m[5]*m[11] + m[0]*m[7]*m[9] + m[4]*m[1]*m[11]
	         - m[4]*m[3]*m[9] - m[8]*m[1]*m[7] + m[8]*m[3]*m[5];
	inv[15] =  m[0]*m[5]*m[10] - m[0]*m[6]*m[9] - m[4]*m[1]*m[10]
	         + m[4]*m[2]*m[9] + m[8]*m[1]*m[6] - m[8]*m[2]*m[5];

	float det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
	if (pDet) *pDet = det;

	// D3DXMatrixInverse returns NULL for a singular matrix; this returns
	// false and leaves `out` untouched, so a caller that ignores the result
	// gets the matrix it already had rather than a matrix of infinities.
	if (det == 0.0f) return false;

	det = 1.0f / det;
	for (int i = 0; i < 16; i++) out.data[i] = inv[i] * det;
	return true;
}

#endif // __VECTORHELPERS_H
