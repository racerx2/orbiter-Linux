// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// ======================================================================
// Custom function selection dialog
// ======================================================================

#include "DlgFunction.h"
#include "Orbiter.h"
#include "imgui.h"
#include "IconsFontAwesome6.h"

extern Orbiter *g_pOrbiter;

DlgFunction::DlgFunction() : ImGuiDialog(ICON_FA_PUZZLE_PIECE " Orbiter: Custom functions", {340, 290}) {
	SetHelp("html/orbiter.chm", "/customcmd.htm");
}

void DlgFunction::OnDraw() {
	ImVec2 button_sz(ImVec2(ImGui::GetContentRegionAvail().x, 20));
    for (int i = 0; i < g_pOrbiter->ncustomcmd; i++) {
	    if(ImGui::Button(g_pOrbiter->customcmd[i].label, button_sz)) {
            g_pOrbiter->customcmd[i].func (g_pOrbiter->customcmd[i].context);
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
            ImGui::TextUnformatted(g_pOrbiter->customcmd[i].desc);
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    }
}

// ======================================================================
// Reaching these entries without a mouse.
//
// NO WINDOWS COUNTERPART AND NONE WANTED: on Windows this dialog is a list
// box in a real window, so a test can post an LB_SETCURSEL and a WM_COMMAND
// to it and the entry runs. Here the entries are ImGui buttons drawn straight
// from Orbiter's customcmd table -- they are not controls, they have no ids,
// and they are not in the HWND tree Win32Dlg.cpp maintains, so the scripted
// driver's `click` (which posts WM_COMMAND to a control's owner) has nothing
// to aim at. Neither does anything outside the process: Linux/UiDriver.cpp's
// header records that synthetic pointer events from xdotool never land inside
// the GLFW window on this desktop, and injected presses reach ImGui as hover
// but never as a press.
//
// So the entries were the one part of the program that could not be exercised
// at all, which is exactly where a freeze was found by hand. CmdRun makes the
// same call OnDraw makes above, from the same place in the frame -- the driver
// steps inside orbiter_PumpFrame -- so a module's registered function is
// entered in the identical context a real click gives it, re-entrancy
// included. That is the whole point: a test that dodged the pump would have
// missed the deadlock.
// ======================================================================

int DlgFunction::CmdCount()
{
	return g_pOrbiter ? (int)g_pOrbiter->ncustomcmd : 0;
}

const char *DlgFunction::CmdLabel(int i)
{
	if (i < 0 || i >= CmdCount()) return nullptr;
	return g_pOrbiter->customcmd[i].label;
}

bool DlgFunction::CmdRun(int i)
{
	if (i < 0 || i >= CmdCount()) return false;
	g_pOrbiter->customcmd[i].func(g_pOrbiter->customcmd[i].context);
	return true;
}

extern "C" int         orbiter_CustomCmdCount(void)      { return DlgFunction::CmdCount(); }
extern "C" const char *orbiter_CustomCmdLabel(int i)     { return DlgFunction::CmdLabel(i); }
extern "C" int         orbiter_CustomCmdRun(int i)       { return DlgFunction::CmdRun(i) ? 1 : 0; }
