// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// class Instrument_User
// Custom MFD mode

#ifndef __MFD_USER_H
#define __MFD_USER_H

#define STRICT 1
#include "Mfd.h"
#include "OrbiterAPI.h"

class Instrument_User: public Instrument {
public:
	Instrument_User (Pane *_pane, INT_PTR _id, const Spec &spec, Vessel *_vessel,
		int _type, const MFDMODE &mode);
	Instrument_User (Pane *_pane, INT_PTR _id, const Spec &spec, Vessel *_vessel);
	virtual ~Instrument_User();
	int Type() const { return type; }
	char ModeSelKey () const { return selkey; }
	// FIVE FORWARDERS, AND ONLY THE LAST ONE CHECKED `mfd`.
	//
	// That asymmetry is the reference's and it is the tell: BtnMenu's
	// `(mfd ? ... : 0)` says the author knew this pointer can be null, and the
	// four above it were left to dereference it. `mfd` IS null in a state the
	// code deliberately constructs -- the generic user-type constructor sets
	// `mfd = mfd2 = 0` and Instrument::Create reaches it for MFD_USERTYPE --
	// and these four are the keyboard and mouse paths, so the first keypress
	// or button click on such an MFD is a null dereference.
	//
	// The guards are written in BtnMenu's own shape rather than a new one.
	// See the long note in MfdUser.cpp's ReadParams for the measured stack.
	inline bool KeyImmediate (char *kstate) { return (mfd ? mfd->ConsumeKeyImmediate (kstate) : false); }
	inline bool KeyBuffered (DWORD key) { return (mfd ? mfd->ConsumeKeyBuffered (key) : false); }
	inline bool ProcessButton (int bt, int event) { return (mfd ? mfd->ConsumeButton (bt, event) : false); }
	inline const char *BtnLabel (int bt) const { return (mfd ? mfd->ButtonLabel (bt) : 0); }
	inline int BtnMenu (const MFDBUTTONMENU **menu) const { return (mfd ? mfd->ButtonMenu (menu) : 0); }
	void UpdateDraw (oapi::Sketchpad *skp);
	void UpdateDraw (HDC hDC);

protected:
	bool ReadParams (std::ifstream &ifs);
	void WriteParams (std::ostream &ofs) const;

private:
	int type;
	char *name;
	char selkey;
	MFD *mfd; // pointer to module interface
	MFD2 *mfd2; // pointer to version 2 interface (0 if not applicable)
	OAPI_MSGTYPE (*msgproc)(UINT,UINT,WPARAM,LPARAM);
};

#endif // !__MFD_USER_H
