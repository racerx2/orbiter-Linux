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
// CONVERTED FROM OVP/D3D9Client/AABBUtil.h. What changed, and why:
//
//   1. <d3d9.h> AND <d3dx9.h> BECAME VectorHelpers.h. Nothing in this file
//      talks to a device or to Direct3D -- it is bounding boxes, frustum tests
//      and a linear solver -- so the two Windows headers were only ever there
//      for the vector and matrix TYPES. D3DXVECTOR3/4 and D3DXMATRIX become
//      oapi::FVECTOR3/4 and oapi::FMATRIX4, which Orbiter's own SDK supplies.
//
//   2. THE DEVICE PARAMETERS ARE GONE from D9NearPlane and
//      D9ComputeMinMaxDistance. This is not a simplification, it is what
//      reading the bodies end to end showed:
//
//        - D9ComputeMinMaxDistance never mentions pDev at all.
//        - D9NearPlane does `D3DVIEWPORT9 vp; pDev->GetViewport(&vp);` and
//          then never reads vp. The viewport is fetched and dropped.
//
//      There is no IDirect3DDevice9 in this client, so the parameter cannot
//      stay; and since neither function uses the thing it names, nothing is
//      lost by removing it. Every call site drops one argument.
//
//   3. LPD3DXVECTOR3 point BECAME const FVECTOR3 *point. The D3DX typedef is a
//      pointer type; spelling the pointer out is the same declaration. It is
//      const because D9AddPointAABB only reads it.
//
//   4. LPD3DXMATRIX BECAME const FMATRIX4 * in WorldPickRay, for the same
//      reason -- and see the note in the .cpp, because the Windows version of
//      that function has a real defect that the typedef is what hides.
//
// The header guard keeps its name. __D3D9TK_H says nothing about D3D9 that
// matters and renaming guards is churn that shows up in every diff.
// =================================================================================================================================

#include "VectorHelpers.h"

#ifndef __D3D9TK_H
#define __D3D9TK_H

#define SctPwr 1.0
#define SctPwr2 2.0

using oapi::FMATRIX4;

typedef struct {
	FVECTOR4 min, max, bs, a, b, c;
} D9BBox;

float		D9NearPlane(float zmin, float zmax, float dmax, const FMATRIX4 *pProj, bool bReduced);
int			D9ComputeMinMaxDistance(const D9BBox *in, const FMATRIX4 *pWV, const FVECTOR4 *F, float *zmin, float *zmax, float *dmin);
void		D9AddAABB(const D9BBox *in, const FMATRIX4 *pM, D9BBox *out, bool bReset=false);
void		D9UpdateAABB(D9BBox *box, const FMATRIX4 *pFisrt=NULL, const FMATRIX4 *pSecond=NULL);
void		D9ZeroAABB(D9BBox *box);
void		D9InitAABB(D9BBox *box);
void		D9AddPointAABB(D9BBox *box, const FVECTOR3 *point);
bool		D9IsAABBVisible(const D9BBox *in, const FMATRIX4 *pWV, const FVECTOR4 *F);
bool		D9IsBSVisible(const D9BBox *in, const FMATRIX4 *pWV, const FVECTOR4 *F);
FVECTOR4	D9LinearFieldOfView(const FMATRIX4 *pProj);
FVECTOR4	D9OffsetRange(double R, double r);
void		EnvMapDirection(int dir, FVECTOR3 *Dir, FVECTOR3 *Up);
FVECTOR3	WorldPickRay(float x, float y, const FMATRIX4 *mProj, const FMATRIX4 *mView);
bool		SolveLUSystem(int n, double *A, double *b, double *x, double *det=NULL);
#endif
