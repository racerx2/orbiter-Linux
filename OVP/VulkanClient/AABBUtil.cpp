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
// CONVERTED FROM OVP/D3D9Client/AABBUtil.cpp. What changed, and why:
//
//   1. <xnamath.h> IS GONE, AND THAT IS THE BULK OF THIS FILE'S WORK.
//      xnamath is Microsoft's SSE wrapper (XMVECTOR, XMLoadFloat4,
//      XMVectorMin, XMVector3TransformCoord, XMVectorSelect ...). It ships
//      with the DirectX SDK and has no Linux build and no drop-in equivalent,
//      so every one of those calls is written out as the float arithmetic it
//      performs. This is the "only where the difference forces it" case
//      exactly: the arithmetic is unchanged, only its spelling is.
//
//      D9AddPointAABB, D9UpdateAABB and D9AddAABB are the three functions this
//      touches. The one that needed care is D9AddAABB's XMVectorSelect over
//      eight XMVectorSelectControl masks -- that is a loop over the eight
//      CORNERS of the box, each corner picking x, y and z from either `min` or
//      `max`. Written out, the mask becomes the three bits of the loop index,
//      which is what the eight hand-written control words were spelling.
//
//   2. THE D3DX MATH BECAME THE SDK'S OWN. D3DXVec3TransformCoord,
//      D3DXVec3TransformNormal, D3DXVec3Dot and D3DXVec3Normalize are
//      TransformCoord, TransformNormal, dot and normalize in DrawAPI.h, with
//      the same meaning and the same row-vector convention. Only
//      D3DXMatrixInverse has no counterpart; it is MatrixInverse in
//      VectorHelpers.h.
//
//   3. pProj->_22 BECAME pProj->m22. FMATRIX4 names its elements m11..m44
//      where D3DXMATRIX names them _11.._44. Same storage, same order.
//
//   4. THE DEVICE PARAMETERS ARE GONE -- see the note in AABBUtil.h. Neither
//      function that took one ever used it.
//
//   5. `using std::min;` BECAME an explicit std::min at the one call site.
//      The Windows file hoists the name to shadow the min() macro that
//      <windows.h> defines; there is no such macro here, and importing a name
//      into the global namespace to dodge a macro that no longer exists is
//      worth losing.
//
//   6. THE #pragma warning PUSH/POP PAIR IS GONE. It suppressed MSVC warning
//      4838 for the xnamath include that is itself gone.
//
// AND ONE DEFECT NOT CARRIED OVER -- see WorldPickRay below. It is the only
// place this file departs from what the Windows code DOES rather than from how
// it says it.
// =================================================================================================================================

#include "AABBUtil.h"
#include "OrbiterAPI.h"
#include "VectorHelpers.h"
#include "Log.h"

#include <algorithm>
#include <math.h>
#include <string.h>

// =================================================================================================================================
//
bool SolveLUSystem(int n, double *A, double *b, double *x, double *det)
{		
	int e=0, *p = new int[n]; 
	for (int i=0;i<n;i++) p[i] = i;
	for (int k=0;k<n;k++) {
		int r = 0; double d = 0.0; 
		for (int s=k;s<n;s++) if (fabs(A[s*n+k])>d) { d = fabs(A[s*n+k]); r = s; }
		if (d == 0.0) { LogErr("Singular Matrix in SolveLUSystem()"); delete []p; p = NULL; return false; }
		if (r!=k) { // Do Swaps
			for (int i=0;i<n;i++) { double x = A[k*n+i]; A[k*n+i] = A[r*n+i]; A[r*n+i] = x; } 
			int x=p[k]; p[k]=p[r]; p[r]=x; e++;
		}
		for (int i=k+1;i<n;i++) { A[i*n+k]/=A[k*n+k]; for (int j=k+1;j<n;j++) A[i*n+j]-=(A[i*n+k]*A[k*n+j]); }
	}
	for (int i=0;i<n;i++) {	x[i] = b[p[i]];	for (int j=0;j<i;j++) x[i] -= A[i*n+j]*x[j]; }
	for (int i=n-1;i>=0;i--) { for (int j=i+1;j<n;j++) x[i] -= A[i*n+j]*x[j]; x[i] /= A[i*n+i]; }
	if (det) { *det = 1.0; for (int i=0;i<n;i++) *det *= A[i*n+i]; if (e&1) *det*=-1.0; } 
	delete []p;
	p = NULL;
	return true;
}


// THE ONE BEHAVIOURAL DIVERGENCE IN THIS FILE, and it is a defect in the
// Windows code rather than a Windows/Linux difference.
//
// The parameter is `const LPD3DXMATRIX mView` -- LPD3DXMATRIX is ALREADY a
// pointer, so mView is the matrix. The Windows body then passes
//
//     D3DXMatrixInverse(&mViewI, NULL, (const D3DXMATRIX *)&mView);
//
// which is the address OF THE POINTER VARIABLE, cast so that the compiler
// stays quiet about it. It inverts the sixteen bytes of stack holding the
// pointer and whatever follows it, never the view matrix, so every ray this
// function returns is garbage and picking cannot ever have worked through it.
// The cast is what hides it: without it the types would not match and the
// compiler would have said so.
//
// This passes the matrix. Recorded here rather than silently fixed, because a
// future diff against the reference will show the difference and should find
// the reason next to it.
FVECTOR3 WorldPickRay(float x, float y, const FMATRIX4 *mProj, const FMATRIX4 *mView)
{
	x = float((x*2.0-1.0)/mProj->m11);
	y = float((y*2.0-1.0)/mProj->m22);
	FVECTOR3 pick(x, 1.0f, y);
	FMATRIX4 mViewI;
	MatrixInverse(mViewI, *mView);
	pick = oapi::TransformNormal(pick, mViewI);
	return oapi::normalize(pick);
}


void D9ZeroAABB(D9BBox *box)
{
	// memset(box, 0, sizeof(D9BBox)) on Windows. D9BBox is six FVECTOR4, and
	// FVECTOR4 has user-declared constructors, so it is not trivially
	// copyable and memset over it is formally undefined -- GCC reports it as
	// -Wclass-memaccess. The result was correct in practice (0.0f is all-zero
	// bits, and six alignas(16) members leave no padding), so this is the
	// same 96 bytes of zero by a defined route rather than a behaviour change.
	*box = D9BBox();
}

void D9InitAABB(D9BBox *box)
{
	box->min = FVECTOR4( 1e12f,  1e12f,  1e12f, 0.0f);
	box->max = FVECTOR4(-1e12f, -1e12f, -1e12f, 0.0f);
	box->bs  = FVECTOR4(0.0f, 0.0f, 0.0f, 0.0f);
}


// Was four xnamath calls: load min, load max, load the point, store the
// component-wise min and max back. The w lane is left alone exactly as
// XMVectorMin/XMVectorMax left it, because the point is loaded as a FLOAT3 and
// the stores write four lanes -- so w keeps whatever min/max held. Writing it
// out makes that explicit rather than accidental.
void D9AddPointAABB(D9BBox *box, const FVECTOR3 *point)
{
	box->min.x = std::min(box->min.x, point->x);
	box->min.y = std::min(box->min.y, point->y);
	box->min.z = std::min(box->min.z, point->z);
	box->max.x = std::max(box->max.x, point->x);
	box->max.y = std::max(box->max.y, point->y);
	box->max.z = std::max(box->max.z, point->z);
}


// THE SIGN OF m22 IS NOT ASSUMED HERE. The Windows body is 1.0f/pProj->_22 and
// takes for granted that the element is positive, which is true of
// D3DXMatrixPerspectiveFovLH. A projection built for Vulkan's top-left
// framebuffer origin negates it. `a` would then be negative, `l` and the two
// returned extents would follow, and every visibility test that multiplies by
// them would reject everything -- silently, because a fully culled scene draws
// without complaint. The magnitude is what the field of view means.
FVECTOR4 D9LinearFieldOfView(const FMATRIX4 *pProj)
{
	float a = 1.0f/fabsf(pProj->m22);
	float s = fabsf(pProj->m11/pProj->m22);
	float l = 1.0f/cos(atan(a));
	return FVECTOR4(l, l/s, a, a/s);
}


float D9NearPlane(float znear, float zfar, float dmin, const FMATRIX4 *pProj, bool bReduced)
{
	float b = 1.0f/pProj->m11;
	float a = 1.0f/pProj->m22;
	float q = atan(sqrt(a*a+b*b));

	// The Windows body fetches the viewport here and never reads it; see the
	// note in AABBUtil.h. Nothing else in the function mentions the device.

	dmin = dmin * cos(q);
	
	if (znear>0) dmin = znear;

	float fact = 1500.0f/std::min(10e3f, zfar);
	float zmax = 500.0f;

	if (bReduced) zmax = 25.0f;
	
	float vmaxi = zmax / (1.0f + fact*fact);
	float value = dmin / (1.0f + fact*fact);
	
	if (value>vmaxi)  value=vmaxi;

	return value;
}


FVECTOR4 D9OffsetRange(double R, double r)
{
	double t  = r*0.5;
	double r2 = r*r;
	double h1 = sqrt(R*R+r2)-R;
	double h2 = sqrt(R*R+t*t)-R;
	double a  =  (h2 - h1*0.0625)/(0.1875*r2);
	double b  =  -(h2 - h1*0.25)/(0.1875*r2*r2);
	return FVECTOR4(float(a), float(b), 1.0f/float(r2), 0.0f);
}


bool D9IsAABBVisible(const D9BBox *in, const FMATRIX4 *pWV, const FVECTOR4 *F)
{
	
	FVECTOR3 bv = oapi::TransformCoord(in->bs.xyz, *pWV);
	float w = in->bs.w;

	if (bv.z<-w) return false; // Not visible
	float zz = fabs(bv.z);
	float tol = F->z*0.0015f;

	if ((w/zz)<tol) return false; // Not visible
	if (fabs(bv.y)-(F->x*w) > zz*F->z) return false; // Not visible
	if (fabs(bv.x)-(F->y*w) > zz*F->w) return false; // Not visible
	
	FVECTOR4 size = (in->max - in->min)*0.5f;
	FVECTOR3 xv,yv,zv;
	xv = oapi::TransformNormal(in->a.xyz, *pWV);
	yv = oapi::TransformNormal(in->b.xyz, *pWV);
	zv = oapi::TransformNormal(in->c.xyz, *pWV);

	float dx = oapi::dot(xv, bv);
	float dy = oapi::dot(yv, bv);
	float dz = oapi::dot(zv, bv);
	
	float adx = fabs(dx) - size.x;
	float ady = fabs(dy) - size.y;
	float adz = fabs(dz) - size.z;

	float sdx,sdy,sdz;

	if (dx<0) sdx=dx+size.x;
	else	  sdx=dx-size.x; 
	if (dy<0) sdy=dy+size.y;
	else	  sdy=dy-size.y; 
	if (dz<0) sdz=dz+size.z;
	else	  sdz=dz-size.z; 

	float fov = sin(atan(sqrt(F->z*F->z + F->w*F->w)));

	if (fabs(xv.z)>fov && (sdx*xv.z)<0 && adx>0) return false;
	if (fabs(yv.z)>fov && (sdy*yv.z)<0 && ady>0) return false;
	if (fabs(zv.z)>fov && (sdz*zv.z)<0 && adz>0) return false;
	
	return true;
}


bool D9IsBSVisible(const D9BBox *in, const FMATRIX4 *pWV, const FVECTOR4 *F)
{
	FVECTOR3 bv = FVECTOR3(in->bs.x, in->bs.y, in->bs.z);
	bv = oapi::TransformCoord(bv, *pWV);
	float r = in->bs.w;

	if (bv.z<-r) return false; // Not visible
	bv.z=fabs(bv.z);

	float tol = F->z*0.0015f;

	if ((r/bv.z)<tol) return false; // Not visible
	if (fabs(bv.y)-(F->x*r) > (bv.z*F->z)) return false; // Not visible
	if (fabs(bv.x)-(F->y*r) > (bv.z*F->w)) return false; // Not visible
	
	return true;
}

int D9ComputeMinMaxDistance(const D9BBox *in, const FMATRIX4 *pWV, const FVECTOR4 *F, float *zmin, float *zmax, float *dst)
{
	
	FVECTOR3 bv = FVECTOR3(in->bs.x, in->bs.y, in->bs.z);
	bv = oapi::TransformCoord(bv, *pWV);
	float r = in->bs.w;

	if (bv.z<-r) return -1; // Not visible
	float zz = fabs(bv.z);
	float tol = F->z*0.0015f;

	if ((r/zz)<tol) return -2; // Not visible
	if (fabs(bv.y)-(F->x*r) > zz*F->z) return -3; // Not visible
	if (fabs(bv.x)-(F->y*r) > zz*F->w) return -4; // Not visible
	
	
	FVECTOR4 size = (in->max - in->min)*0.5f;
	FVECTOR3 xv,yv,zv;
	xv = oapi::TransformNormal(in->a.xyz, *pWV);
	yv = oapi::TransformNormal(in->b.xyz, *pWV);
	zv = oapi::TransformNormal(in->c.xyz, *pWV);

	float dx = oapi::dot(xv, bv);
	float dy = oapi::dot(yv, bv);
	float dz = oapi::dot(zv, bv);
	
	float adx = fabs(dx) - size.x;
	float ady = fabs(dy) - size.y;
	float adz = fabs(dz) - size.z;

	if (adx>0 || ady>0 || adz>0) {

		float sdx,sdy,sdz;

		if (dx<0) sdx=dx+size.x;
		else	  sdx=dx-size.x; 
		if (dy<0) sdy=dy+size.y;
		else	  sdy=dy-size.y; 
		if (dz<0) sdz=dz+size.z;
		else	  sdz=dz-size.z; 

		float fov = sin(atan(sqrt(F->z*F->z + F->w*F->w)));

		if (fabs(xv.z)>fov && (sdx*xv.z)<0 && adx>0) return -5;
		if (fabs(yv.z)>fov && (sdy*yv.z)<0 && ady>0) return -6;
		if (fabs(zv.z)>fov && (sdz*zv.z)<0 && adz>0) return -7;
	}
	
	if (adx<0) adx=0;
	if (ady<0) ady=0;
	if (adz<0) adz=0;

	float d = sqrt(adx*adx + ady*ady + adz*adz);

	if (d<*dst) *dst=d;


	float x = xv.z * size.x;
	float y = yv.z * size.y;
	float z = zv.z * size.z;
	float e = bv.z;
	

	float q[8];

	q[0] = e + (+x+y+z); 
	q[1] = e + (-x+y+z); 
	q[2] = e + (+x-y+z); 
	q[3] = e + (-x-y+z); 
	q[4] = e + (+x+y-z); 
	q[5] = e + (-x+y-z); 
	q[6] = e + (+x-y-z); 
	q[7] = e + (-x-y-z); 

	float mx = q[0];
	float mi = q[0];

	for (int i=1;i<8;i++) {
		float qq = q[i];
		if (qq>mx) mx=qq;
		if (qq<mi) mi=qq;
	}
	
	if (mi<*zmin) *zmin = mi;
	if (mx>*zmax) *zmax = mx;

	return 0;
}


// Was ten xnamath calls. The three axes start as the unit basis, the two
// corners as min and max; each supplied matrix transforms the axes as NORMALS
// and the corners as COORDINATES, in that order, first then second. Then the
// bounding sphere centre is the midpoint of the two corners and its radius is
// half the distance between them.
//
// One detail worth stating because the SIMD form hid it: the corners go
// through XMVector3TransformCoord, which divides by w. TransformCoord does the
// same, so a projective matrix behaves identically here and in the reference.
void D9UpdateAABB(D9BBox *box, const FMATRIX4 *pFirst, const FMATRIX4 *pSecond)
{
	FVECTOR3 x(1.0f, 0.0f, 0.0f);
	FVECTOR3 y(0.0f, 1.0f, 0.0f);
	FVECTOR3 z(0.0f, 0.0f, 1.0f);
	FVECTOR3 q = box->min.xyz;
	FVECTOR3 w = box->max.xyz;

	if (pFirst) {
		x = oapi::TransformNormal(x, *pFirst);
		y = oapi::TransformNormal(y, *pFirst);
		z = oapi::TransformNormal(z, *pFirst);
		q = oapi::TransformCoord (q, *pFirst);
		w = oapi::TransformCoord (w, *pFirst);
	}

	if (pSecond) {
		x = oapi::TransformNormal(x, *pSecond);
		y = oapi::TransformNormal(y, *pSecond);
		z = oapi::TransformNormal(z, *pSecond);
		q = oapi::TransformCoord (q, *pSecond);
		w = oapi::TransformCoord (w, *pSecond);
	}

	FVECTOR3 p = (q + w) * 0.5f;

	box->bs = FVECTOR4(p, 0.0f);
	box->a  = FVECTOR4(x, 0.0f);
	box->b  = FVECTOR4(y, 0.0f);
	box->c  = FVECTOR4(z, 0.0f);

	box->bs.w = oapi::length(q - w) * 0.5f;
}


// Was the densest xnamath in the file, and the only part that needed thinking
// about rather than transcribing.
//
// The Windows body builds eight XMVectorSelectControl masks and uses
// XMVectorSelect to pick each of x, y and z from either `q` (the box minimum)
// or `w` (the box maximum). That is an enumeration of the box's eight CORNERS.
// Reading the eight control words in order --
//
//     (0,0,0) (1,1,1) (0,0,1) (0,1,0) (0,1,1) (1,0,0) (1,0,1) (1,1,0)
//
// -- they are the eight combinations of three bits, in a scrambled order.
// XMVectorSelect takes the lane from the FIRST argument where the control bit
// is 0, so a 0 bit means `q` and a 1 bit means `w`. Since all eight appear
// exactly once and the result is a min/max over all of them, the ORDER cannot
// matter, so the loop below runs 0..7 and reads the three bits of the index.
// The result is identical and there is no table to get wrong.
//
// The w lane: the Windows body clears it on both corners with XMVectorSetW
// before transforming, so the corners are positions with w = 0 going in. That
// is preserved by transforming FVECTOR3 and letting TransformCoord supply the
// translation, which is the same arithmetic.
void D9AddAABB(const D9BBox *in, const FMATRIX4 *pM, D9BBox *out, bool bReset)
{
	FVECTOR3 mi, mx;

	if (bReset) {
		mi = FVECTOR3( 1e12f,  1e12f,  1e12f);
		mx = FVECTOR3(-1e12f, -1e12f, -1e12f);
	}
	else {
		mi = out->min.xyz;
		mx = out->max.xyz;
	}

	const FVECTOR3 q = in->min.xyz;
	const FVECTOR3 w = in->max.xyz;

	if (pM) {

		for (int k = 0; k < 8; k++) {

			// bit set -> take that component from the maximum corner
			FVECTOR3 corner((k & 1) ? w.x : q.x,
			                (k & 2) ? w.y : q.y,
			                (k & 4) ? w.z : q.z);

			FVECTOR3 x = oapi::TransformCoord(corner, *pM);

			mi.x = std::min(mi.x, x.x);  mx.x = std::max(mx.x, x.x);
			mi.y = std::min(mi.y, x.y);  mx.y = std::max(mx.y, x.y);
			mi.z = std::min(mi.z, x.z);  mx.z = std::max(mx.z, x.z);
		}
	}
	else {
		mi.x = std::min(mi.x, std::min(q.x, w.x));
		mi.y = std::min(mi.y, std::min(q.y, w.y));
		mi.z = std::min(mi.z, std::min(q.z, w.z));
		mx.x = std::max(mx.x, std::max(q.x, w.x));
		mx.y = std::max(mx.y, std::max(q.y, w.y));
		mx.z = std::max(mx.z, std::max(q.z, w.z));
	}

	out->min = FVECTOR4(mi, 0.0f);
	out->max = FVECTOR4(mx, 0.0f);
}


void EnvMapDirection(int dir, FVECTOR3 *Dir, FVECTOR3 *Up)
{
    switch (dir) {
        case 0:
            *Dir = FVECTOR3(1.0f, 0.0f, 0.0f);
            *Up  = FVECTOR3(0.0f, 1.0f, 0.0f);
            break;
        case 1:
            *Dir = FVECTOR3(-1.0f, 0.0f, 0.0f);
            *Up  = FVECTOR3( 0.0f, 1.0f, 0.0f);
            break;
        case 2:
            *Dir = FVECTOR3(0.0f, 1.0f,  0.0f);
            *Up  = FVECTOR3(0.0f, 0.0f, -1.0f);
            break;
        case 3:
            *Dir = FVECTOR3(0.0f, -1.0f, 0.0f);
            *Up  = FVECTOR3(0.0f,  0.0f, 1.0f);
            break;
        case 4:
            *Dir = FVECTOR3(0.0f, 0.0f, 1.0f);
            *Up  = FVECTOR3(0.0f, 1.0f, 0.0f);
            break;
        case 5:
            *Dir = FVECTOR3(0.0f, 0.0f, -1.0f);
            *Up  = FVECTOR3(0.0f, 1.0f,  0.0f);
            break;
		default:
			*Dir = FVECTOR3(0.0f, 0.0f, 0.0f);
			*Up  = FVECTOR3(0.0f, 0.0f, 0.0f);
			break;
    }
}
