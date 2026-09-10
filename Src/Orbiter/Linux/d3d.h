// Linux <d3d.h> — see d3dtypes.h for the rationale.
//
// The real SDK header declares the Direct3D 7 COM interfaces on top of the
// types in d3dtypes.h. Utils/meshc includes both but uses only the types, so
// this forwards and adds nothing.

#ifndef ORBITER_LINUX_D3D_H
#define ORBITER_LINUX_D3D_H

#ifdef _WIN32
#error "This header is for non-Windows builds only."
#endif

#include <d3dtypes.h>

// The real d3d.h includes ddraw.h: Direct3D 7 is layered on DirectDraw, and
// its surface and pixel-format types come from there. Src/Orbiter/Texture.h
// relies on that, including only <d3d.h> while using DDSURFACEDESC2 and
// DDPIXELFORMAT. Reproducing the chain here keeps that source unmodified.
#include <ddraw.h>

#endif // ORBITER_LINUX_D3D_H
