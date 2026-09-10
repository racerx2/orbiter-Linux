// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// ======================================================================
// Custom function selection dialog
// ======================================================================

#ifndef __DLGFUNCTION_H
#define __DLGFUNCTION_H

#include "OrbiterAPI.h"

class DlgFunction : public ImGuiDialog {
public:
    DlgFunction();
    void OnDraw() override;

    // THE SCRIPTED UI DRIVER'S WAY IN; see Linux/UiDriver.cpp for why that
    // file exists and the extern "C" wrappers at the foot of DlgFunction.cpp.
    //
    // Members rather than free functions because Orbiter::customcmd and
    // Orbiter::CUSTOMCMD are both private and this class is named in
    // Orbiter.h's `friend class DlgFunction;` -- friendship is granted to the
    // class, not to the file, so a free function beside OnDraw cannot reach
    // the table OnDraw walks.
    static int         CmdCount();
    static const char *CmdLabel(int i);
    static bool        CmdRun(int i);
};
#endif // !__DLGFUNCTION_H