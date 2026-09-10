// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// ==============================================================
//                ORBITER MODULE: DeltaGlider
//                  Part of the ORBITER SDK
//
// RcsSubsys.h
// Reaction control subsystem: lin/rot selection, attitude programs
// ==============================================================

#ifndef __RCSSUBSYS_H
#define __RCSSUBSYS_H

#include "DGSwitches.h"
#include "DGSubsys.h"

// Forward declarations.
//
// RcsSubsys names RcsModeDial as a member pointer before the class itself is
// defined further down this header. MSVC accepts that because a `friend class`
// elsewhere in the tree injects the name into the enclosing namespace -- a
// non-conforming extension. GCC rejects it outright with
//     error: 'RcsModeDial' does not name a type
// so the declarations are made properly here. Additive: nothing else changes.
class RcsModeDial;
class RcsProgButtons;

// ==============================================================
// Reaction control subsystem
// ==============================================================

class RcsModeSelector;
class RcsProgButtons;

class RcsSubsystem: public DGSubsystem {
public:
	RcsSubsystem (DeltaGlider *dg);
	~RcsSubsystem ();

	void SetMode (int mode);
	void SetProg (int prog, bool active);
	bool clbkLoadPanel2D (int panelid, PANELHANDLE hPanel, DWORD viewW, DWORD viewH);
	bool clbkLoadVC (int vcid);

private:
	RcsModeSelector *modeselector;
	RcsProgButtons *progbuttons;
	int ELID_PROGBUTTONS;
};

// ==============================================================
// Control selector dial
// ==============================================================

class RcsModeSelector: public DGSubsystem {
	friend class RcsModeDial;

public:
	RcsModeSelector (RcsSubsystem *_subsys);
	int GetMode () const;
	void SetMode (int mode);
	bool IncMode ();
	bool DecMode ();
	bool clbkLoadPanel2D (int panelid, PANELHANDLE hPanel, DWORD viewW, DWORD viewH);
	bool clbkLoadVC (int vcid);

public:
	RcsModeDial *dial;
	int ELID_DIAL;
};

// ==============================================================
// Mode dial
// ==============================================================

class RcsModeDial: public DGDial1 {
public:
	RcsModeDial (RcsModeSelector *comp);
	void Reset2D (int panelid, MESHHANDLE hMesh);
	void ResetVC (DEVMESHHANDLE hMesh);
	bool Redraw2D (SURFHANDLE surf);
	bool RedrawVC (DEVMESHHANDLE hMesh, SURFHANDLE surf);
	bool ProcessMouse2D (int event, int mx, int my);
	bool ProcessMouseVC (int event, VECTOR3 &p);

private:
	RcsModeSelector *component;
};


// ==============================================================
// RCS program buttons
// ==============================================================

class RcsProgButtons: public PanelElement {
public:
	RcsProgButtons (RcsSubsystem *_subsys);
	~RcsProgButtons ();
	void SetMode (int mode, bool active);
	void DefineAnimationsVC (const VECTOR3 &axis, DWORD meshgrp, DWORD meshgrp_label,
		DWORD vofs[6], DWORD vofs_label[6]);
	void Reset2D (int panelid, MESHHANDLE hMesh);
	void ResetVC (DEVMESHHANDLE hMesh);
	bool Redraw2D (SURFHANDLE surf);
	bool RedrawVC (DEVMESHHANDLE hMesh, SURFHANDLE surf);
	bool ProcessMouse2D (int event, int mx, int my);
	bool ProcessMouseVC (int event, VECTOR3 &p);

private:
	RcsSubsystem *subsys;
	DGButton3 *btn[6]; // the list of navmode buttons
};

#endif // !__RCSSUBSYS_H