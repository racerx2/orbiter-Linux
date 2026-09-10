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
// CONVERTED FROM OVP/D3D9Client/VectorHelpers.h. What changed, and why:
//
//   1. THE D3DXVECTOR HALVES BECAME FVECTOR HALVES. D3DXVECTOR3/4 come from
//      <d3dx9.h>, which does not exist on Linux. Their replacement is not
//      invented here -- Orbiter's own SDK already carries oapi::FVECTOR3 and
//      oapi::FVECTOR4 (Orbitersdk/include/DrawAPI.h:196 and :367), they are
//      cross-platform, DrawAPI.h's own comment says FVECTOR4 "is compatible
//      with the D3DXVECTOR4 type", and the Sketchpad API this client
//      implements is already written in terms of them.
//
//   2. MOST OF THE OPERATOR HELPERS DROPPED OUT, because FVECTOR3 and
//      FVECTOR4 already define them and a second definition is an ambiguity
//      error, not a harmless duplicate. Read out of DrawAPI.h rather than
//      assumed:
//
//        FVECTOR3 already has  *= /= += -= * / + -  against float AND FVECTOR3
//        FVECTOR4 already has  *= /= += -= * / + -  against float,
//                              + and - against FVECTOR4, and unary -
//
//      So of the D3DX operator helpers the Windows file carried, exactly ONE
//      survives: operator*= (FVECTOR4&, const FVECTOR4&). FVECTOR4 has the
//      float form of *= and no vector form. The others are gone because the
//      type grew them, not because anything was dropped.
//
//   3. _D3DXVECTOR3() BECAME _FVECTOR3(), and the VECTOR3 overload is kept
//      even though FVECTOR3's own VECTOR3 constructor makes it redundant.
//      Call sites spell it as a function in a good many places, and a
//      mechanical rename is a smaller change than rewriting each of them.
//
//   4. THE _MSC_VER BLOCK IS GONE. It supplied exp2/log2/log1p for Visual
//      Studio versions before 2015; <cmath> has all three here. _constexpr_
//      keeps its name so that nothing which spells it has to change, and is
//      simply constexpr now.
//
//   5. <algorithm> IS INCLUDED EXPLICITLY. The file uses std::max and
//      std::min; MSVC pulled them in through another header, libstdc++ does
//      not.
//
//   6. abs() ON A FLOAT IS fabsf() HERE, and this is the one place the
//      conversion changes behaviour rather than spelling. The Windows file
//      writes abs(a.x) on floats. MSVC resolves that to the floating-point
//      overload; with libstdc++ a bare ::abs can resolve to the INT overload
//      from <stdlib.h> and silently truncate 0.5f to 0. Getting it wrong is
//      invisible -- the vector still comes back with four numbers in it.
//
// NOT CONVERTED, because none of it needed converting: the scalar templates
// and the VECTOR3/VECTOR4 halves. VECTOR3, VECTOR4 and _V() are Orbiter's own
// and already build on Linux.
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
// Unchanged from the Windows file. VECTOR3 and _V() are Orbiter's own.
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
// Unchanged from the Windows file.
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
// Was the D3DXVECTOR3 section. The five operator helpers that stood here --
// operator* (v,v), operator+ (v,float), operator*= (v,v), operator+= (v,float)
// -- are gone: FVECTOR3 defines all of them itself (DrawAPI.h:254-336), and
// redefining one is an ambiguity error rather than a duplicate.
//
inline FVECTOR3 exp2(const FVECTOR3 &v)
{
	return FVECTOR3(exp2(v.x), exp2(v.y), exp2(v.z));
}

// Was _D3DXVECTOR3(). FVECTOR3 has a VECTOR3 constructor of its own, so the
// first of these is redundant on its own terms and is kept only so that the
// call sites need a rename and not a rewrite.
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

// The Windows file's lerp(D3DXVECTOR3,...) is NOT carried over: the SDK
// already has lerp for FVECTOR2/3/4 (DrawAPI.h:819-833). Because FVECTOR3 is
// an oapi type, argument-dependent lookup finds the SDK's copy at every call
// site whether or not the site says so, and a second one in the global
// namespace is an AMBIGUITY error rather than an override. Found by compiling,
// not by reading -- which is the whole reason this file gets a syntax check
// before the next one is started.



// FVECTOR4 Helpers =================================================================
//
// Was the D3DXVECTOR4 section. operator- (v,float) and operator+ (v,float) are
// gone -- FVECTOR4 has both (DrawAPI.h:533-543). operator*= (v,v) STAYS,
// because FVECTOR4 has only the float form of *= and nothing supplies the
// per-component one.
//
// The three signatures below take a non-const reference exactly as the Windows
// file did. It is not an oversight worth correcting: a const& would be a
// widening no call site needs, and keeping the signature identical means a
// call that compiles there compiles here.
//
inline FVECTOR4 abs(FVECTOR4 &a)
{
	// fabsf, NOT abs. With libstdc++ a bare abs() on a float can bind to the
	// int overload out of <stdlib.h> and truncate; MSVC picked the float one.
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
// ADDED, not converted, for the same reason MatrixInverse below is: D3DX
// supplied it, Linux has no D3DX, and Orbiter's SDK has no ray/triangle test
// of any kind. It lives here rather than in the file that needed it first
// because there are TWO callers -- Tile::Pick in Tilemgr2.cpp and
// VulkanMesh::Pick in Mesh.cpp -- and a copy per file is how two of them end
// up disagreeing.
//
// It is Moller-Trumbore, and the OUTPUT CONVENTION is the part that had to be
// established rather than guessed, because getting it wrong produces a hit at
// the wrong point rather than no hit at all:
//
//   u, v are barycentric coordinates against the FIRST vertex, so the hit
//   point is p0 + u*(p1-p0) + v*(p2-p0).
//   dist is measured in units of the ray direction's own length; the
//   direction is NOT normalised by either version.
//
// Both callers pin that down: each calls with (_c, _b, _a) and then
// reconstructs the point as _b*u + _a*v + _c*(1-u-v), which is exactly
// _c + u*(_b-_c) + v*(_a-_c).
//
// This is the two-sided variant -- it accepts a hit from either face. D3DX's
// is one-sided, but both callers have already established the facing with
// their own dot product before calling, so on every path that reaches here
// the two agree.

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


// D3DX MATRIX FUNCTIONS WITH NO SDK COUNTERPART ====================================
//
// ADDED, not converted -- there is nothing in the Windows VectorHelpers.h that
// corresponds to this. It is here because D3DX supplied it, Linux has no D3DX,
// and Orbiter's SDK stops short of it: DrawAPI.h has mul, tmul, TransformCoord,
// TransformNormal, dot, cross, length, normalize and unit, and no inverse of
// any kind (checked across DrawAPI.h and OrbiterAPI.h before writing this).
//
// The client's math header is where it belongs rather than in whichever file
// needed it first: D3DXMatrixInverse has call sites all over the Windows
// client, and a copy per file is how two of them end up disagreeing.
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
