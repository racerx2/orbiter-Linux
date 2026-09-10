// The Launchpad-phase UI host: window, Vulkan, ImGui, and the frame pump.
//
// WHY THIS EXISTS SEPARATELY FROM THE GRAPHICS CLIENT
//   Orbiter::Create builds the Launchpad before any graphics client is loaded
//   -- the Launchpad is what selects the client. So the Launchpad cannot be
//   drawn by one. It needs its own window and renderer, and when the user
//   presses Launch, clbkCreateRenderWindow creates a *separate* window for the
//   session while the Launchpad hides. Two windows, at different times.
//
// WHERE THE FRAME COMES FROM
//   Orbiter::Run blocks in GetMessage waiting for the user. That is the hook:
//   when Orbiter asks for a message and none is queued, a frame is pumped
//   here. Rendering from inside the message loop is what Win32 effectively
//   does anyway, and it means Orbiter.cpp needs no modification at all.
//
// WHAT IT DRAWS
//   The dialog tree that Win32Dlg.cpp maintains. Each visible dialog becomes
//   an ImGui window; each control becomes the ImGui widget matching its class
//   and style; interaction is fed back as the WM_COMMAND the dialog procedure
//   expects. Owner-drawn controls get a WM_PAINT and their recorded GDI
//   commands are replayed into the frame.

#include <windows.h>
#include <commctrl.h>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_vulkan.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>   // the FontAwesome merge below tests for the .ttf
#include <map>
#include <mutex>
#include <thread>       // the zero-framebuffer skip sleeps instead of spinning
#include <vector>
#include <string>

// ---------------------------------------------------------------------------
// THE DEVICE LOCK -- the counterpart of D3DCREATE_MULTITHREADED.
//
// D3D9Client.cpp creates its device with
//
//     devBehaviorFlags | D3DCREATE_MULTITHREADED | D3DCREATE_FPU_PRESERVE
//
// which tells the D3D9 runtime to take its own lock around every device call.
// That one flag is why the reference can upload a tile texture from
// TileLoader::Load_ThreadProc while the render thread is drawing: the runtime
// serialises them. The reference's only explicit thread guard --
// D3D9Client::clbkGetSketchpad_const's "Sketchpad called from a worker
// thread!" -- is dead code on Windows, because GetCurrentThread() returns the
// same pseudo-handle in every thread, so nothing else restricts what a worker
// may touch.
//
// VULKAN HAS NO SUCH FLAG. VkQueue and VkCommandPool are externally
// synchronised objects: the application must serialise them itself. Without
// it, two threads landed in VulkanDevice::EndOneShot at once -- the render
// thread submitting an ImageProcessing upload from inside the scene callback,
// the tile loader submitting a cloud tile -- and the driver crashed:
//
//   Thread 1   ... EndOneShot:653 (vkQueueSubmit)   -> SIGSEGV in libnvidia-glcore
//   Thread 885 ... EndOneShot:654 (vkQueueWaitIdle) -> blocked in the driver's rwlock
//
// with the validation layers reporting it first:
//
//   UNASSIGNED-Threading-MultipleThreads-Write
//   vkQueueSubmit(): THREADING ERROR : object of type VkQueue is
//   simultaneously used in current thread A and thread B
//
// The lock lives here rather than in the client because the QUEUE is the
// core's -- the client adopts it -- and both sides submit on it: the client
// for its one-shot uploads, the core for the frame and for dialog bitmaps.
// It is recursive because a one-shot can nest inside an offscreen pass, which
// holds one of its own.
//
// It is deliberately taken per OPERATION and not per frame, which is what the
// D3D9 runtime lock did: holding it across the whole of renderFrame would
// stall the tile loader for the entire frame.
// ---------------------------------------------------------------------------
std::recursive_mutex g_deviceMutex;

// Provided by LinuxMain.cpp; the ImGui platform bridge reads it.

// ---------------------------------------------------------------------------
// Vulkan context, shared with the graphics client
//
// The Launchpad already stands up a complete Vulkan device here: instance,
// physical device, logical device, graphics queue, descriptor pool, surface
// and swapchain, plus the ImGui backend bound to them. A graphics client must
// use THAT device rather than create its own -- two devices cannot present to
// one surface, and the client has to render into the same swapchain images
// that ImGui draws its dialogs over.
//
// So the context is published rather than duplicated. Filled by
// orbiter_GetVulkanContext once initialise() has run.
// ---------------------------------------------------------------------------
struct OrbiterVulkanContext {
    void     *instance;        // VkInstance
    void     *physicalDevice;  // VkPhysicalDevice
    void     *device;          // VkDevice
    void     *queue;           // VkQueue
    unsigned  queueFamily;
    void     *descriptorPool;  // VkDescriptorPool
    void     *renderPass;      // VkRenderPass of the main swapchain
    void     *window;          // GLFWwindow*
    unsigned  minImageCount;
    unsigned  imageCount;
};

extern "C" int orbiter_GetVulkanContext(OrbiterVulkanContext *out);

extern "C" void orbiter_SetGLFWWindow(GLFWwindow *win);

// Provided by Win32Dlg.cpp: lets this file walk the live dialog tree without
// exposing the Window type, which is private to that translation unit.
extern "C" {
int   orbiter_EnumTopLevelDialogs(HWND *out, int max);
void  orbiter_DrawImGuiDialogs(void);
int   orbiter_EnumChildren(HWND parent, HWND *out, int max);
void  orbiter_GetControlInfo(HWND h, const char **cls, const char **text,
                             int *id, int *x, int *y, int *cx, int *cy,
                             unsigned *style, int *visible, int *enabled);
int   orbiter_IsDialog(HWND h);
int   orbiter_HasWndProc(HWND h);
int   orbiter_QueryCtlColor(HWND h, unsigned *fg, unsigned *bk,
                            int *opaque);
void  orbiter_SetShiftState(int down);
void  orbiter_SetCtrlState(int down);
HWND  orbiter_ActiveDialog(void);
// Linux/UiDriver.cpp. Inert unless ORBITER_UI_SCRIPT is set.
void  orbiter_UiDriverStep(void);
HWND  orbiter_GetOwner(HWND h);
HWND  orbiter_GetParentWnd(HWND h);
int   orbiter_TreeVisibleCount(HWND h);
int   orbiter_TreeGetRow(HWND h, int index, const char **text, int *depth,
                         int *image, int *selected, int *hasChildren,
                         int *expanded, int *checkState);
void  orbiter_TreeToggleCheck(HWND h, int index);
void  orbiter_TreeClickRow(HWND h, int index, int onExpander, int doubleClick);
void  orbiter_NotifyTreeCustomDraw(HWND ctrl);
unsigned long long orbiter_ImageListTexture(HWND tree, int image,
                                            float *u0, float *v0,
                                            float *u1, float *v1);
unsigned long long orbiter_BitmapTexture(int resId, int *w, int *h);
int   orbiter_GetBitmapId(HWND h);
void  orbiter_GetRange(HWND h, int *lo, int *hi, int *pos);
void  orbiter_SetTrackPos(HWND h, int pos);
void  orbiter_SetSpinPos(HWND h, int pos, int delta);
void  orbiter_ScrollBarNotify(HWND h, int request, int pos);
void  orbiter_GetScrollState(HWND h, int *lo, int *hi, int *pos,
                             int *page);
void  orbiter_NotifyCommand(HWND ctrl, unsigned short notifyCode);
int   orbiter_GetCheckState(HWND h);
void  orbiter_SetCheckState(HWND h, int state);
// Checks this auto-radio and clears its WS_GROUP siblings, as USER32 does.
void  orbiter_CheckRadioButton(HWND h);
int   orbiter_GetItemCount(HWND h);
const char *orbiter_GetItemText(HWND h, int index);
int   orbiter_GetCurSel(HWND h);
int   orbiter_IsMultiSel(HWND h);
int   orbiter_GetItemSel(HWND h, int index);
void  orbiter_ToggleItemSel(HWND h, int index);
void  orbiter_SetCurSel(HWND h, int sel);
void  orbiter_SetText(HWND h, const char *text);
// Typing in an editable combo's edit field: writes the text and clears the
// list selection, which is what a CBS_DROPDOWN does on Windows.
void  orbiter_SetComboEditText(HWND h, const char *text);
void  orbiter_SendPaint(HWND h);

// From Gdi.cpp: the decoded images of an ICON resource, for glfwSetWindowIcon.
int   orbiter_GetWindowIconImages(int resId, int max, int *widths,
                                  int *heights, unsigned char **pixels);
}

// From Gdi.cpp.
extern "C" void orbiter_ReplayDC(HDC hdc, float originX, float originY);
extern "C" void orbiter_ResetDC(HDC hdc);
extern "C" HDC  orbiter_GetPaintDC(HWND h);
// Gdi.cpp: the same lookup without creating one, null when nothing has been
// recorded. Lets the control pass replay GDI drawn through GetDC.
extern "C" HDC  orbiter_FindPaintDC(HWND h);
// Win32Dlg.cpp: a trackbar's TBM_SETTICFREQ, for the TBS_AUTOTICKS renderer.
extern "C" int  orbiter_GetTicFreq(HWND h);

// Defined at the foot of THIS file, declared here because drawControl needs
// it: a VkImageView becomes the VkDescriptorSet ImGui takes as an ImTextureID.
extern "C" unsigned long long orbiter_ImGuiTextureFromView(void *imageView);

// Declared rather than pulled in from OrbiterAPI.h: that header wants the SDK
// include chain, and this file is a Win32 shim compiled ahead of it. The
// signature takes a non-const char* upstream and is matched exactly. Gdi.cpp,
// Win32Dlg.cpp and UiDriver.cpp each declare it the same way.
void oapiWriteLog(char *line);

namespace {

// ---------------------------------------------------------------------------
// Vulkan state
//
// Deliberately minimal: one device, one swapchain, one render pass. ImGui's
// own ImGui_ImplVulkanH_* helpers own the swapchain and per-frame resources,
// which is what the upstream glfw+vulkan example does and avoids
// reimplementing several hundred lines of resize and present handling here.
// ---------------------------------------------------------------------------

VkAllocationCallbacks   *g_allocator      = nullptr;
VkInstance               g_instance       = VK_NULL_HANDLE;
VkPhysicalDevice         g_physicalDevice = VK_NULL_HANDLE;
VkDevice                 g_device         = VK_NULL_HANDLE;
uint32_t                 g_queueFamily    = (uint32_t)-1;
VkQueue                  g_queue          = VK_NULL_HANDLE;
VkDescriptorPool         g_descriptorPool = VK_NULL_HANDLE;

ImGui_ImplVulkanH_Window g_mainWindowData;
int                      g_minImageCount  = 2;
bool                     g_swapChainRebuild = false;

// ---------------------------------------------------------------------------
// A FRAME THAT IS NOT PRESENTED IS A FRAME THAT DOES NOT BLOCK.
//
// vkAcquireNextImageKHR is called with UINT64_MAX and is the ONE blocking call
// in the frame. It is what makes the simulation run at display rate instead of
// at CPU rate. Every path that returns from orbiter_PumpFrame before reaching
// it therefore does two things, not one: it drops a picture, AND it removes
// the only thing pacing Orbiter::Run.
//
// There are five such paths, and until now every one of them was SILENT:
//
//   1. the framebuffer is 0x0
//   2. nothing is visible -- no session, no splash, no dialog
//   3. ImGui's DisplaySize is 0
//   4. vkAcquireNextImageKHR answered OUT_OF_DATE
//   5. renderFrame returned without submitting, so presentFrame has nothing
//
// A user reported a freeze on 2026-09-09. The core showed 8,629,484 frames in
// 231 s -- 56,662 fps, one core pinned -- with bVisible, bSession, bActive and
// bRunning all true and every GPU thread parked. The application was not hung
// at all: it was running 900x too fast and presenting nothing, which from the
// outside is indistinguishable from a deadlock. WHICH of the five it was could
// not be determined, because none of them said anything.
//
// So they all report now, once on entry and once on recovery with a duration,
// and the next occurrence names itself. This is deliberately NOT a fix for the
// freeze -- the cause is still unknown; see the porting notes.
// It is the instrument that will identify it.
//
// The reason is compared BY POINTER, not by strcmp: every caller passes a
// string literal, so pointer identity is "the same reason as last frame" and
// costs nothing per frame.
// ---------------------------------------------------------------------------
const char                           *g_skipReason = nullptr;
std::chrono::steady_clock::time_point g_skipSince;

void noteFramePresented();

void noteFrameSkipped(const char *why)
{
    if (g_skipReason == why) return;      // already reported, still true
    if (g_skipReason) noteFramePresented();  // a DIFFERENT reason: close the old
    g_skipReason = why;
    g_skipSince  = std::chrono::steady_clock::now();
    char m[320];
    snprintf(m, sizeof m,
             "UIHost: not presenting -- %s. The simulation is no longer paced "
             "by the swapchain and the window will look frozen until this "
             "clears.", why);
    oapiWriteLog(m);
}

void noteFramePresented()
{
    if (!g_skipReason) return;
    const double ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - g_skipSince).count();
    char m[320];
    snprintf(m, sizeof m, "UIHost: presenting again after %.0f ms (%s).",
             ms, g_skipReason);
    oapiWriteLog(m);
    g_skipReason = nullptr;
}

// ---------------------------------------------------------------------------
// Video-tab state
//
// "Disable vertical sync" and the Full Screen / Window choice are settings the
// Launchpad's Video tab owns, but the window and the swapchain are owned here,
// so this is where they have to land. Both are read by setupSurface and by the
// rebuild in renderFrame; changing either sets g_swapChainRebuild so the next
// frame is presented with the new configuration rather than the old.
// ---------------------------------------------------------------------------

bool                     g_noVSync = false;

// Can this platform report or set a window's position?
//
// WAYLAND CANNOT, BY DESIGN. A client is not told where its window is and
// cannot move it -- placement is the compositor's alone. glfwGetWindowPos and
// glfwSetWindowPos therefore fail with
//     GLFW error 65548: Wayland: The platform does not provide the window
//                       position
// and calling them anyway logs an error per call for something that will never
// work. Everything that uses a position here is either a nicety (restoring
// where a window was before fullscreen) or already the compositor's job, so
// the calls are simply skipped.
bool platformHasWindowPos()
{
    return glfwGetPlatform() != GLFW_PLATFORM_WAYLAND;
}

// THE PRIMARY MONITOR, WHICH glfwGetPrimaryMonitor DOES NOT RELIABLY GIVE.
//
// Everything the reference does with a display goes through a system that
// knows which one is primary. CD3DFramework9::Initialize's fullscreen styles
// are GetSystemMetrics(SM_CXSCREEN) and SM_CYSCREEN; the mode list is
// EnumAdapterModes(Adapter, ...); GraphicsClient::clbkCreateRenderWindow's
// CW_USEDEFAULT lets Windows place the window, and Windows places it on the
// primary. Win32 has a primary display and always has.
//
// WAYLAND DOES NOT. There is no primary output in the core protocol, so GLFW's
// Wayland backend has nothing to answer with and returns the FIRST monitor it
// enumerated. MEASURED on this desktop, three monitors:
//
//     monitor 0 (glfwGetPrimaryMonitor) "HDMI-A-1" 1920x1080 at 0,0
//     monitor 1                         "DP-1"     2560x1440 at 1920,0
//     monitor 2                         "HDMI-A-2" 1920x1080 at 4480,0
//
// and KDE's own ~/.config/kwinoutputconfig.json gives DP-1 "priority": 1,
// which is KDE's primary. X11 agrees with KDE -- xrandr marks 2560x1440+1920+0
// primary -- so this is specifically a Wayland gap, not a misconfigured
// desktop, and it is invisible until there is more than one monitor.
//
// What it cost: buildVideoModeList enumerates this monitor's modes, so the
// Video tab offered HDMI-A-1's resolutions and could not offer 2560x1440 at
// all; and both fullscreen styles attach the window to this monitor, so
// "Full Screen" on a machine whose main display is 2560x1440 went fullscreen
// on a 1920x1080 one.
//
// SDL HAS THE SAME PROBLEM AND ITS ANSWER IS THE ONE COPIED HERE. From
// SDL's own docs/README-wayland.md:
//
//     "Wayland doesn't natively have the concept of a primary display, so SDL
//      attempts to determine it by querying various system settings, and
//      falling back to a selection algorithm if this fails."
//
// with SDL_HINT_VIDEO_DISPLAY_PRIORITY as the user's override -- "a comma
// separated list containing the names of the displays that SDL should sort to
// the front of the display list", named by connector ("DP-1", "HDMI-A-1").
// So: an explicit override first, a selection algorithm second. ORBITER_DISPLAY
// is that override and glfwGetMonitorName returns exactly those connector
// names -- MEASURED on this desktop, "HDMI-A-1", "DP-1", "HDMI-A-2".
//
// THE ONE SOURCE DELIBERATELY NOT USED is KDE's own kde-primary-output-v1,
// which exists and would answer exactly this question. Its specification
// refuses the use: "The protocol described in this file is a desktop
// environment implementation detail. Regular clients must not use this
// protocol." Reading ~/.config/kwinoutputconfig.json directly is the same
// trespass with none of the protocol's guarantees.
//
// THE SELECTION ALGORITHM IS A HEURISTIC AND IS LABELLED AS ONE. Largest pixel
// area wins. It is not what KDE means by primary and cannot be, but it is
// strictly better than "index 0", which carries no meaning whatsoever.
//
// The principled fix is the reference's own: D3D9's Adapter is an OUTPUT, and
// `vData->deviceidx` selects it from the Video tab -- CD3DFramework9::Initialize
// does `Adapter = vData->deviceidx` and enumerates that adapter's modes. This
// port lost the mapping because Vulkan enumerates GPUs where D3D9 enumerated
// adapter/output pairs, so the device combo lists one physical device and
// cannot express "which monitor". Restoring it is a Video tab change and is not
// made here.
//
// X11 keeps glfwGetPrimaryMonitor, which there reads RandR's real primary and
// agrees with the desktop -- confirmed on this machine, where X11 and KDE both
// name DP-1 and only Wayland disagrees.
// ORBITER'S OWN FIELD FOR THIS, and it was already plumbed end to end.
//
// VIDEODATA carries `int outputidx; ///< video output` (graphicsapi.h:840);
// GraphicsClient::clbkRefreshVideoData fills it from CfgDevPrm.Device_out
// (GraphicsAPI.cpp:116), which is the `OutputIndex` line already sitting in
// every Orbiter.cfg; and TabVideo.cpp:126 writes it back. The whole channel
// exists and nothing on this platform was reading it.
//
// The REFERENCE never reads it either -- neither D3D9Client.cpp, D3D9Frame.cpp
// nor its VideoTab.cpp mentions outputidx -- and that is not an oversight. A
// D3D9 ADAPTER IS AN OUTPUT, so `Adapter = vData->deviceidx` already selects
// the display and a second index would be redundant. Vulkan enumerates GPUs,
// not adapter/output pairs, so deviceidx cannot carry it here and outputidx is
// the field Orbiter provides for exactly the question that leaves open.
//
// ZERO MEANS AUTOMATIC, and that is the one semantic choice made here.
// GraphicsAPI.cpp:64 defaults outputidx to 0 with no "unset" sentinel (unlike
// deviceidx, which VideoTab treats -1 as fresh), and no Video tab on this
// platform offers an output combo yet, so every existing Orbiter.cfg says 0.
// Reading that as "monitor 0" would hard-code the very wrong answer this
// function exists to avoid. A combo on the Video tab is the finish; until then
// 1..n select monitors 0..n-1 and 0 means "work it out".
int g_outputIndex = 0;

GLFWmonitor *pickPrimaryMonitor()
{
    GLFWmonitor *fallback = glfwGetPrimaryMonitor();

    int count = 0;
    GLFWmonitor **all = glfwGetMonitors(&count);
    if (!all || count <= 0) return fallback;

    // ORBITER_DISPLAY names a connector and outranks everything, on EVERY
    // platform. It is the counterpart of SDL_HINT_VIDEO_DISPLAY_PRIORITY and
    // exists for the same reason: to settle an argument about which display is
    // primary without editing a config file.
    if (const char *want = getenv("ORBITER_DISPLAY")) {
        for (int i = 0; i < count; ++i) {
            const char *nm = glfwGetMonitorName(all[i]);
            if (nm && strcmp(nm, want) == 0) {
                static bool said = false;
                if (!said) {
                    said = true;
                    fprintf(stderr, "Orbiter: ORBITER_DISPLAY=%s -- using that "
                                    "monitor.\n", want);
                }
                return all[i];
            }
        }
        static bool warned = false;
        if (!warned) {
            warned = true;
            fprintf(stderr, "Orbiter: ORBITER_DISPLAY=%s names no connected "
                            "monitor. Connected:", want);
            for (int i = 0; i < count; ++i)
                fprintf(stderr, " %s",
                        glfwGetMonitorName(all[i]) ? glfwGetMonitorName(all[i]) : "?");
            fputc('\n', stderr);
        }
    }

    // Then Orbiter's own OutputIndex. 1..n select monitors 0..n-1; 0 is
    // "automatic" and falls through -- see the note above this function.
    if (g_outputIndex > 0 && g_outputIndex <= count) {
        static bool said = false;
        if (!said) {
            said = true;
            const char *nm = glfwGetMonitorName(all[g_outputIndex - 1]);
            fprintf(stderr, "Orbiter: OutputIndex = %d -- using monitor "
                            "\"%s\".\n", g_outputIndex, nm ? nm : "?");
        }
        return all[g_outputIndex - 1];
    }
    if (g_outputIndex > count) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            fprintf(stderr, "Orbiter: OutputIndex = %d but only %d monitor(s) "
                            "are connected; choosing automatically.\n",
                    g_outputIndex, count);
        }
    }

    if (glfwGetPlatform() != GLFW_PLATFORM_WAYLAND || count <= 1)
        return fallback;

    GLFWmonitor *best = fallback;
    long long bestArea = -1;
    for (int i = 0; i < count; ++i) {
        const GLFWvidmode *vm = glfwGetVideoMode(all[i]);
        if (!vm) continue;
        const long long area = (long long)vm->width * vm->height;
        if (area > bestArea) { bestArea = area; best = all[i]; }
    }

    if (best != fallback) {
        static bool said = false;
        if (!said) {
            said = true;
            fprintf(stderr,
                    "Orbiter: Wayland has no primary-output concept, so GLFW "
                    "named the first monitor.\n"
                    "         Using \"%s\" (the largest) instead. Set "
                    "ORBITER_DISPLAY=<connector> to choose,\n"
                    "         e.g. ORBITER_DISPLAY=DP-1.\n",
                    glfwGetMonitorName(best) ? glfwGetMonitorName(best) : "?");
        }
    }
    return best;
}

// ---------------------------------------------------------------------------
// ROTATION MODE: the cursor hidden and confined, which is what Windows does.
// ---------------------------------------------------------------------------
//
// Orbiter::InitRotationMode is explicit about it, and its own comment gives
// the reason:
//
//     if (g_iCursorShowCount == 0) g_iCursorShowCount = ShowCursor(FALSE);
//     SetCapture (hRenderWnd);
//     // Limit cursor to render window confines, so we don't miss the
//     // button up event
//     ClipCursor (&rScreen);
//
// and ExitRotationMode undoes all three. Every one of those was a no-op stub
// in the Linux shim, so on this platform the pointer stayed visible and was
// free to wander off the window mid-drag -- exactly the lost button-up the
// reference is guarding against.
//
// GLFW spells hidden-and-confined as GLFW_CURSOR_DISABLED, which also gives
// unbounded virtual motion, so it covers ShowCursor(FALSE), ClipCursor() and
// the confinement half of SetCapture in one call. The two Win32 concepts are
// kept separate here because the shim exposes them separately; they are
// combined into a mode at the point of use.
bool g_cursorVisible  = true;
bool g_cursorConfined = false;
void applyCursorMode();   // defined below, once g_window exists

// The EMULATED warp, and it is now the ONLY warp.
//
// SetCursorPos is not decoration in Camera::UpdateMouse -- it is half of the
// delta calculation:
//
//     dx = pt.x - mx;                 // pt from GetCursorPos
//     SetCursorPos (x0-dx, y0-dy);    // put it back, so the next read is new
//
// Without the warp, pt keeps advancing while mx lags, dx measures the distance
// from where the drag BEGAN rather than movement since the last frame, and the
// camera accelerates the further the hand travels.
//
// glfwSetCursorPos IS NOT USED, on either platform. Measured, not assumed:
// under GLFW_CURSOR_DISABLED -- the mode rotation mode now runs in -- it is
// silently ignored on Wayland (120 calls, 0 errors, position unchanged), and
// outside that mode it is the call that produced
//     GLFW error 65548: Wayland: The platform does not support setting the
//                       cursor position
// once per frame for the length of every drag. Since the reported position is
// this port's own concept anyway, moving the OFFSET rather than the pointer
// works identically in both modes and on both platforms -- one mechanism, no
// capability branch, and nothing that can fail.
//
// SCOPED TO A DRAG, and that is not optional. GetCursorPos is also read by
// Panel2D::GetMouseState, VCockpit, Panel and Defpanel to track a finger held
// on a switch, and those want the REAL pointer. A bias left standing after a
// drag would put every panel click at the wrong place. postMouseMessages
// clears it whenever no button is down, which is exactly the interval Windows
// would have the pointer pinned for.
double g_cursorBiasX = 0.0, g_cursorBiasY = 0.0;

// The windowed geometry, remembered across a trip through fullscreen.
//
// glfwSetWindowMonitor takes the position and size to restore when leaving
// fullscreen, and GLFW keeps none of its own: asking the window for its size
// AFTER it has gone fullscreen returns the monitor's, so the original must be
// captured before the switch or the user never gets their window back.
int                      g_savedWndX = 0, g_savedWndY = 0;
int                      g_savedWndW = 0, g_savedWndH = 0;
bool                     g_savedWndValid = false;

// The display style currently in force: 0 true fullscreen, 1 fullscreen
// window, 2 window with decorations. Matches CFG_DEVPRM::Device_style and
// GraphicsClient::VIDEODATA::style, so no translation is needed anywhere.
int                      g_displayStyle = 2;

// A frame has been submitted and MUST still be presented.
//
// renderFrame and presentFrame are two halves of one operation, and the thing
// that used to break them apart was g_swapChainRebuild: renderFrame sets it on
// VK_SUBOPTIMAL_KHR and then goes on to record and submit anyway, while
// presentFrame's first line was `if (g_swapChainRebuild) return;`. So a
// suboptimal frame was built, submitted, and never presented -- which leaves
// its render-complete semaphore signalled with nothing to wait on it, and an
// acquired swapchain image never handed back.
//
// The rule this flag enforces: WHAT WAS SUBMITTED IS PRESENTED. A rebuild is
// something that happens to the NEXT frame, not something that abandons this
// one halfway through.
bool                     g_framePending = false;

GLFWwindow              *g_window = nullptr;

// Declared above, beside the two flags it reads. See the note there.
void applyCursorMode()
{
    if (!g_window) return;
    // Hidden AND confined is DISABLED; hidden alone is HIDDEN. Confinement
    // without hiding has no GLFW spelling, and nothing in this tree asks for
    // it -- ClipCursor is only ever called alongside ShowCursor(FALSE).
    const int mode = (!g_cursorVisible && g_cursorConfined) ? GLFW_CURSOR_DISABLED
                   : (!g_cursorVisible)                     ? GLFW_CURSOR_HIDDEN
                                                            : GLFW_CURSOR_NORMAL;
    static int applied = GLFW_CURSOR_NORMAL;
    if (mode == applied) return;   // re-setting it per frame would fight ImGui
    applied = mode;
    glfwSetInputMode(g_window, GLFW_CURSOR, mode);

    if (getenv("ORBITER_TRACE_MSG"))
        fprintf(stderr, "[cursor] mode -> %s\n",
                mode == GLFW_CURSOR_DISABLED ? "DISABLED (hidden+confined)" :
                mode == GLFW_CURSOR_HIDDEN   ? "HIDDEN" : "NORMAL");
}
// True between clbkRenderScene and clbkDisplayFrame -- a PER-FRAME flag,
// used only to pick the clear colour.
bool                     g_inSceneFrame = false;

// True for the LIFETIME of a session with a graphics client attached.
//
// These must not be confused. Window visibility is a session-lifetime
// property; driving it from the per-frame flag showed and hid the window on
// every single frame, which on a fast GPU is hundreds of times a second. It
// flashed violently and stole focus each time it reappeared, to the point of
// making the desktop hard to use.
bool                     g_sessionActive = false;

// ---------------------------------------------------------------------------
// THE SPLASH SCREEN, and why any of it is here rather than in the client.
// ---------------------------------------------------------------------------
//
// D3D9Client::SplashScreen() composes the whole thing itself and puts it on
// screen itself:
//
//     CreateOffscreenPlainSurface(viewW, viewH, ...)   -> pSplashScreen
//     ColorFill(pSplashScreen, black)
//     D3DXLoadSurfaceFromFileInMemory(pSplashScreen, ..., &imgRect, ...)
//     pSplashScreen->GetDC(&hDC);  TextOut(...) x3;  ReleaseDC
//     StretchRect(pSplashScreen, NULL, pBackBuffer, NULL, ...)
//     pDevice->Present(0, 0, 0, 0)
//
// and OutputLoadStatus repeats the last three every time the core reports a
// load step. TWO of those steps cannot be done by a client on this platform,
// and they are the reason the splash lives here:
//
//   1. IT PRESENTS. Nothing else in this port does -- renderFrame owns the
//      only command buffer that reaches the swapchain and presentFrame is the
//      only vkQueuePresentKHR. A client that presented as well would acquire
//      and present a second image per frame. So the "put it on screen" half
//      has to be a pump from here, exactly as the scene hook is.
//
//   2. IT RASTERISES TEXT WITH GDI. Linux/Gdi.cpp is a display-list RECORDER,
//      not a rasteriser: TextOut appends a DrawCmd and orbiter_ReplayDC turns
//      it into ImGui draw commands. There are no glyph pixels for a client to
//      blit into a surface. The equivalent is the ImGui font this file already
//      owns.
//
// Everything ABOVE those two lines is still the client's: which image, scaled
// how, where the rectangles go, what the strings say and in what colour. It
// hands the result over through orbiter_SetSplash*, in the coordinates the
// reference computes, and this draws it. See VulkanClient::SplashScreen().
//
// Drawn into the BACKGROUND draw list, which is behind every ImGui window --
// the counterpart of blitting to the back buffer before the dialogs are
// composited over it.
struct SplashState {
    bool active = false;

    // The image, uploaded once, at the size and position the client computed.
    // The handles are kept so they can be destroyed: a session can be started
    // and closed repeatedly, and this is a ten-megabyte image.
    VkImage         image  = VK_NULL_HANDLE;
    VkDeviceMemory  memory = VK_NULL_HANDLE;
    VkImageView     view   = VK_NULL_HANDLE;
    VkDescriptorSet set    = VK_NULL_HANDLE;
    int imgW = 0, imgH = 0, imgX = 0, imgY = 0;

    // The three version lines, written once by SplashScreen().
    std::string info[3];
    int infoX = 0, infoY = 0, infoSpacing = 20, infoSize = 18;

    // The load-status panel, rewritten on every clbkSplashLoadMsg.
    bool        hasStatus = false;
    std::string label, item;
    int statusX = 0, statusY = 0, statusW = 0;
    int labelSize = 24, itemSize = 18;

    // COLORREF, 0x00BBGGRR -- the reference hands this straight to
    // SetTextColor and CreatePen, so it is in Windows' order, not ImGui's.
    ImU32 colour = IM_COL32(0xA0, 0xA0, 0xE0, 255);
};

SplashState g_splash;

// Courier New's stand-in, loaded once in initialise().
//
// The reference asks for "Courier New" at weight 700. Liberation Mono is the
// metric-compatible substitute for Courier New and has a real bold face, so
// the line lengths match what a Windows user sees. Wine's courier.ttf is the
// fallback, and ImGui's built-in font the last resort -- the splash is still
// legible with either.
ImFont                  *g_splashFont = nullptr;

// The device context of the client's main render surface.
//
// Orbiter draws the HUD and panel instruments into that surface, and the MFDs
// into their own which the client blits onto it. Only the client knows which
// surface it is, so it publishes the DC here and the frame pump replays it.
HDC                      g_sceneDC = nullptr;
bool                     g_initialised = false;
bool                     g_initFailed  = false;

// ---------------------------------------------------------------------------
// The depth attachment.
//
// ImGui does not need depth, so ImGui_ImplVulkanH_CreateOrResizeWindow builds
// a render pass with ONE colour attachment and framebuffers to match. That is
// enough for the Launchpad, for dialogs, and for the sky -- every technique in
// Planet.fx, HorizonHaze.fx and BeaconArray.fx has Z test and Z write off.
//
// It is NOT enough for a vessel. All five VesselTech passes need depth test
// AND depth write, as do SimplifiedTech, AxisTech, GeometryTech and the
// bounding volumes. Without a depth attachment those pipelines cannot even be
// created, so the graphics client could draw a sky and never a spacecraft.
//
// So after ImGui's helper builds the swapchain, attachDepth() replaces the
// render pass with a colour+depth one and rebuilds the framebuffers. ImGui's
// own pipeline is created against whatever wd->RenderPass holds at
// ImGui_ImplVulkan_Init time, so the swap must happen BEFORE that call -- and
// it does, because createSwapchain() runs first.
//
// On resize the helper recreates its own render pass and framebuffers, so
// attachDepth() must run again. That is safe for ImGui's already-built
// pipeline: two render passes are COMPATIBLE when their attachments match in
// format and sample count, which successive calls here guarantee.
// ---------------------------------------------------------------------------
VkFormat                 g_depthFormat = VK_FORMAT_UNDEFINED;
VkImage                  g_depthImage  = VK_NULL_HANDLE;
VkDeviceMemory           g_depthMemory = VK_NULL_HANDLE;
VkImageView              g_depthView   = VK_NULL_HANDLE;

// The colour+depth render pass, OWNED HERE AND KEPT ACROSS RESIZES.
//
// A VkRenderPass describes formats, sample counts and load/store ops. It does
// NOT describe an extent -- that lives in the framebuffer -- so a resize is no
// reason to make a new one, and there is a strong reason not to:
// orbiter_GetVulkanContext publishes this handle to the graphics client,
// VulkanDevice stores it in vkRenderPass and exposes GetRenderPass(), and
// every pipeline the client builds must be created against it. Rebuilding it
// on resize left the client holding a destroyed handle.
//
// The two formats are recorded so the pass can be rebuilt in the one case that
// genuinely invalidates it: the surface or depth format changing under a
// swapchain rebuild.
VkRenderPass             g_scenePass       = VK_NULL_HANDLE;
VkFormat                 g_scenePassColour = VK_FORMAT_UNDEFINED;
VkFormat                 g_scenePassDepth  = VK_FORMAT_UNDEFINED;

// ---------------------------------------------------------------------------
// GPU crash reporting. See the note above createLogicalDevice for why the
// extensions are enabled there; this is the part that reads them back.
//
// VK_ERROR_DEVICE_LOST is reported by whichever call runs after the reset, not
// by the one that caused it, so on its own it names nothing. These two
// routines turn it into a location:
//
//   orbiter_VkCheckpoint  records a marker into a command buffer. The tag must
//       be a STRING LITERAL or otherwise outlive the submission -- the driver
//       stores the pointer, not the text, and hands it back verbatim after the
//       loss. Gated on ORBITER_VK_CHECKPOINTS so the markers cost nothing in a
//       normal run.
//   orbiter_DumpGpuCheckpoints  asks the queue which markers the GPU reached.
//       Markers at TOP_OF_PIPE were fetched but not finished; the last one at
//       BOTTOM_OF_PIPE is the last command that COMPLETED, so the offender is
//       between the two.
//
// Both are exported so the graphics client can use them: the client records
// the scene into the command buffer this host owns, so its draws and this
// host's ImGui pass share one stream and one queue. The exported wrappers are
// at the bottom of the file -- everything in this file lives in an anonymous
// namespace, which gives internal linkage, and a symbol with internal linkage
// is not exported from the executable no matter what its language linkage
// says. The same pattern the other orbiter_* entry points already use.
// ---------------------------------------------------------------------------
extern bool g_hasCheckpoints;
extern bool g_hasDeviceFault;

// Set by createVulkanInstance; read by createLogicalDevice, which runs after
// it. Both diagnostic device extensions list it as a dependency.
bool g_hasPhysDevProps2 = false;

bool g_wantCheckpoints = (getenv("ORBITER_VK_CHECKPOINTS") != nullptr);

void vkCheckpoint(void *cmdBuf, const char *tag)
{
    if (!g_wantCheckpoints || !g_hasCheckpoints || !cmdBuf) return;
    static PFN_vkCmdSetCheckpointNV fn =
        (PFN_vkCmdSetCheckpointNV)vkGetDeviceProcAddr(g_device, "vkCmdSetCheckpointNV");
    if (fn) fn((VkCommandBuffer)cmdBuf, (const void *)tag);
}

void dumpGpuCheckpoints(const char *where)
{
    if (g_hasCheckpoints && g_queue) {
        static PFN_vkGetQueueCheckpointDataNV fn =
            (PFN_vkGetQueueCheckpointDataNV)vkGetDeviceProcAddr(
                g_device, "vkGetQueueCheckpointDataNV");
        if (fn) {
            uint32_t n = 0;
            fn(g_queue, &n, nullptr);
            if (n) {
                std::vector<VkCheckpointDataNV> data(n);
                for (auto &d : data) { d = {}; d.sType = VK_STRUCTURE_TYPE_CHECKPOINT_DATA_NV; }
                fn(g_queue, &n, data.data());
                fprintf(stderr, "Orbiter: ---- GPU checkpoints at %s (%u) ----\n",
                        where, n);
                for (uint32_t i = 0; i < n; i++) {
                    const char *stage =
                        (data[i].stage & VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT) ? "COMPLETED" :
                        (data[i].stage & VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT)    ? "started  " :
                                                                                 "in-flight";
                    fprintf(stderr, "Orbiter:   [%s] %s\n", stage,
                            data[i].pCheckpointMarker
                                ? (const char *)data[i].pCheckpointMarker : "(null)");
                }
            } else {
                fprintf(stderr, "Orbiter: no GPU checkpoints recorded at %s "
                                "(set ORBITER_VK_CHECKPOINTS=1 to enable them)\n",
                        where);
            }
        }
    }

    if (g_hasDeviceFault && g_device) {
        static PFN_vkGetDeviceFaultInfoEXT fn =
            (PFN_vkGetDeviceFaultInfoEXT)vkGetDeviceProcAddr(
                g_device, "vkGetDeviceFaultInfoEXT");
        if (fn) {
            VkDeviceFaultCountsEXT counts{};
            counts.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT;
            if (fn(g_device, &counts, nullptr) == VK_SUCCESS) {
                std::vector<VkDeviceFaultAddressInfoEXT> addrs(counts.addressInfoCount);
                std::vector<VkDeviceFaultVendorInfoEXT>  vend(counts.vendorInfoCount);
                VkDeviceFaultInfoEXT info{};
                info.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT;
                info.pAddressInfos = addrs.empty() ? nullptr : addrs.data();
                info.pVendorInfos  = vend.empty()  ? nullptr : vend.data();
                // The vendor BINARY is not asked for: leaving vendorBinarySize
                // zero tells the driver not to produce one, and there is
                // nothing here that could parse it anyway.
                info.pVendorBinaryData  = nullptr;
                counts.vendorBinarySize = 0;
                if (fn(g_device, &counts, &info) == VK_SUCCESS) {
                    fprintf(stderr, "Orbiter: ---- device fault: %s ----\n",
                            info.description);
                    for (uint32_t i = 0; i < counts.addressInfoCount; i++)
                        fprintf(stderr, "Orbiter:   addr type %u at 0x%llx "
                                        "(precision 0x%llx)\n",
                                (unsigned)addrs[i].addressType,
                                (unsigned long long)addrs[i].reportedAddress,
                                (unsigned long long)addrs[i].addressPrecision);
                    for (uint32_t i = 0; i < counts.vendorInfoCount; i++)
                        fprintf(stderr, "Orbiter:   vendor: %s (code %llu, data %llu)\n",
                                vend[i].description,
                                (unsigned long long)vend[i].vendorFaultCode,
                                (unsigned long long)vend[i].vendorFaultData);
                }
            }
        }
    }
}

void check(VkResult err, const char *what)
{
    if (err == VK_SUCCESS) return;
    fprintf(stderr, "Orbiter: Vulkan error in %s: %d\n", what, (int)err);
    if (err == VK_ERROR_DEVICE_LOST) dumpGpuCheckpoints(what);
    if (err < 0) g_initFailed = true;
}

// ---------------------------------------------------------------------------
// Depth attachment support. See the note by g_depthView for why this exists.
// ---------------------------------------------------------------------------

uint32_t findMemoryTypeIdx(uint32_t typeBits, VkMemoryPropertyFlags want)
{
    VkPhysicalDeviceMemoryProperties props;
    vkGetPhysicalDeviceMemoryProperties(g_physicalDevice, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; i++)
        if ((typeBits & (1u << i)) &&
            (props.memoryTypes[i].propertyFlags & want) == want)
            return i;
    return UINT32_MAX;
}

VkFormat selectDepthFormat()
{
    // D24_UNORM_S8_UINT first, matching VulkanDevice::SelectDepthFormat in the
    // graphics client so both agree on what the buffer is; D32_SFLOAT_S8_UINT
    // is the fallback that AMD and some mobile parts prefer. Stencil is
    // included because Mesh.fx's ShadowTech needs it -- its whole correctness
    // rests on a stencil test, and a depth-only format would rule that
    // technique out later for no saving now.
    const VkFormat candidates[] = { VK_FORMAT_D24_UNORM_S8_UINT,
                                    VK_FORMAT_D32_SFLOAT_S8_UINT,
                                    VK_FORMAT_D16_UNORM_S8_UINT };
    for (VkFormat f : candidates) {
        VkFormatProperties p;
        vkGetPhysicalDeviceFormatProperties(g_physicalDevice, f, &p);
        if (p.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
            return f;
    }
    return VK_FORMAT_UNDEFINED;
}

void destroyDepthResources()
{
    if (g_depthView)   { vkDestroyImageView(g_device, g_depthView, g_allocator); g_depthView  = VK_NULL_HANDLE; }
    if (g_depthImage)  { vkDestroyImage(g_device, g_depthImage, g_allocator);    g_depthImage = VK_NULL_HANDLE; }
    if (g_depthMemory) { vkFreeMemory(g_device, g_depthMemory, g_allocator);     g_depthMemory = VK_NULL_HANDLE; }
}

bool attachDepth(ImGui_ImplVulkanH_Window *wd, uint32_t width, uint32_t height)
{
    if (!wd || width == 0 || height == 0) return false;

    // Everything in flight references the framebuffers and render pass about
    // to be destroyed. ImGui's own resize path waits the device idle before
    // calling CreateOrResizeWindow, but this runs on the resize path too and
    // must not assume that.
    vkDeviceWaitIdle(g_device);
    destroyDepthResources();

    g_depthFormat = selectDepthFormat();
    if (g_depthFormat == VK_FORMAT_UNDEFINED) return false;

    // --- the depth image -------------------------------------------------
    //
    // ONE image shared by every swapchain frame, not one per frame. That is
    // safe only because the depth buffer carries nothing between frames: the
    // render pass clears it at load and nothing reads it afterwards. Colour
    // needs one per frame because the presentation engine may still be
    // scanning out the previous one; depth is never presented.
    VkImageCreateInfo ici{};
    ici.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType     = VK_IMAGE_TYPE_2D;
    ici.format        = g_depthFormat;
    ici.extent        = { width, height, 1 };
    ici.mipLevels     = 1;
    ici.arrayLayers   = 1;
    ici.samples       = VK_SAMPLE_COUNT_1_BIT;   // must match the colour attachment
    ici.tiling        = VK_IMAGE_TILING_OPTIMAL;
    ici.usage         = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    ici.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(g_device, &ici, g_allocator, &g_depthImage) != VK_SUCCESS)
        return false;

    VkMemoryRequirements mr{};
    vkGetImageMemoryRequirements(g_device, g_depthImage, &mr);
    uint32_t typeIdx = findMemoryTypeIdx(mr.memoryTypeBits,
                                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (typeIdx == UINT32_MAX) { destroyDepthResources(); return false; }

    VkMemoryAllocateInfo mai{};
    mai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize  = mr.size;
    mai.memoryTypeIndex = typeIdx;
    if (vkAllocateMemory(g_device, &mai, g_allocator, &g_depthMemory) != VK_SUCCESS) {
        destroyDepthResources(); return false;
    }
    vkBindImageMemory(g_device, g_depthImage, g_depthMemory, 0);

    VkImageViewCreateInfo ivci{};
    ivci.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    ivci.image    = g_depthImage;
    ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivci.format   = g_depthFormat;
    // DEPTH only in the view's aspect mask, even though the format carries
    // stencil: a depth/stencil attachment view must select one aspect or both,
    // and VK_IMAGE_ASPECT_DEPTH_BIT alone is what a depth-stencil ATTACHMENT
    // wants. Adding the stencil bit here is legal for an attachment and is
    // included so ShadowTech can use it later without rebuilding the view.
    ivci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT |
                                       VK_IMAGE_ASPECT_STENCIL_BIT;
    ivci.subresourceRange.levelCount = 1;
    ivci.subresourceRange.layerCount = 1;
    if (vkCreateImageView(g_device, &ivci, g_allocator, &g_depthView) != VK_SUCCESS) {
        destroyDepthResources(); return false;
    }

    // --- the render pass -------------------------------------------------
    //
    // BUILT ONCE AND KEPT. See the note by g_scenePass: the client stores this
    // handle, so a resize must not invalidate it. The only thing that
    // genuinely does is a format change, which is what the check below is for.
    //
    // What arrives here in wd->RenderPass is ImGui's own colour-only pass,
    // freshly made by CreateOrResizeWindow. It is not wanted; it is destroyed
    // once ours is known good.
    const bool passStillValid = (g_scenePass != VK_NULL_HANDLE) &&
                                (g_scenePassColour == wd->SurfaceFormat.format) &&
                                (g_scenePassDepth  == g_depthFormat);

    if (!passStillValid) {

    VkAttachmentDescription att[2]{};
    // colour -- matching what ImGui's helper built, so its pipeline stays
    // compatible: same format, same sample count.
    att[0].format         = wd->SurfaceFormat.format;
    att[0].samples        = VK_SAMPLE_COUNT_1_BIT;
    att[0].loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att[0].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    att[0].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[0].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    att[0].finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    // depth -- cleared every frame, and NOT stored. Nothing reads it after the
    // pass ends, and DONT_CARE lets a tiler discard it instead of writing it
    // back to memory.
    att[1].format         = g_depthFormat;
    att[1].samples        = VK_SAMPLE_COUNT_1_BIT;
    att[1].loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att[1].storeOp        = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[1].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[1].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    att[1].finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colourRef{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkAttachmentReference depthRef { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };

    VkSubpassDescription sub{};
    sub.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount    = 1;
    sub.pColorAttachments       = &colourRef;
    sub.pDepthStencilAttachment = &depthRef;

    // TWO DEPENDENCIES, BECAUSE THE TWO ATTACHMENTS ARE SYNCHRONISED BY
    // DIFFERENT THINGS -- AND ONLY ONE OF THEM IS SYNCHRONISED AT ALL.
    //
    // COLOUR is a swapchain image. The acquire semaphore already makes the
    // presentation engine's prior use available, so srcAccessMask = 0 is the
    // canonical form: an execution dependency is all that is needed. That is
    // what ImGui's own pass declares and it is correct.
    //
    // DEPTH is not. It is ONE image shared by every swapchain frame, it is
    // never presented, and no semaphore anywhere refers to it -- so nothing
    // orders frame N+1's use of it against frame N's. Consecutive frames get
    // DIFFERENT swapchain images, so even the per-image fence wait at the top
    // of renderFrame does not cover it.
    //
    // Synchronization validation names this exactly, 1904 times in a 30 s
    // session:
    //
    //   SYNC-HAZARD-WRITE-AFTER-WRITE
    //   vkCmdBeginRenderPass ... writes to resource, which was previously
    //   written by vkCmdEndRenderPass ...
    //   No sufficient synchronization is present to ensure that a layout
    //   transition does not conflict with a prior write
    //   (VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT) at
    //   VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT.
    //
    // The first version of this dependency got both halves wrong:
    //
    //   * THE STAGE. Depth is written in EARLY *and* LATE fragment tests, and
    //     the end-of-pass layout transition is a LATE_FRAGMENT_TESTS write.
    //     Naming only EARLY put the actual prior write outside the first
    //     synchronization scope, so the dependency did not reach it.
    //   * THE ACCESS. srcAccessMask = 0 is an execution dependency only. A
    //     write-after-write against a prior write additionally needs that
    //     write made AVAILABLE, which requires naming it.
    //
    // srcSubpass = EXTERNAL reaches back over submission order on this queue,
    // so the previous frame's separate vkQueueSubmit is genuinely the source.
    VkSubpassDependency dep[2]{};

    dep[0].srcSubpass    = VK_SUBPASS_EXTERNAL;
    dep[0].dstSubpass    = 0;
    dep[0].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep[0].dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep[0].srcAccessMask = 0;
    dep[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    dep[1].srcSubpass    = VK_SUBPASS_EXTERNAL;
    dep[1].dstSubpass    = 0;
    dep[1].srcStageMask  = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                           VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dep[1].dstStageMask  = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                           VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dep[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dep[1].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                           VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rpci{};
    rpci.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpci.attachmentCount = 2;
    rpci.pAttachments    = att;
    rpci.subpassCount    = 1;
    rpci.pSubpasses      = &sub;
    rpci.dependencyCount = 2;
    rpci.pDependencies   = dep;

    VkRenderPass newPass = VK_NULL_HANDLE;
    if (vkCreateRenderPass(g_device, &rpci, g_allocator, &newPass) != VK_SUCCESS) {
        // Leaves wd->RenderPass holding ImGui's colour-only pass, which still
        // works. A failure here costs depth, not the window.
        destroyDepthResources(); return false;
    }

    if (g_scenePass) vkDestroyRenderPass(g_device, g_scenePass, g_allocator);
    g_scenePass       = newPass;
    g_scenePassColour = wd->SurfaceFormat.format;
    g_scenePassDepth  = g_depthFormat;

    }   // !passStillValid

    // Swap ours in and drop the one ImGui's helper made. Nothing references
    // that pass any more: its framebuffers are rebuilt below, and ImGui's own
    // pipeline was created against ours at ImGui_ImplVulkan_Init time.
    if (wd->RenderPass != VK_NULL_HANDLE && wd->RenderPass != g_scenePass)
        vkDestroyRenderPass(g_device, wd->RenderPass, g_allocator);
    wd->RenderPass = g_scenePass;

    // --- the framebuffers ------------------------------------------------
    // ImGui built one per swapchain image with a single attachment. Each is
    // rebuilt here with [colour, depth]; the depth view is the SAME one in
    // every framebuffer, per the note on the image above.
    for (uint32_t i = 0; i < wd->ImageCount; i++) {
        ImGui_ImplVulkanH_Frame *fd = &wd->Frames[i];
        if (fd->Framebuffer)
            vkDestroyFramebuffer(g_device, fd->Framebuffer, g_allocator);

        VkImageView views[2] = { fd->BackbufferView, g_depthView };
        VkFramebufferCreateInfo fci{};
        fci.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fci.renderPass      = wd->RenderPass;
        fci.attachmentCount = 2;
        fci.pAttachments    = views;
        fci.width           = width;
        fci.height          = height;
        fci.layers          = 1;
        if (vkCreateFramebuffer(g_device, &fci, g_allocator,
                                &fd->Framebuffer) != VK_SUCCESS)
            return false;
    }

    return true;
}

bool createVulkanInstance()
{
    uint32_t extCount = 0;
    const char **glfwExts = glfwGetRequiredInstanceExtensions(&extCount);
    if (!glfwExts) {
        fprintf(stderr, "Orbiter: GLFW reports no Vulkan surface extensions; "
                        "is a Vulkan loader installed?\n");
        return false;
    }

    std::vector<const char *> extensions(glfwExts, glfwExts + extCount);

    // VK_KHR_get_physical_device_properties2, when the loader has it.
    //
    // It is core from Vulkan 1.1, and this instance asks for 1.0 (see below),
    // so on a 1.0 instance it has to be requested by name. It is a dependency
    // of BOTH crash-diagnostic device extensions createLogicalDevice enables,
    // and without it those two are enabled illegally:
    //
    //   vkCreateDevice(): ppEnabledExtensionNames[1] Missing extension
    //   required by the device extension VK_NV_device_diagnostic_checkpoints:
    //   VK_KHR_get_physical_device_properties2
    //
    // -- which the driver honoured anyway, so the checkpoints worked and only
    // the validation layer objected. Enabled properly rather than left as a
    // reported error nobody reads.
    {
        uint32_t n = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
        std::vector<VkExtensionProperties> avail(n);
        if (n) vkEnumerateInstanceExtensionProperties(nullptr, &n, avail.data());
        for (const VkExtensionProperties &e : avail) {
            if (std::string(e.extensionName) ==
                VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME) {
                extensions.push_back(
                    VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
                g_hasPhysDevProps2 = true;
                break;
            }
        }
    }

    VkApplicationInfo app{};
    app.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName   = "Orbiter";

    // VULKAN 1.2, NOT 1.0, AND THE GRAPHICS CLIENT IS WHY.
    //
    // apiVersion is not a request for features -- it declares which semantics
    // the application was written against, and the SPIR-V a shader module may
    // contain is one of them. Under 1.0 semantics the maximum accepted module
    // is SPIR-V 1.0, and OVP/VulkanClient compiles its GLSL with
    //
    //     setEnvClient(EShClientVulkan, EShTargetVulkan_1_2)
    //     setEnvTarget(EShTargetSpv,    EShTargetSpv_1_5)
    //
    // because layout(scalar) -- which every shader-facing struct in that
    // client depends on for its layout -- is core in Vulkan 1.2. With 1.0
    // declared here, EVERY vkCreateShaderModule was rejected:
    //
    //     VUID-VkShaderModuleCreateInfo-pCode-08737
    //     Invalid SPIR-V binary version 1.5 for target environment
    //     SPIR-V 1.0 (under Vulkan 1.0 semantics).
    //
    // Without validation enabled the driver does not report it; it hands back
    // a module that is never usable, and the first draw that reaches it calls
    // through a null pointer inside libnvidia-glcore. That is what stopped the
    // client rendering its first frame.
    //
    // 1.2 is also what VK_EXT_scalar_block_layout was promoted into, so this
    // and createLogicalDevice's hard requirement for that extension are the
    // same decision stated in two places.
    app.apiVersion         = VK_API_VERSION_1_2;

    VkInstanceCreateInfo ci{};
    ci.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo        = &app;
    ci.enabledExtensionCount   = (uint32_t)extensions.size();
    ci.ppEnabledExtensionNames = extensions.data();

    VkResult err = vkCreateInstance(&ci, g_allocator, &g_instance);
    if (err != VK_SUCCESS) {
        fprintf(stderr, "Orbiter: vkCreateInstance failed (%d)\n", (int)err);
        return false;
    }
    return true;
}

// The GPU the user picked on the Launchpad's Video tab, as an index into
// vkEnumeratePhysicalDevices' order. -1 means "no preference recorded", which
// is both the factory default (Config.cpp: Device_idx = -1) and what is left
// after a graphics client is unloaded.
//
// This is set by Orbiter::Create from CfgDevPrm.Device_idx BEFORE the
// Launchpad is built, which is what makes it available here: initialise() runs
// lazily on the first dialog, and the config has been read by then. A value
// arriving after initialise() has no effect this run -- the device is already
// created -- which is exactly right for a setting whose tooltip on Windows is
// also "takes effect on restart".
int g_preferredGpu = -1;

bool pickPhysicalDevice()
{
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(g_instance, &count, nullptr);
    if (count == 0) {
        fprintf(stderr, "Orbiter: no Vulkan-capable device found\n");
        return false;
    }

    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(g_instance, &count, devices.data());

    // Prefer a discrete GPU, as the ImGui example does; fall back to the first
    // device so an integrated-only machine still works.
    g_physicalDevice = devices[0];
    for (VkPhysicalDevice d : devices) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(d, &props);
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            g_physicalDevice = d;
            break;
        }
    }

    // An explicit choice overrides the heuristic. Range-checked rather than
    // trusted: the device list can change between runs (an eGPU unplugged, a
    // driver removed), and a stale index out of range must fall back to the
    // heuristic above rather than index past the end of the vector.
    if (g_preferredGpu >= 0 && (uint32_t)g_preferredGpu < count) {
        g_physicalDevice = devices[g_preferredGpu];
    } else if (g_preferredGpu >= 0) {
        fprintf(stderr, "Orbiter: configured GPU index %d is out of range "
                        "(%u present); using the default device\n",
                g_preferredGpu, count);
    }

    uint32_t famCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g_physicalDevice, &famCount, nullptr);
    std::vector<VkQueueFamilyProperties> fams(famCount);
    vkGetPhysicalDeviceQueueFamilyProperties(g_physicalDevice, &famCount, fams.data());

    for (uint32_t i = 0; i < famCount; ++i) {
        if (fams[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { g_queueFamily = i; break; }
    }
    if (g_queueFamily == (uint32_t)-1) {
        fprintf(stderr, "Orbiter: no graphics queue family\n");
        return false;
    }
    return true;
}

// GPU-CRASH DIAGNOSTICS, and why they are enabled at DEVICE CREATION.
//
// When the GPU is reset out from under us the driver reports Xid 109
// (CTX SWITCH TIMEOUT) and every subsequent Vulkan call returns
// VK_ERROR_DEVICE_LOST. That error names no command, no pipeline and no
// draw -- it is reported by whatever call happens to run next, which is
// almost never the one at fault. On D3D9 the equivalent (D3DERR_DEVICELOST
// after a TDR) is equally mute, so there is no reference behaviour to copy;
// this is a pure Vulkan facility with no Windows counterpart.
//
// Two extensions turn that silence into an answer:
//
//   VK_NV_device_diagnostic_checkpoints  vkCmdSetCheckpointNV writes a marker
//       into the command stream; after a loss vkGetQueueCheckpointDataNV
//       returns the markers the GPU had reached, so the LAST one names the
//       command that hung.
//   VK_EXT_device_fault  vkGetDeviceFaultInfoEXT returns the driver's own
//       description of the fault (address ranges, vendor fault code).
//
// Both must be enabled here, in the device this host owns, because a client
// cannot add an extension to a device it merely adopts. Enabling an extension
// costs nothing on its own -- the per-command cost is only paid when
// vkCmdSetCheckpointNV is actually recorded, which the client gates behind
// ORBITER_VK_CHECKPOINTS. Neither is required: absent, the flags stay false
// and the client falls back to reporting the bare error.
bool g_hasCheckpoints = false;
bool g_hasDeviceFault = false;

bool createLogicalDevice()
{
    const float priority = 1.0f;

    std::vector<const char *> deviceExts;
    deviceExts.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);

    // BOTH DIAGNOSTIC EXTENSIONS DEPEND ON VK_KHR_get_physical_device_properties2,
    // an INSTANCE extension, and enabling a device extension whose dependency
    // is missing is invalid however willing the driver is to accept it. So the
    // instance's answer gates the device's request rather than the two being
    // decided independently. See createVulkanInstance.
    //
    // VK_EXT_scalar_block_layout has the same dependency and is found in the
    // same sweep, but it is REQUIRED rather than diagnostic -- see below.
    bool hasScalarLayout = false;
    {
        uint32_t n = 0;
        vkEnumerateDeviceExtensionProperties(g_physicalDevice, nullptr, &n, nullptr);
        std::vector<VkExtensionProperties> avail(n);
        if (n) vkEnumerateDeviceExtensionProperties(g_physicalDevice, nullptr,
                                                    &n, avail.data());
        for (const VkExtensionProperties &e : avail) {
            const std::string name(e.extensionName);
            if (name == VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME)
                g_hasCheckpoints = true;
            else if (name == VK_EXT_DEVICE_FAULT_EXTENSION_NAME)
                g_hasDeviceFault = true;
            else if (name == VK_EXT_SCALAR_BLOCK_LAYOUT_EXTENSION_NAME)
                hasScalarLayout = true;
        }
        if (!g_hasPhysDevProps2) {
            g_hasCheckpoints = g_hasDeviceFault = false;
            hasScalarLayout = false;
            fprintf(stderr, "Orbiter: GPU crash diagnostics unavailable -- "
                            "VK_KHR_get_physical_device_properties2 is not "
                            "supported by this loader, and both extensions "
                            "require it\n");
        }
        if (g_hasCheckpoints)
            deviceExts.push_back(VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME);
        if (g_hasDeviceFault)
            deviceExts.push_back(VK_EXT_DEVICE_FAULT_EXTENSION_NAME);

        // SCALAR BLOCK LAYOUT, AND WHY IT IS NOT OPTIONAL.
        //
        // This has no Windows counterpart at all, and it exists because of
        // the one thing HLSL constant registers did not have: a layout rule
        // that the C++ struct has to agree with.
        //
        // D3D9 uploaded a constant with ID3DXConstantTable::SetValue, which
        // copied the caller's bytes into the register file at the offset the
        // constant table named. The client's C++ structs -- VulkanSun,
        // VulkanMatExt, VulkanTune, CelDataStruct and the rest -- are packed
        // (#pragma pack(1) around them in VulkanUtil.h) and were written to
        // match the HLSL declaration field for field.
        //
        // GLSL's DEFAULT uniform-block rule, std140, does not lay out that
        // way: it rounds every vec3 up to 16 bytes and every array element up
        // to 16. VulkanSun's five FVECTOR3s would go from 60 bytes to 80, and
        // every memcpy the client makes into a uniform buffer would write the
        // wrong fields into the wrong places -- silently, because a memcpy
        // cannot fail.
        //
        // layout(scalar) is the rule that says "lay this out exactly as C
        // would", and every converted shader in the client declares its block
        // that way. It is what makes those structs, the static_asserts that
        // guard their sizes, and ShaderReflection's byte offsets all describe
        // the same bytes.
        //
        // It is therefore a hard requirement, not a degradation: without it
        // the extension is not enabled, the shaders' layout decorations are
        // illegal, and there is no fallback path that could work.
        if (!hasScalarLayout) {
            fprintf(stderr, "Orbiter: required Vulkan extension '%s' is not "
                            "supported by this device (every graphics-client "
                            "shader declares its uniform blocks "
                            "layout(scalar))\n",
                    VK_EXT_SCALAR_BLOCK_LAYOUT_EXTENSION_NAME);
            return false;
        }
        deviceExts.push_back(VK_EXT_SCALAR_BLOCK_LAYOUT_EXTENSION_NAME);
    }

    VkDeviceQueueCreateInfo q{};
    q.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    q.queueFamilyIndex = g_queueFamily;
    q.queueCount       = 1;
    q.pQueuePriorities = &priority;

    // ENABLED FEATURES, NOT AVAILABLE ONES.
    //
    // pEnabledFeatures used to be left null here, which means EVERY optional
    // feature is off no matter what the hardware supports. That is a quiet
    // trap, because VulkanDevice.cpp queries vkGetPhysicalDeviceFeatures --
    // what the GPU CAN do -- and logs it:
    //
    //     BC/DXT compression...... : Yes
    //     Fill mode non-solid..... : Yes
    //     Anisotropic filtering... : Yes
    //
    // Every one of those lines was true and useless: the device had none of
    // them enabled, so using them is invalid. It is the same shape as
    // "Multisampling: 4x" -- a capability reported as though it were a
    // decision. Anything that reached for one of these would have failed at
    // pipeline or image creation, naming the feature but not the reason.
    //
    // The list is taken from what the client actually asks for --
    // VulkanDevice::CheckMinimumRequirements and the technique state table in
    // the porting notes -- not from what looked useful.
    VkPhysicalDeviceFeatures have{};
    vkGetPhysicalDeviceFeatures(g_physicalDevice, &have);

    VkPhysicalDeviceFeatures want{};

    struct FeatureReq {
        const char *name;
        VkBool32 VkPhysicalDeviceFeatures::*bit;
        bool required;
        const char *why;
    };
    static const FeatureReq kFeatures[] = {
        // Orbiter's textures are DXT1/3/5, and VulkanDevice hard-fails
        // without this, so it is required here too.
        //
        // MEASURED CORRECTION, because the obvious reading of this is wrong:
        // enabling the feature is NOT what permits a BC image to be created.
        // With it deliberately left disabled, D3D9Moon_A.dds still loaded as
        // BC3_UNORM_BLOCK, sampled correctly and produced ZERO validation
        // errors on this device. The feature is a PORTABILITY GUARANTEE --
        // "every BC format is supported" -- while whether a given BC format
        // can actually be used is a physical-device format query, which does
        // not consult enabled features at all.
        //
        // So it stays required for the guarantee and to match the client's own
        // check, not because anything here would fail without it.
        { "textureCompressionBC", &VkPhysicalDeviceFeatures::textureCompressionBC,
          true,  "Orbiter's textures are DXT1/3/5" },

        // Degradations, not failures: the client warns and carries on.
        { "samplerAnisotropy", &VkPhysicalDeviceFeatures::samplerAnisotropy,
          false, "the Anisotrophy config setting is ignored without it" },
        { "fillModeNonSolid", &VkPhysicalDeviceFeatures::fillModeNonSolid,
          false, "Mesh.fx BoundingBox/BoundingSphere/TileBox are wireframe" },
        { "depthClamp", &VkPhysicalDeviceFeatures::depthClamp,
          false, "the distant-body pass clamps rather than clips" },
        { "wideLines", &VkPhysicalDeviceFeatures::wideLines,
          false, "any pipeline with lineWidth != 1.0" },
        { "independentBlend", &VkPhysicalDeviceFeatures::independentBlend,
          false, "per-attachment blend state in multi-target passes" },
        { "dualSrcBlend", &VkPhysicalDeviceFeatures::dualSrcBlend,
          false, "dual-source blending" },
        { "shaderFloat64", &VkPhysicalDeviceFeatures::shaderFloat64,
          false, "double precision in shaders" },
        { "geometryShader", &VkPhysicalDeviceFeatures::geometryShader,
          false, "no technique needs one yet" },
        { "tessellationShader", &VkPhysicalDeviceFeatures::tessellationShader,
          false, "no technique needs one yet" },
        { "depthBounds", &VkPhysicalDeviceFeatures::depthBounds,
          false, "no technique uses the bounds test" },
    };

    for (const FeatureReq &f : kFeatures) {
        if (have.*f.bit) {
            want.*f.bit = VK_TRUE;
        } else if (f.required) {
            fprintf(stderr, "Orbiter: required Vulkan feature '%s' is not "
                            "supported by this device (%s)\n", f.name, f.why);
            return false;
        } else {
            fprintf(stderr, "Orbiter: optional Vulkan feature '%s' unavailable "
                            "(%s)\n", f.name, f.why);
        }
    }

    VkDeviceCreateInfo ci{};
    ci.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    ci.queueCreateInfoCount    = 1;
    ci.pQueueCreateInfos       = &q;
    ci.enabledExtensionCount   = (uint32_t)deviceExts.size();
    ci.ppEnabledExtensionNames = deviceExts.data();
    ci.pEnabledFeatures        = &want;

    // ENABLING AN EXTENSION IS NOT ENABLING ITS FEATURE. Several of these
    // extensions carry a feature bit that lives in its own structure rather
    // than in VkPhysicalDeviceFeatures, and the bit is off unless the
    // structure is chained onto VkDeviceCreateInfo::pNext and set. Naming the
    // extension alone leaves the feature disabled, which is exactly the trap
    // pEnabledFeatures used to be.
    //
    // Two of them here, chained in the order they are declared. (The NV
    // checkpoints extension has no feature bit at all, so it needs nothing.)
    void *pNextChain = NULL;

    // Scalar block layout -- see the note in the extension sweep above for
    // why the whole client depends on it.
    VkPhysicalDeviceScalarBlockLayoutFeaturesEXT scalarFeat{};
    scalarFeat.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES_EXT;
    scalarFeat.scalarBlockLayout = VK_TRUE;
    scalarFeat.pNext = pNextChain;
    pNextChain = &scalarFeat;

    // VK_EXT_device_fault is the other one.
    VkPhysicalDeviceFaultFeaturesEXT faultFeat{};
    if (g_hasDeviceFault) {
        faultFeat.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT;
        faultFeat.deviceFault = VK_TRUE;
        faultFeat.pNext = pNextChain;
        pNextChain = &faultFeat;
    }

    ci.pNext = pNextChain;

    if (vkCreateDevice(g_physicalDevice, &ci, g_allocator, &g_device) != VK_SUCCESS) {
        fprintf(stderr, "Orbiter: vkCreateDevice failed\n");
        return false;
    }
    fprintf(stderr, "Orbiter: GPU crash diagnostics -- checkpoints %s, "
                    "device fault %s\n",
            g_hasCheckpoints ? "available" : "UNAVAILABLE",
            g_hasDeviceFault ? "available" : "UNAVAILABLE");
    vkGetDeviceQueue(g_device, g_queueFamily, 0, &g_queue);

    // THIS POOL SERVES TWO CONSUMERS, AND ONLY ONE OF THEM IS ImGui.
    //
    // It used to declare COMBINED_IMAGE_SAMPLER and nothing else, which is
    // exactly right for ImGui -- one combined image/sampler per texture it
    // binds, and the Launchpad binds very few. But this pool is also what
    // orbiter_GetVulkanContext publishes to the graphics client, and the
    // client's descriptor model is deliberately not ImGui's.
    //
    // The 47 translated shaders use SEPARATE images and samplers, because
    // that is what the HLSL->GLSL translation required: HLSL's `sampler`
    // parameter becomes a (texture2D, sampler) pair in GLSL. So:
    //
    //   set 0   FrameBlock, ObjectBlock          2 x UNIFORM_BUFFER
    //   set 1   13 x texture2D, 2 x textureCube  15 x SAMPLED_IMAGE
    //   set 2   6 x sampler                      6 x SAMPLER
    //
    // NONE of those three types was in the pool, so the client's first
    // vkAllocateDescriptorSets would have failed with
    // VK_ERROR_OUT_OF_POOL_MEMORY -- at allocation time, naming the pool but
    // not the reason it was built that way.
    //
    // THE NUMBERS BELOW ARE A STARTING POINT, NOT A MEASUREMENT. Set 1 is the
    // one that scales: one per material, so a full scene wants as many as
    // there are distinct texture bindings. 512 is a guess with headroom, and
    // the right way to settle it is to count materials in a real scenario and
    // come back. Exhaustion is loud (OUT_OF_POOL_MEMORY at allocation) rather
    // than silent, which is what makes a provisional number acceptable here.
    // SET 0 IS PER DRAW, NOT PER FRAME, and that is why kFrameSets is no
    // longer 32. ObjectBlock lives in set 0 and the reference sets its
    // per-group state before every DrawIndexedPrimitive, so with an ordinary
    // (non-dynamic) uniform buffer the client needs one set 0 per MESH GROUP
    // per frame in flight. A DeltaGlider exterior is dozens of groups and a
    // scenario holds several vessels.
    //
    // 2048 is the interim ceiling, matching the client's ring length. It goes
    // away when ObjectBlock becomes UNIFORM_BUFFER_DYNAMIC, at which point one
    // set 0 per frame is enough again and this returns to 32.
    const uint32_t kImGuiSets    = 64;    // ImGui, one per bound texture
    const uint32_t kMaterialSets = 512;   // client set 1
    const uint32_t kFrameSets    = 2048;  // client set 0, per group per frame
    const uint32_t kSamplerSets  = 16;    // client set 2, shared and few

    VkDescriptorPoolSize sizes[] = {
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kImGuiSets },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         kFrameSets    *  2 },
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,          kMaterialSets * 15 },
        { VK_DESCRIPTOR_TYPE_SAMPLER,                kSamplerSets  *  6 },
    };
    VkDescriptorPoolCreateInfo pool{};
    pool.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    // FREE_DESCRIPTOR_SET_BIT so the client can release a material's set when
    // its mesh is unloaded, rather than only ever resetting the whole pool.
    pool.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool.maxSets       = kImGuiSets + kMaterialSets + kFrameSets + kSamplerSets;
    pool.poolSizeCount = (uint32_t)(sizeof(sizes) / sizeof(sizes[0]));
    pool.pPoolSizes    = sizes;

    if (vkCreateDescriptorPool(g_device, &pool, g_allocator,
                               &g_descriptorPool) != VK_SUCCESS) {
        fprintf(stderr, "Orbiter: vkCreateDescriptorPool failed\n");
        return false;
    }
    return true;
}

// The usage flags every swapchain image is created with.
//
// COLOR_ATTACHMENT is what ImGui's example asks for and all it needs.
// TRANSFER_SRC is added so the back buffer can be copied out for a
// screenshot -- see recordFrameCapture and orbiter_CaptureBackBuffer -- and
// it is asked for only when the surface actually offers it, because a usage
// flag a surface does not support makes vkCreateSwapchainKHR fail outright.
// Every desktop driver offers it; a headless or exotic one that does not
// simply loses screenshots.
//
// setupSurface must have run: this reads the surface's capabilities.
VkImageUsageFlags swapchainUsage()
{
    VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    VkSurfaceCapabilitiesKHR caps{};
    if (g_mainWindowData.Surface != VK_NULL_HANDLE &&
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_physicalDevice,
                                                  g_mainWindowData.Surface,
                                                  &caps) == VK_SUCCESS &&
        (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
        usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

    return usage;
}

bool setupSurface(int width, int height)
{
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (glfwCreateWindowSurface(g_instance, g_window, g_allocator,
                                &surface) != VK_SUCCESS) {
        fprintf(stderr, "Orbiter: glfwCreateWindowSurface failed\n");
        return false;
    }

    ImGui_ImplVulkanH_Window *wd = &g_mainWindowData;
    wd->Surface = surface;

    VkBool32 supported = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(g_physicalDevice, g_queueFamily,
                                         wd->Surface, &supported);
    if (!supported) {
        fprintf(stderr, "Orbiter: queue family cannot present to the surface\n");
        return false;
    }

    const VkFormat formats[] = {
        VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM,
        VK_FORMAT_B8G8R8_UNORM,   VK_FORMAT_R8G8B8_UNORM
    };
    const VkColorSpaceKHR colourSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    wd->SurfaceFormat = ImGui_ImplVulkanH_SelectSurfaceFormat(
        g_physicalDevice, wd->Surface, formats,
        (size_t)IM_ARRAYSIZE(formats), colourSpace);

    // FIFO is the only mode required to be supported, and it is the right
    // choice for a menu: vsynced, no tearing, and it idles the GPU rather
    // than spinning it on a static screen.
    //
    // "Disable vertical sync" on the Video tab is exactly this choice, so the
    // preference selects the candidate list rather than a fixed one. MAILBOX
    // is offered ahead of IMMEDIATE for the unsynced case because it drops
    // frames instead of tearing; SelectPresentMode falls back through the list
    // and ends at FIFO, which is guaranteed present, so an implementation
    // offering neither still works.
    VkPresentModeKHR vsynced[]  = { VK_PRESENT_MODE_FIFO_KHR };
    VkPresentModeKHR unsynced[] = { VK_PRESENT_MODE_MAILBOX_KHR,
                                    VK_PRESENT_MODE_IMMEDIATE_KHR,
                                    VK_PRESENT_MODE_FIFO_KHR };
    wd->PresentMode = g_noVSync
        ? ImGui_ImplVulkanH_SelectPresentMode(g_physicalDevice, wd->Surface,
                                              unsynced, IM_ARRAYSIZE(unsynced))
        : ImGui_ImplVulkanH_SelectPresentMode(g_physicalDevice, wd->Surface,
                                              vsynced, IM_ARRAYSIZE(vsynced));

    // ImGui's helper destroys whatever is in wd->RenderPass before making its
    // own. Ours is not its to destroy, so it is unhooked first -- see the note
    // by g_scenePass. (Nothing to protect on this first call; the pairing is
    // kept identical to the resize site so the two cannot drift apart.)
    wd->RenderPass = VK_NULL_HANDLE;

    // TRANSFER_SRC AS WELL AS COLOR_ATTACHMENT, so the back buffer can be
    // READ BACK. A swapchain image may only be the source of a copy if it was
    // created with that flag, and ImGui's example asks for the attachment bit
    // alone:
    //
    //     VUID-vkCmdCopyImageToBuffer-srcImage-00186
    //     srcImage was created with VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT but
    //     requires VK_IMAGE_USAGE_TRANSFER_SRC_BIT
    //
    // D3D9 needed no equivalent request: a back buffer was always a valid
    // source for GetRenderTargetData, which is what the reference's
    // clbkSaveSurfaceToImage uses. See recordFrameCapture.
    //
    // It is a request, not an assumption -- swapchainUsage is intersected with
    // what the surface reports, so an implementation that does not offer
    // TRANSFER_SRC loses the screenshot rather than failing to start.
    ImGui_ImplVulkanH_CreateOrResizeWindow(
        g_instance, g_physicalDevice, g_device, wd, g_queueFamily,
        g_allocator, width, height, (uint32_t)g_minImageCount,
        swapchainUsage());

    // Replace ImGui's colour-only pass with colour+depth, BEFORE
    // ImGui_ImplVulkan_Init reads wd->RenderPass. See the note by g_depthView.
    if (!attachDepth(wd, (uint32_t)width, (uint32_t)height)) {
        fprintf(stderr, "Orbiter: could not attach a depth buffer; "
                        "3D clients will be limited to depth-less passes\n");
    }
    return true;
}

// ---------------------------------------------------------------------------
// Dialog tree rendering
//
// ImGui is used here as a draw surface and input source, NOT as a widget
// toolkit. An ImGui::Button looks like ImGui; a Win32 push button looks like
// Windows, and no styling of the former produces the latter. So every control
// is drawn explicitly, and hit-testing is done against its rectangle.
//
// Orbiter.rc is the specification: it gives the exact position, size, style
// and font of every control, which is the same data USER32 lays out from. Draw
// it the way USER32 draws it at those coordinates and the result matches by
// construction rather than by eye.
// ---------------------------------------------------------------------------

// Windows system colours, matching Gdi.cpp's GetSysColor. Orbiter's
// owner-drawn controls tint themselves from that function and sit beside
// controls drawn here, so the two must agree.
const ImU32 kFace       = IM_COL32(240, 240, 240, 255);
const ImU32 kFaceHot    = IM_COL32(229, 241, 251, 255);   // hover
const ImU32 kFacePushed = IM_COL32(204, 228, 247, 255);
const ImU32 kBorder     = IM_COL32(173, 173, 173, 255);
const ImU32 kBorderHot  = IM_COL32( 0,  120, 215, 255);
const ImU32 kText       = IM_COL32(  0,   0,   0, 255);
const ImU32 kTextGrey   = IM_COL32(109, 109, 109, 255);
const ImU32 kWindow     = IM_COL32(255, 255, 255, 255);
const ImU32 kFieldEdge  = IM_COL32(122, 122, 122, 255);
// Scrollbar colours, matching the Win32 3D look: COLOR_SCROLLBAR for the
// channel and COLOR_BTNFACE for the thumb.
const ImU32 kScrollTrack = IM_COL32(200, 200, 200, 255);
const ImU32 kScrollThumb = IM_COL32(240, 240, 240, 255);
const ImU32 kEtchDark   = IM_COL32(160, 160, 160, 255);
const ImU32 kEtchLight  = IM_COL32(255, 255, 255, 255);
// The dialog background is the system face colour, NOT Launchpad.cpp's
// dlgcol. Read Launchpad.cpp end to end and the reason is plain:
//
//   const DWORD dlgcol = 0xF0F4F8;                      // line 39
//   hDlgBrush = CreateSolidBrush (dlgcol);              // constructor
//
//   INT_PTR LaunchpadDialog::DlgProc (...)
//   ...
//   //	case WM_CTLCOLORDLG:                            // lines 398-399
//   //		return (LRESULT)hDlgBrush;                  // COMMENTED OUT
//
// The main dialog's WM_CTLCOLORDLG is commented out, so the Launchpad never
// paints itself with dlgcol -- it gets the default dialog face. hDlgBrush is
// returned only by WaitProc, for IDD_PAGE_WAIT2 and IDC_WAITTEXT. dlgcol is
// the LOADING SCREEN colour.
//
// Confirmed against a running Windows Launchpad: its page background samples
// (245,245,245), a neutral grey. Painting dlgcol here gave (248,244,240) --
// warm, and wrong everywhere at once.
//
// IDC_MNU_PAGECONTAINER is separate and already correct: it is SS_OWNERDRAW,
// and Launchpad.cpp's WM_DRAWITEM fills it with GetSysColorBrush(COLOR_3DFACE)
// explicitly, which arrives through the owner-draw branch below.
const ImU32 kDialogBk   = IM_COL32(240, 240, 240, 255);   // COLOR_3DFACE

// dlgcol itself, kept for the wait page that is its only real consumer.
const ImU32 kWaitPageBk = IM_COL32(248, 244, 240, 255);

// Control geometry arrives in PIXELS.
//
// Win32Dlg.cpp converts the template's dialog units once, when the dialog is
// instantiated, and everything after that is pixels -- which is what Windows
// does and what Launchpad::Resize assumes when it recomputes positions from
// GetClientRect and feeds them back through SetWindowPos. Converting again
// here would scale every control a second time.
//
// Tahoma is loaded at the size Windows uses for 8pt MS Shell Dlg, so text
// measures the same and captions fit the rectangles the template gave them.

// ---------------------------------------------------------------------------
// Latin-1 to UTF-8, for text that came from the application.
// ---------------------------------------------------------------------------
//
// ImGui draws UTF-8 and substitutes a replacement glyph for a byte that is not
// part of a valid sequence. Orbiter's sources are NOT all UTF-8:
//
//     $ file Src/Plugin/ScnEditor/Editor.cpp
//     Editor.cpp: C++ source, ISO-8859 text
//
// and Editor.cpp:1590 is
//
//     sprintf (cbuf, "%0.3f \xB0", prm.MnA*DEG);   // 0xB0 = Latin-1 degree
//
// so the Scenario Editor's Elements tab reported "Mn anomaly: -180.000 ?" for
// six of its nine secondary parameters. On Windows the byte is drawn through
// the ANSI codepage and comes out as a degree sign.
//
// THE TREE IS MIXED, which is why this is a test and not a blanket
// conversion. the porting notes record the counterpart in the
// MFDs: MfdHsi.cpp:216 passes length 9 for "CRS %03.0f°" because THAT source
// file is UTF-8 and the degree is two bytes there. Converting unconditionally
// would turn those into "Â°".
//
// So: if the string is already valid UTF-8, it is left exactly as it is;
// otherwise every byte is treated as Latin-1 and widened. A byte sequence that
// is accidentally valid multi-byte UTF-8 while being meant as Latin-1 is
// possible in principle and does not occur here -- it would need a 0xC2..0xF4
// byte followed by the right number of 0x80..0xBF continuation bytes, and the
// strings in question are ASCII plus one high byte.
bool isValidUtf8(const std::string &s)
{
    const unsigned char *p = (const unsigned char *)s.c_str();
    const unsigned char *end = p + s.size();
    while (p < end) {
        if (*p < 0x80) { ++p; continue; }
        int extra;
        if      ((*p & 0xE0) == 0xC0) extra = 1;
        else if ((*p & 0xF0) == 0xE0) extra = 2;
        else if ((*p & 0xF8) == 0xF0) extra = 3;
        else return false;                       // 0x80..0xBF or 0xF8..0xFF lead
        if (p + extra >= end) return false;
        for (int i = 1; i <= extra; ++i)
            if ((p[i] & 0xC0) != 0x80) return false;
        p += extra + 1;
    }
    return true;
}

std::string toUtf8(const std::string &s)
{
    bool hasHigh = false;
    for (unsigned char c : s) if (c >= 0x80) { hasHigh = true; break; }
    if (!hasHigh) return s;                      // pure ASCII, the common case
    if (isValidUtf8(s)) return s;                // already UTF-8, leave alone

    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        if (c < 0x80) out += (char)c;
        else { out += (char)(0xC0 | (c >> 6)); out += (char)(0x80 | (c & 0x3F)); }
    }
    return out;
}

// Strips the '&' mnemonic marker and reports where the underline goes.
// Windows draws the marked character underlined and binds it to Alt.
std::string stripMnemonic(const std::string &s, int *underlineAt)
{
    std::string out;
    if (underlineAt) *underlineAt = -1;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '&') {
            if (i + 1 < s.size() && s[i + 1] == '&') { out += '&'; ++i; continue; }
            if (underlineAt && *underlineAt < 0) *underlineAt = (int)out.size();
            continue;
        }
        out += s[i];
    }
    return out;
}

// Draws text with the mnemonic underline, honouring the alignment the control
// style asks for.
void drawLabel(ImDrawList *dl, const ImVec2 &rectMin, const ImVec2 &rectMax,
               const std::string &raw, ImU32 colour, unsigned style,
               bool centreVertically = true)
{
    int underlineAt = -1;
    // toUtf8 first: the mnemonic scan is byte-wise and only looks at '&',
    // which no UTF-8 continuation byte can be, so either order is safe -- but
    // converting first means underlineAt indexes the string that is actually
    // drawn, which is what the underline measurement below assumes.
    const std::string text = stripMnemonic(toUtf8(raw), &underlineAt);
    if (text.empty()) return;

    const ImVec2 size = ImGui::CalcTextSize(text.c_str());

    float x = rectMin.x;
    if (style & SS_CENTER)     x = rectMin.x + ((rectMax.x - rectMin.x) - size.x) * 0.5f;
    else if (style & SS_RIGHT) x = rectMax.x - size.x;

    const float y = centreVertically
        ? rectMin.y + ((rectMax.y - rectMin.y) - size.y) * 0.5f
        : rectMin.y;

    dl->AddText(ImVec2(x, y), colour, text.c_str());

    if (underlineAt >= 0 && underlineAt < (int)text.size()) {
        const std::string before = text.substr(0, underlineAt);
        const std::string ch     = text.substr(underlineAt, 1);
        const float ux = x + ImGui::CalcTextSize(before.c_str()).x;
        const float uw = ImGui::CalcTextSize(ch.c_str()).x;
        const float uy = y + size.y - 1.0f;
        dl->AddLine(ImVec2(ux, uy), ImVec2(ux + uw, uy), colour, 1.0f);
    }
}

// Text for a push button, honouring the alignment its BS_ style asks for.
//
// A push button does NOT always centre. BS_LEFT (0x0100), BS_RIGHT (0x0200)
// and BS_CENTER (0x0300) move the caption, and the Launchpad depends on it:
// every tab button carries BS_RIGHT -- IDC_MNU_SCN is 0x50020200 and the rest
// are 0x50010200 -- so "Scenarios ", "Options " and the others sit against the
// right edge, the trailing space in each caption providing the gap. Centring
// them regardless left the whole tab column misaligned against Windows.
//
// Vertical alignment follows BS_TOP (0x0400) / BS_BOTTOM (0x0800), defaulting
// to centred as BS_VCENTER (0x0C00) does.
void drawButtonLabel(ImDrawList *dl, const ImVec2 &a, const ImVec2 &b,
                     const std::string &raw, ImU32 colour, unsigned style)
{
    // Map the button's horizontal alignment onto the static equivalent that
    // drawLabel understands.
    unsigned align;
    switch (style & BS_CENTER) {
    case BS_LEFT:  align = SS_LEFT;   break;
    case BS_RIGHT: align = SS_RIGHT;  break;
    default:       align = SS_CENTER; break;   // BS_CENTER, or unspecified
    }

    if ((style & BS_VCENTER) == BS_TOP) {
        drawLabel(dl, a, b, raw, colour, align, false);
    } else {
        drawLabel(dl, a, b, raw, colour, align, true);
    }
}

// Hit-test plus click detection against an explicit rectangle. ImGui's
// InvisibleButton is used only for input; nothing it draws is kept.
bool hitButton(const char *id, const ImVec2 &pos, const ImVec2 &size,
               bool *hovered, bool *held)
{
    ImGui::SetCursorScreenPos(pos);
    const bool clicked = ImGui::InvisibleButton(id, size);
    if (hovered) *hovered = ImGui::IsItemHovered();
    if (held)    *held    = ImGui::IsItemActive();
    return clicked;
}

// ---------------------------------------------------------------------------
// WHAT A CUSTOM SWAP CHAIN PRESENTS INTO, ON A PLATFORM WITH ONE WINDOW.
//
// gcCore::RegisterSwap lets an add-on hang a D3D9 additional swap chain on one
// of its own child windows and present its own rendering into it. DX9ExtMFD is
// the consumer in this tree: MFDWindow::clbkRefreshDisplay blits the MFD's
// display surface onto the swap's back buffer and calls FlipSwap, and its
// WM_PAINT handler (RepaintDisplay) deliberately draws NOTHING, because the
// picture arrives from the present rather than from GDI.
//
// There is no OS child window here to present to -- IDC_DISPLAY is a rectangle
// this file draws, not an X11 window -- and the core owns the only
// VkSurfaceKHR and the only vkQueuePresentKHR. But the *destination* is
// something this file can express exactly: the swap's back buffer becomes an
// ordinary sampled render target, and "present it into that child window"
// becomes "draw that image over that control's rectangle". Nothing about the
// add-on's side changes; MFDWindow.cpp is byte-identical to the reference.
//
// Keyed on HWND, published by gcCore::FlipSwap through orbiter_SetControlImage
// and cleared by RegisterSwap (on re-registration) and ReleaseSwap, so the
// entry cannot outlive the VkImageView it names.
struct ControlImage {
    void *view = nullptr;
    int   w    = 0;
    int   h    = 0;
};
std::map<HWND, ControlImage> g_controlImages;

void drawControl(HWND h, float originX, float originY)
{
    const char *cls = nullptr, *text = nullptr;
    int id = 0, x = 0, y = 0, cx = 0, cy = 0, visible = 0, enabled = 0;
    unsigned style = 0;
    orbiter_GetControlInfo(h, &cls, &text, &id, &x, &y, &cx, &cy,
                           &style, &visible, &enabled);
    if (!visible || !cls) return;

    const ImVec2 pos(originX + (float)x, originY + (float)y);
    const ImVec2 size((float)cx, (float)cy);
    const ImVec2 posMax(pos.x + size.x, pos.y + size.y);

    ImDrawList *dl = ImGui::GetWindowDrawList();
    const std::string cname(cls);
    const std::string label(text ? text : "");
    const ImU32 labelColour = enabled ? kText : kTextGrey;

    // Scoped by the window HANDLE, not the control id.
    //
    // Control ids are NOT unique: every IDC_STATIC in a template is -1, and
    // the resource header defines it that way deliberately. Pushing the id
    // gives every static in a dialog the same ImGui ID, and any two that
    // submit an item collide -- which is what raised
    //     "Programmer error: 2 visible items with conflicting ID!"
    // on the MFD parameters dialog, whose template has several. The window
    // pointer is unique per control by construction.
    ImGui::PushID((const void *)h);

    // A child dialog draws no chrome of its own -- a Launchpad tab page is
    // just a container for its controls -- so it falls straight through to the
    // child pass at the bottom.
    if (!orbiter_IsDialog(h)) {
    if (cname == "Button") {
        const unsigned kind = style & 0x0F;

        // BS_PUSHLIKE: "Makes a button (such as a check box, three-state check
        // box, or radio button) look and act like a push button. The button
        // looks raised when it isn't pushed or checked, and sunken when it is
        // pushed or checked."
        //
        // It is a DRAWING bit, not a behaviour bit -- the control is still a
        // check box or a radio and still carries a check state, still clears
        // its group siblings, still sends BN_CLICKED. Only the picture is a
        // button, held down while checked.
        //
        // Never tested here, so every one of these drew as a box with a label
        // beside it. That is not a corner case:
        //
        //   Orbiter.rc:483   IDC_VID_FULL / IDC_VID_WINDOW -- the Launchpad
        //                    Video tab's "Full Screen" and "Window" pair
        //   Orbiter.rc:963   IDC_MAP_DRAG / IDC_MAP_SELECT -- the Map
        //                    dialog's tool buttons (also BS_ICON)
        //   DeltaGlider.rc   gear Up/Down, nosecone Close/Open, outer and
        //                    inner airlock Close/Open -- the whole of the
        //                    DG control dialog
        //   ShuttleA.rc:64   IDC_AUX_SYNC
        //
        // A pair of them reads as a two-position switch, which is the entire
        // point of the style and was completely lost.
        const bool pushLike = (style & BS_PUSHLIKE) &&
            (kind == BS_AUTOCHECKBOX || kind == BS_CHECKBOX ||
             kind == BS_AUTORADIOBUTTON || kind == BS_RADIOBUTTON);

        if (pushLike) {
            bool hovered = false, held = false;
            const bool clicked = enabled &&
                hitButton("##pl", pos, size, &hovered, &held);
            const bool checked = orbiter_GetCheckState(h) == BST_CHECKED;

            // Sunken while CHECKED as well as while held -- that is the whole
            // of what the style asks for.
            const ImU32 face = !enabled            ? kFace
                             : (held || checked)   ? kFacePushed
                             : hovered             ? kFaceHot : kFace;
            dl->AddRectFilled(pos, posMax, face);
            dl->AddRect(pos, posMax,
                        (hovered || checked) ? kBorderHot : kBorder);
            drawButtonLabel(dl, pos, posMax, label, labelColour, style);

            if (clicked) {
                // The same two answers the box-drawn forms give below: a
                // radio clears its group, a check box toggles.
                if (kind == BS_AUTORADIOBUTTON || kind == BS_RADIOBUTTON)
                    orbiter_CheckRadioButton(h);
                else
                    orbiter_SetCheckState(h, checked ? BST_UNCHECKED
                                                     : BST_CHECKED);
                orbiter_NotifyCommand(h, BN_CLICKED);
            }

        } else if (kind == BS_AUTOCHECKBOX || kind == BS_CHECKBOX) {
            // A 13x13 box on the left, text to its right, vertically centred.
            const float boxSide = 13.0f;
            const ImVec2 boxMin(pos.x, pos.y + (size.y - boxSide) * 0.5f);
            const ImVec2 boxMax(boxMin.x + boxSide, boxMin.y + boxSide);

            bool hovered = false, held = false;
            const bool clicked = enabled &&
                hitButton("##cb", pos, size, &hovered, &held);

            dl->AddRectFilled(boxMin, boxMax, enabled ? kWindow : kFace);
            dl->AddRect(boxMin, boxMax, hovered ? kBorderHot : kFieldEdge);

            if (orbiter_GetCheckState(h) == BST_CHECKED) {
                // The check mark, drawn as two strokes like the Windows glyph.
                const ImVec2 a(boxMin.x + 3, boxMin.y + 6);
                const ImVec2 b(boxMin.x + 5, boxMin.y + 9);
                const ImVec2 c(boxMin.x + 10, boxMin.y + 3);
                dl->AddLine(a, b, labelColour, 1.6f);
                dl->AddLine(b, c, labelColour, 1.6f);
            }

            drawLabel(dl, ImVec2(boxMax.x + 4, pos.y),
                      ImVec2(posMax.x, posMax.y), label, labelColour, 0);

            if (clicked) {
                orbiter_SetCheckState(h,
                    orbiter_GetCheckState(h) == BST_CHECKED ? BST_UNCHECKED
                                                            : BST_CHECKED);
                orbiter_NotifyCommand(h, BN_CLICKED);
            }

        } else if (kind == BS_AUTORADIOBUTTON || kind == BS_RADIOBUTTON) {
            const float r = 6.0f;
            const ImVec2 centre(pos.x + r, pos.y + size.y * 0.5f);

            bool hovered = false, held = false;
            const bool clicked = enabled &&
                hitButton("##rb", pos, size, &hovered, &held);

            dl->AddCircleFilled(centre, r, enabled ? kWindow : kFace);
            dl->AddCircle(centre, r, hovered ? kBorderHot : kFieldEdge);
            if (orbiter_GetCheckState(h) == BST_CHECKED)
                dl->AddCircleFilled(centre, r - 3.0f, labelColour);

            drawLabel(dl, ImVec2(centre.x + r + 4, pos.y),
                      ImVec2(posMax.x, posMax.y), label, labelColour, 0);

            if (clicked) {
                // NOT SetCheckState. An auto-radio must also CLEAR its group
                // siblings, and the core never does it itself -- see the
                // block at orbiter_CheckRadioButton in Win32Dlg.cpp.
                orbiter_CheckRadioButton(h);
                orbiter_NotifyCommand(h, BN_CLICKED);
            }

        } else if (kind == BS_GROUPBOX) {
            // An etched frame whose top edge is broken for the caption.
            int dummy = 0;
            const std::string cap = stripMnemonic(label, &dummy);
            const ImVec2 capSize = ImGui::CalcTextSize(cap.c_str());
            const float top = pos.y + capSize.y * 0.5f;

            // Two offset rectangles give the classic etched look.
            dl->AddRect(ImVec2(pos.x, top), ImVec2(posMax.x, posMax.y), kEtchDark);
            dl->AddRect(ImVec2(pos.x + 1, top + 1),
                        ImVec2(posMax.x + 1, posMax.y + 1), kEtchLight);

            if (!cap.empty()) {
                // Punch a gap in the frame behind the caption.
                dl->AddRectFilled(ImVec2(pos.x + 7, pos.y),
                                  ImVec2(pos.x + 11 + capSize.x, pos.y + capSize.y),
                                  kDialogBk);
                dl->AddText(ImVec2(pos.x + 9, pos.y), labelColour, cap.c_str());
            }

        } else {
            // Push button, including the default button.
            bool hovered = false, held = false;
            const bool clicked = enabled &&
                hitButton("##btn", pos, size, &hovered, &held);

            const ImU32 face = !enabled ? kFace
                             : held     ? kFacePushed
                             : hovered  ? kFaceHot : kFace;
            dl->AddRectFilled(pos, posMax, face);
            dl->AddRect(pos, posMax,
                        (hovered || (style & BS_DEFPUSHBUTTON)) ? kBorderHot
                                                                : kBorder);
            drawButtonLabel(dl, pos, posMax, label, labelColour, style);

            if (clicked) orbiter_NotifyCommand(h, BN_CLICKED);
        }

    } else if (cname == "Static" || cname == "STATIC") {
        const unsigned kind = style & 0x1F;

        if (kind == SS_BITMAP) {
            // Windows draws an SS_BITMAP static itself, from the resource its
            // template names -- no WM_DRAWITEM is sent for it. IDC_LOGO is
            // this case: Launchpad.cpp's owner-draw handler covers only
            // IDC_SHADOW and IDC_MNU_PAGECONTAINER, so nothing else would
            // paint the banner.
            const int resId = orbiter_GetBitmapId(h);
            int bw = 0, bh = 0;
            const unsigned long long tex =
                resId ? orbiter_BitmapTexture(resId, &bw, &bh) : 0;
            if (tex) {
                // The control rectangle wins over the image's own size, which
                // is what a static does when the two differ.
                dl->AddImage((ImTextureID)tex, pos, posMax);
            }

        } else if (kind == SS_OWNERDRAW) {
            // Painted by the dialog procedure. Send WM_DRAWITEM/WM_PAINT and
            // replay whatever GDI commands it recorded into this frame.
            orbiter_SendPaint(h);
            if (HDC dc = orbiter_GetPaintDC(h)) {
                orbiter_ReplayDC(dc, pos.x, pos.y);
                orbiter_ResetDC(dc);
            }
        } else if (kind == SS_BLACKRECT) {
            dl->AddRectFilled(pos, posMax, IM_COL32(0, 0, 0, 255));
        } else if (kind == SS_ETCHEDVERT) {
            dl->AddLine(pos, ImVec2(pos.x, posMax.y), kEtchDark);
            dl->AddLine(ImVec2(pos.x + 1, pos.y),
                        ImVec2(pos.x + 1, posMax.y), kEtchLight);
        } else if (!label.empty()) {
            // WM_CTLCOLORSTATIC lets the parent recolour a static before it is
            // drawn. Launchpad.cpp uses it for IDC_BLACKBOX: it sets white-ish
            // text on a black background and returns BLACK_BRUSH, which is the
            // only reason that panel is black -- the template gives it no
            // special style. Without asking, it would render as ordinary text
            // on the dialog face.
            unsigned fg = 0, bk = 0;
            int opaque = 0;
            const int recoloured =
                orbiter_QueryCtlColor(h, &fg, &bk, &opaque);

            if (recoloured && opaque) {
                dl->AddRectFilled(pos, posMax,
                                  IM_COL32(bk & 0xFF, (bk >> 8) & 0xFF,
                                           (bk >> 16) & 0xFF, 255));
            }
            const ImU32 col = recoloured
                ? IM_COL32(fg & 0xFF, (fg >> 8) & 0xFF, (fg >> 16) & 0xFF, 255)
                : labelColour;

            // Static text honours SS_CENTER/SS_RIGHT and is top-aligned,
            // unlike a button caption. Clipped to its rectangle, as a Win32
            // static is.
            ImGui::PushClipRect(pos, posMax, true);
            drawLabel(dl, pos, posMax, label, col, style, false);
            ImGui::PopClipRect();
        }

    // RICHEDIT joins Edit and HtmlCtrl here, and it had NO BRANCH AT ALL --
    // its three controls fell through to the pink "unimplemented" outline at
    // the bottom of this chain.
    //
    // It is a rich edit in name only. All three live uses are
    // ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, which is a
    // read-only multi-line plain-text box, and NOTHING IN THE TREE EVER
    // FORMATS THEM: there is not one EM_SETCHARFORMAT, EM_STREAMIN or
    // CHARFORMAT in any .cpp that this platform builds. The data views are
    // driven with plain SetWindowTextA --
    //
    //     SetWindowTextA(GetDlgItem(hDataWnd, IDC_DBG_DATAVIEW), buffer.c_str());
    //                                             DebugControls.cpp:2020
    //
    // -- so the Edit path renders exactly what Windows shows. Implementing a
    // rich-text engine for controls that are never sent rich text would be
    // inventing behaviour rather than porting it, which is the same call, for
    // the same reason, that HtmlCtrl records below.
    } else if (cname == "Edit" || cname == "HtmlCtrl" || cname == "RICHEDIT") {
        dl->AddRectFilled(pos, posMax, enabled ? kWindow : kFace);
        dl->AddRect(pos, posMax, kFieldEdge);

        // HtmlCtrl is Orbiter's scenario-description pane. On Windows it hosts
        // an Internet Explorer OLE object; Linux/HtmlCtrl.cpp replaces that
        // with a markup-to-text converter that calls SetWindowText, so what
        // the control holds is plain wrapped text -- the same shape as a
        // read-only multi-line edit, and drawn by the same path.
        if ((style & ES_MULTILINE) || cname == "HtmlCtrl") {
            // A multi-line edit. The description panes are these:
            // IDC_EXT_TEXT and IDC_SCN_DESC are both
            // ES_MULTILINE | ES_READONLY | WS_VSCROLL, holding paragraphs that
            // Orbiter sets with SetWindowText. Drawing them through the
            // single-line path shows only the first line, clipped at the
            // control's edge -- which is what they looked like.

            // WS_VSCROLL: a scrollbar down the right edge, and a wheel that
            // scrolls. Windows gives the control one because the text
            // routinely overflows -- a scenario description is several
            // paragraphs -- and without it everything past the last visible
            // line was simply unreachable.
            const bool wantBar = (style & WS_VSCROLL) != 0;
            const float barW   = wantBar ? 15.0f : 0.0f;   // Win32 SM_CXVSCROLL
            const float textW  = size.x - 8.0f - barW;

            // How tall the wrapped text actually is, which is what decides
            // whether the bar is live and how far it can travel.
            float textH = 0.0f;
            if (!label.empty() && textW > 0.0f) {
                textH = ImGui::GetFont()->CalcTextSizeA(
                            ImGui::GetFontSize(), FLT_MAX, textW,
                            label.c_str()).y;
            }
            const float viewH  = size.y - 6.0f;
            const float maxOff = (textH > viewH) ? (textH - viewH) : 0.0f;

            // The offset lives per control, keyed by handle, so each pane
            // keeps its own position across frames.
            static std::map<HWND, float> scrollOff;
            float &off = scrollOff[h];
            if (off > maxOff) off = maxOff;
            if (off < 0.0f)   off = 0.0f;

            // Wheel over the control scrolls it, as it does on Windows.
            if (maxOff > 0.0f && ImGui::IsMouseHoveringRect(pos, posMax)) {
                const float wheel = ImGui::GetIO().MouseWheel;
                if (wheel != 0.0f) {
                    off -= wheel * ImGui::GetTextLineHeight() * 3.0f;
                    if (off > maxOff) off = maxOff;
                    if (off < 0.0f)   off = 0.0f;
                }
            }

            ImGui::PushClipRect(ImVec2(pos.x + 1, pos.y + 1),
                                ImVec2(posMax.x - 1 - barW, posMax.y - 1), true);

            if (!label.empty() && textW > 0.0f) {
                dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                            ImVec2(pos.x + 4, pos.y + 3 - off), labelColour,
                            label.c_str(), nullptr, textW);
            }
            ImGui::PopClipRect();

            if (wantBar) {
                // Drawn in the Win32 shape: a channel with a proportional
                // thumb. The thumb fills the channel when there is nothing to
                // scroll, which is how a disabled scrollbar looks.
                const ImVec2 barTL(posMax.x - barW, pos.y + 1);
                const ImVec2 barBR(posMax.x - 1,    posMax.y - 1);
                dl->AddRectFilled(barTL, barBR, kScrollTrack);

                const float chanH = barBR.y - barTL.y;
                float thumbH = chanH;
                float thumbY = barTL.y;
                if (maxOff > 0.0f && textH > 0.0f) {
                    thumbH = chanH * (viewH / textH);
                    if (thumbH < 12.0f) thumbH = 12.0f;
                    thumbY = barTL.y + (chanH - thumbH) * (off / maxOff);
                }
                dl->AddRectFilled(ImVec2(barTL.x + 1, thumbY),
                                  ImVec2(barBR.x - 1, thumbY + thumbH),
                                  kScrollThumb);
                dl->AddRect(barTL, barBR, kFieldEdge);
            }

            // Read-only panes take no input; an editable one would need a
            // caret and scrolling, which nothing in the Launchpad uses.
            if (!(style & ES_READONLY) && cname != "HtmlCtrl") {
                ImGui::SetCursorScreenPos(pos);
                ImGui::InvisibleButton("##medit", size);
            }

        } else {
            // Input is handled by an ImGui field with all its own decoration
            // removed, so what is visible is the frame drawn above.
            char buf[512];
            snprintf(buf, sizeof(buf), "%s", label.c_str());

            // ES_CENTER / ES_RIGHT -- an edit field's text alignment, which
            // was never read, so every field in the tree drew left-aligned.
            //
            // 85 fields declare an alignment; the atmospheric controls dialog
            // is 20-odd of them --
            //     EDITTEXT IDC_ATD_S1, 15,255,30,12,
            //              ES_CENTER | ES_AUTOHSCROLL | ES_READONLY
            // -- and they are centred on Windows.
            //
            // ES_LEFT/CENTER/RIGHT are 0/1/2 and SS_LEFT/CENTER/RIGHT are the
            // same three values, so the mask is the whole conversion.
            //
            // DONE BY MOVING THE FIELD, not by padding it. ImGui's InputText
            // draws its own text hard against the left of its frame and has
            // no alignment of its own; FramePadding can only pad both sides,
            // which centres but cannot right-align. Narrowing the frame and
            // shifting it does both, and the frame is invisible anyway
            // (ImGuiCol_FrameBg is pushed to 0 below), so the only cost is
            // that the clickable area shrinks to the text.
            //
            // NOT restricted to read-only fields: 25 of the 85 are editable,
            // so a read-only-only fix would have left a quarter of them
            // wrong.
            float eoff = 0.0f, ewidth = size.x - 6.0f;
            {
                const unsigned ealign = style & 0x3;
                if (ealign != ES_LEFT && !label.empty()) {
                    const float textW = ImGui::GetFont()->CalcTextSizeA(
                        ImGui::GetFontSize(), FLT_MAX, 0.0f, label.c_str()).x;
                    const float slack = ewidth - textW;
                    if (slack > 0.0f) {
                        eoff = (ealign == ES_CENTER) ? slack * 0.5f : slack;
                        ewidth -= eoff;
                    }
                }
            }

            ImGui::SetCursorScreenPos(ImVec2(pos.x + 3 + eoff, pos.y + 2));
            ImGui::SetNextItemWidth(ewidth);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, 0);
            ImGui::PushStyleColor(ImGuiCol_Text, labelColour);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));

            // A DISABLED EDIT MUST NOT ACCEPT TYPING. EnableWindow(FALSE) was
            // greying the text -- labelColour above -- and changing nothing
            // else, so a field Windows would have locked was still editable
            // here. The Video tab is where that shows: it disables the window
            // width and height in fullscreen, and typing into them then wrote
            // values that were read straight back out by UpdateConfigData.
            //
            // ES_READONLY gets the same treatment, for the same reason: it is
            // the style bit that says exactly this.
            ImGuiInputTextFlags eflags = 0;
            if (!enabled || (style & ES_READONLY))
                eflags |= ImGuiInputTextFlags_ReadOnly;

            if (ImGui::InputText("##edit", buf, sizeof(buf), eflags)) {
                orbiter_SetText(h, buf);
                orbiter_NotifyCommand(h, EN_CHANGE);
            }
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(2);
        }

    } else if (cname == "ComboBox") {
        const int count = orbiter_GetItemCount(h);
        const int sel   = orbiter_GetCurSel(h);
        // A COMBO'S TOP TWO STYLE BITS DECIDE WHETHER IT HAS AN EDIT FIELD.
        //
        //     CBS_SIMPLE       1   edit field, list always shown
        //     CBS_DROPDOWN     2   edit field, list on the arrow
        //     CBS_DROPDOWNLIST 3   NO edit field -- a picker, and nothing else
        //
        // Every combo here was drawn as the third kind: a static preview of
        // the selected item with no way to type. Twenty-one of the templates'
        // combos are CBS_DROPDOWN, and for some of them the typed text is the
        // whole point. ScnEditor's IDC_REF is the clear case -- Landed,
        // Statevec and Elements each listen for
        //
        //     if (HIWORD (wParam) == CBN_SELCHANGE ||
        //         HIWORD (wParam) == CBN_EDITCHANGE)     Editor.cpp:1622,1832,2128
        //
        // and answer by re-reading the field with GetWindowText and resolving
        // it with oapiGetGbodyByName. CBN_EDITCHANGE could not be sent by a
        // control with no edit field, so the second half of every one of those
        // tests was dead: a reference body could only be chosen from the list,
        // never named.
        //
        // CBS_SIMPLE shares the field but shows its list permanently and has
        // no arrow. No template in the tree uses it, so it is treated as
        // CBS_DROPDOWN here rather than growing a second layout for a case
        // that never occurs.
        const unsigned cbKind  = style & 0x3;
        const bool     editable = (cbKind == CBS_SIMPLE) ||
                                  (cbKind == CBS_DROPDOWN);

        // A combo's template height covers its dropped-down list; the closed
        // control is one line tall, which is what Windows shows.
        const float lineH = ImGui::GetTextLineHeight() + 6.0f;
        const ImVec2 boxMax(posMax.x, pos.y + lineH);

        dl->AddRectFilled(pos, boxMax, enabled ? kWindow : kFace);
        dl->AddRect(pos, boxMax, kFieldEdge);

        // What the closed control shows. For a picker that is the selected
        // item; for an editable one it is the edit field's own text, which
        // after typing names no item at all -- so it is read from the window
        // text, where CB_SETCURSEL and the popup below both put it.
        const char *preview = editable
                            ? label.c_str()
                            : ((sel >= 0 && sel < count)
                                   ? orbiter_GetItemText(h, sel) : "");
        if (preview && !editable)
            drawLabel(dl, ImVec2(pos.x + 4, pos.y), boxMax,
                      preview, labelColour, 0);

        // The drop-down arrow button on the right edge.
        const float arrowW = 17.0f;
        const ImVec2 arrMin(boxMax.x - arrowW, pos.y + 1);
        const ImVec2 arrMax(boxMax.x - 1, boxMax.y - 1);
        dl->AddRectFilled(arrMin, arrMax, kFace);
        const float ax = (arrMin.x + arrMax.x) * 0.5f;
        const float ay = (arrMin.y + arrMax.y) * 0.5f;
        dl->AddTriangleFilled(ImVec2(ax - 3, ay - 2), ImVec2(ax + 3, ay - 2),
                              ImVec2(ax, ay + 2), labelColour);

        // Opening and picking are driven by a plain invisible button over the
        // closed control, and the list by a popup of the same width.
        //
        // ImGui::BeginCombo cannot be used here even with NoPreview: that flag
        // suppresses the preview *field* but still draws ImGui's own arrow
        // button, and it lands at the cursor position -- the LEFT edge of the
        // control, next to the text. The result is two drop-down arrows on
        // every combo, one at each end. (NoPreview|NoArrowButton together
        // assert, since nothing would remain clickable.)
        //
        // The hit region differs by kind, as it does on Windows. A
        // CBS_DROPDOWNLIST is one button end to end -- clicking anywhere in it
        // drops the list. A CBS_DROPDOWN's text area belongs to its edit
        // field, so only the ARROW opens the list; a click on the text places
        // the caret. Leaving the button over the whole control would swallow
        // every click before the field could see one, and the field would
        // never take focus.
        if (editable) {
            // The same field the Edit branch above uses -- an ImGui InputText
            // with its own frame and padding removed, so what shows is the
            // box already drawn here. Reusing it keeps one text-entry
            // mechanism in this file rather than a second one for combos.
            char buf[512];
            snprintf(buf, sizeof(buf), "%s", label.c_str());

            ImGui::SetCursorScreenPos(ImVec2(pos.x + 3, pos.y + 3));
            ImGui::SetNextItemWidth(size.x - arrowW - 6);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, 0);
            ImGui::PushStyleColor(ImGuiCol_Text, labelColour);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));

            // EnableWindow(FALSE) locks the field, exactly as it does for an
            // edit control -- greying the text and still accepting typing is
            // the defect the Video tab exposed.
            const ImGuiInputTextFlags eflags =
                enabled ? 0 : ImGuiInputTextFlags_ReadOnly;

            if (ImGui::InputText("##cbedit", buf, sizeof(buf), eflags)) {
                orbiter_SetComboEditText(h, buf);
                orbiter_NotifyCommand(h, CBN_EDITCHANGE);
            }
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(2);
        }

        ImGui::SetCursorScreenPos(editable ? ImVec2(arrMin.x, pos.y) : pos);
        const bool opened = ImGui::InvisibleButton("##combo",
                                editable ? ImVec2(arrowW, lineH)
                                         : ImVec2(size.x, lineH));

        char popupId[32];
        snprintf(popupId, sizeof(popupId), "##combopop%d", id);
        if (opened && enabled) ImGui::OpenPopup(popupId);

        ImGui::SetNextWindowPos(ImVec2(pos.x, boxMax.y));
        ImGui::SetNextWindowSizeConstraints(ImVec2(size.x, 0),
                                            ImVec2(size.x, FLT_MAX));
        ImGui::PushStyleColor(ImGuiCol_PopupBg, kWindow);
        ImGui::PushStyleColor(ImGuiCol_Text, kText);
        if (ImGui::BeginPopup(popupId)) {
            for (int i = 0; i < count; ++i) {
                const char *item = orbiter_GetItemText(h, i);
                if (!item) continue;
                // Typing leaves no item selected (CB_GETCURSEL answers
                // CB_ERR), so an editable combo's list is highlighted by
                // matching the field instead -- which is what dropping the
                // list after typing shows on Windows.
                const bool chosen = (i == sel) ||
                                    (editable && sel < 0 && label == item);
                if (ImGui::Selectable(item, chosen)) {
                    orbiter_SetCurSel(h, i);
                    orbiter_NotifyCommand(h, CBN_SELCHANGE);
                }
            }
            ImGui::EndPopup();
        }
        ImGui::PopStyleColor(2);

    } else if (cname == "ListBox") {
        // A list box row is one CHARACTER HEIGHT, which is the vertical
        // dialog base unit -- 13px for the 8pt MS Shell Dlg the templates
        // specify. It is not ImGui's text line height, which is derived from
        // the font atlas and is about 15px here; using that makes every row
        // taller than Windows and throws off the integral-height trim below.
        const float rowH = 13.0f;

        // Integral height.
        //
        // A Win32 list box created WITHOUT LBS_NOINTEGRALHEIGHT is resized at
        // creation so its client area holds a whole number of rows -- it never
        // shows a partial one. That trim is not cosmetic here; the About page
        // depends on it. From IDD_PAGE_ABT, in dialog units:
        //
        //     Button  "Installed components" 1713  y=63 cy=50  -> 63..113
        //     ListBox                        1714  y=75 cy=40  -> 75..115
        //
        // The list box bottom is 2 units BELOW the group box that frames it.
        // Windows hides that by trimming 40 units of height down to three
        // rows; drawing the template height verbatim punches the list box
        // straight through the group frame, which is exactly what it did.
        ImVec2 lbMax = posMax;
        if (!(style & LBS_NOINTEGRALHEIGHT)) {
            const float inner = size.y - 2.0f;              // less the border
            const int   rows  = (int)(inner / rowH);
            if (rows > 0)
                lbMax.y = pos.y + (float)rows * rowH + 2.0f;
        }

        const int count = orbiter_GetItemCount(h);
        const int sel   = orbiter_GetCurSel(h);
        const bool multi = orbiter_IsMultiSel(h) != 0;
        const float lineH = rowH;

        // WS_VSCROLL, AND IT IS NOT DECORATION.
        //
        // ALL SEVEN list boxes in the templates carry WS_VSCROLL, and the
        // renderer had no scrolling at all: the row loop simply `break`ed at
        // the bottom edge and everything below it was unreachable. That is
        // silent -- a short list and a truncated one look identical.
        //
        // It is live. OptionsPage_Planetarium::RescanMarkerList fills
        // IDC_OPT_PLN_MKRLIST from the marker files of the selected body, and
        // Config/Venus/Marker holds 23 of them against roughly thirteen
        // visible rows. The list is not a display: LB_SETSEL/LB_GETSEL are how
        // each marker set is toggled on and read back, so the hidden ten could
        // not be switched on at all.
        //
        // Modelled on the multi-line edit above, which already does exactly
        // this -- same channel, same proportional thumb, same wheel step --
        // rather than inventing a second scrolling idiom in one file.
        const float viewH  = (lbMax.y - pos.y) - 4.0f;
        const int   rowsFit = (viewH > 0.0f) ? (int)(viewH / lineH) : 0;
        const bool  wantBar = (style & WS_VSCROLL) && (count > rowsFit);
        const float barW    = wantBar ? 15.0f : 0.0f;   // Win32 SM_CXVSCROLL

        // The offset is in ROWS, not pixels: a list box scrolls by whole
        // items, which is why Windows never shows a half row at the top.
        static std::map<HWND, int> lbTop;
        int &top = lbTop[h];
        const int maxTop = (count > rowsFit) ? (count - rowsFit) : 0;
        if (top > maxTop) top = maxTop;
        if (top < 0)      top = 0;

        if (wantBar && ImGui::IsMouseHoveringRect(pos, lbMax)) {
            const float wheel = ImGui::GetIO().MouseWheel;
            if (wheel != 0.0f) {
                top -= (int)wheel * 3;          // three rows, as Windows does
                if (top > maxTop) top = maxTop;
                if (top < 0)      top = 0;
            }
        }

        dl->AddRectFilled(pos, lbMax, enabled ? kWindow : kFace);
        dl->AddRect(pos, lbMax, kFieldEdge);

        ImGui::PushClipRect(ImVec2(pos.x + 1, pos.y + 1),
                            ImVec2(lbMax.x - 1 - barW, lbMax.y - 1), true);

        for (int i = top; i < count; ++i) {
            const char *item = orbiter_GetItemText(h, i);
            if (!item) continue;
            const float ly = pos.y + 2 + (i - top) * lineH;
            if (ly + lineH > lbMax.y) break;   // clipped by the frame

            // A multi-selection list marks each selected item independently;
            // the Planetarium marker list and Labels feature list are these.
            const bool isSel = multi ? (orbiter_GetItemSel(h, i) != 0)
                                     : (i == sel);
            if (isSel)
                dl->AddRectFilled(ImVec2(pos.x + 1, ly),
                                  ImVec2(lbMax.x - 1 - barW, ly + lineH),
                                  IM_COL32(0, 120, 215, 255));
            dl->AddText(ImVec2(pos.x + 3, ly),
                        isSel ? IM_COL32(255, 255, 255, 255) : labelColour,
                        item);
        }

        ImGui::PopClipRect();

        if (wantBar) {
            const ImVec2 barTL(lbMax.x - barW, pos.y + 1);
            const ImVec2 barBR(lbMax.x - 1,    lbMax.y - 1);
            dl->AddRectFilled(barTL, barBR, kScrollTrack);

            const float chanH  = barBR.y - barTL.y;
            float thumbH = chanH * ((float)rowsFit / (float)count);
            if (thumbH < 12.0f) thumbH = 12.0f;
            const float thumbY = barTL.y + (chanH - thumbH) *
                                 ((maxTop > 0) ? ((float)top / (float)maxTop) : 0.0f);
            dl->AddRectFilled(ImVec2(barTL.x + 1, thumbY),
                              ImVec2(barBR.x - 1, thumbY + thumbH),
                              kScrollThumb);
            dl->AddRect(barTL, barBR, kFieldEdge);
        }

        // The hit area STOPS AT THE SCROLLBAR. Running it the full width would
        // make a click on the bar select whatever row is behind it.
        ImGui::SetCursorScreenPos(pos);
        if (ImGui::InvisibleButton("##lb", ImVec2(size.x - barW, lbMax.y - pos.y)) &&
            enabled) {
            const float rel = ImGui::GetIO().MousePos.y - pos.y - 2;
            const int idx = top + (int)(rel / lineH);
            if (idx >= 0 && idx < count) {
                // A click toggles in a multi-selection list and replaces the
                // selection in a single-selection one.
                if (multi) orbiter_ToggleItemSel(h, idx);
                else       orbiter_SetCurSel(h, idx);
                orbiter_NotifyCommand(h, LBN_SELCHANGE);

                // AND LBN_DBLCLK, which this never sent.
                //
                // Found by set-difference rather than by reading: every
                // notification the Scenario Editor listens for, diffed against
                // the ones this file posts. It listens for this one twice --
                // EditorTab_Statevec and EditorTab_Landed both have
                //
                //     case LBN_DBLCLK: Refresh (hV); Apply (); break;
                //
                // on IDC_STATECPY, the "Copy state from" list. Single click
                // previews another vessel's state, double click applies it,
                // and the second half simply did not exist here.
                //
                // Windows sends BOTH notifications for the second click of a
                // double click (LBN_SELCHANGE then LBN_DBLCLK), which is why
                // this is posted after the one above rather than instead of
                // it -- the handler above expects to have run.
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                    orbiter_NotifyCommand(h, LBN_DBLCLK);
            }
        }

        // SAID ONCE PER LIST THAT OVERFLOWS, because the defect this replaces
        // was invisible by construction and the fix has the same shape: if the
        // scroll ever stops working, a truncated list looks exactly like a
        // short one again.
        if (count > rowsFit) {
            static std::map<HWND, bool> said;
            if (!said[h]) {
                said[h] = true;
                if (getenv("ORBITER_TRACE_MSG"))
                    fprintf(stderr, "[msg] listbox id=%d: %d items, %d rows "
                                    "visible -- scrollbar %s\n",
                            id, count, rowsFit,
                            (style & WS_VSCROLL) ? "on" : "ABSENT (no WS_VSCROLL)");
            }
        }

    } else if (cname == "ScrollBar" || cname == "SCROLLBAR") {
        // A standalone vertical scrollbar. The options container uses one to
        // scroll a page taller than its pane, driving it entirely through
        // WM_VSCROLL and GetScrollInfo(SB_CTL).
        int lo = 0, hi = 0, val = 0, page = 0;
        orbiter_GetScrollState(h, &lo, &hi, &val, &page);

        dl->AddRectFilled(pos, posMax, IM_COL32(240, 240, 240, 255));

        const float arrowH = 16.0f;
        const ImVec2 upMax(posMax.x, pos.y + arrowH);
        const ImVec2 dnMin(pos.x, posMax.y - arrowH);

        // Arrow buttons at each end.
        ImGui::SetCursorScreenPos(pos);
        const bool upHit = ImGui::InvisibleButton("##sbup",
                                                  ImVec2(size.x, arrowH));
        ImGui::SetCursorScreenPos(dnMin);
        const bool dnHit = ImGui::InvisibleButton("##sbdn",
                                                  ImVec2(size.x, arrowH));

        const float cx = (pos.x + posMax.x) * 0.5f;
        dl->AddTriangleFilled(ImVec2(cx - 4, pos.y + 10),
                              ImVec2(cx + 4, pos.y + 10),
                              ImVec2(cx, pos.y + 5),
                              IM_COL32(80, 80, 80, 255));
        dl->AddTriangleFilled(ImVec2(cx - 4, posMax.y - 10),
                              ImVec2(cx + 4, posMax.y - 10),
                              ImVec2(cx, posMax.y - 5),
                              IM_COL32(80, 80, 80, 255));

        // The thumb. Its length reflects the visible fraction, as Windows
        // draws it, so a long page gets a short thumb.
        const float trackTop = pos.y + arrowH;
        const float trackH   = (dnMin.y - trackTop);
        if (trackH > 0 && hi > lo) {
            const float frac  = page > 0
                              ? std::min(1.0f, (float)page / (float)(hi - lo))
                              : 0.25f;
            const float thumbH = std::max(16.0f, trackH * frac);
            const int   travel = std::max(1, (hi - lo) - page);
            const float t = (float)(val - lo) / (float)travel;
            const float ty = trackTop + (trackH - thumbH) * std::min(1.0f, t);

            ImGui::SetCursorScreenPos(ImVec2(pos.x, ty));
            ImGui::InvisibleButton("##sbthumb", ImVec2(size.x, thumbH));
            const bool dragging = ImGui::IsItemActive();

            dl->AddRectFilled(ImVec2(pos.x + 1, ty),
                              ImVec2(posMax.x - 1, ty + thumbH),
                              dragging ? IM_COL32(120, 120, 120, 255)
                                       : IM_COL32(205, 205, 205, 255));

            if (dragging && trackH > thumbH) {
                const float rel = (ImGui::GetIO().MousePos.y - trackTop
                                   - thumbH * 0.5f) / (trackH - thumbH);
                const int np = lo + (int)(std::clamp(rel, 0.0f, 1.0f) * travel);
                orbiter_ScrollBarNotify(h, SB_THUMBTRACK, np);
            }
        }

        if (upHit) orbiter_ScrollBarNotify(h, SB_LINEUP, val);
        if (dnHit) orbiter_ScrollBarNotify(h, SB_LINEDOWN, val);

    } else if (cname == "msctls_trackbar32") {
        // A slider: groove down the long axis, thumb at the current position.
        // Dragging reports through WM_HSCROLL, which the options pages
        // dispatch to OnHScroll.
        //
        // TBS_VERT WAS NOT HANDLED, and a vertical trackbar is not a rare
        // case: the atmospheric controls dialog is twenty-odd of them --
        //
        //     CONTROL "", IDC_ATM_S16, TRACKBAR_CLASS,
        //             WS_TABSTOP | TBS_VERT | TBS_BOTH | TBS_NOTICKS,
        //             612, 50, 15, 200
        //
        // -- fifteen dialog units wide and two hundred tall. Drawn by the
        // horizontal code that stood here, the groove ran from pos.x+6 to
        // posMax.x-6, which on a control that narrow is DEGENERATE (it showed
        // as a two-pixel tick halfway down), and the thumb spanned
        // pos.y+2..posMax.y-2 -- the control's WHOLE HEIGHT. So every slider
        // rendered as a tall empty box with a speck beside it, and none of
        // them showed its value.
        //
        // MINIMUM AT THE TOP, which is the Win32 convention and is confirmed
        // by the caller rather than assumed: AtmoControls.cpp inverts in both
        // directions --
        //
        //     double x = (1000.0 - double(pos)) / 1000.0;      // read
        //     DWORD dpos = 1000 - DWORD(x * 1000.0);           // write
        //
        // -- which is only necessary if position increases DOWNWARD and the
        // author wants a high value to sit high. Drawing it the other way up
        // would put every slider in the dialog at the wrong end and still
        // look plausible.
        int lo = 0, hi = 0, val = 0;
        orbiter_GetRange(h, &lo, &hi, &val);
        const int span = (hi > lo) ? (hi - lo) : 1;
        const float frac = (float)(val - lo) / (float)span;

        const bool vertical  = (style & TBS_VERT) != 0;
        const float grooveT  = 2.0f;
        const float travel   = (vertical ? size.y : size.x) - 12.0f;

        // The groove, along whichever axis is the long one.
        ImVec2 gMin, gMax;
        if (vertical) {
            const float midX = pos.x + size.x * 0.5f;
            gMin = ImVec2(midX - grooveT, pos.y + 6);
            gMax = ImVec2(midX + grooveT, posMax.y - 6);
        } else {
            const float midY = pos.y + size.y * 0.5f;
            gMin = ImVec2(pos.x + 6, midY - grooveT);
            gMax = ImVec2(posMax.x - 6, midY + grooveT);
        }
        dl->AddRectFilled(gMin, gMax, IM_COL32(200, 200, 200, 255));
        dl->AddRect(gMin, gMax, kFieldEdge);

        // TICK MARKS -- TBS_AUTOTICKS, and where they go.
        //
        // The bar draws one tick per TBM_SETTICFREQ unit along the travel,
        // including both ends. TBS_NOTICKS suppresses them; TBS_BOTH puts a
        // row on each side of the groove; TBS_TOP (== TBS_LEFT) moves the
        // single row to the top of a horizontal bar or the left of a vertical
        // one, and the default is the opposite side.
        //
        // THE FREQUENCY IS WHY TBM_SETTICFREQ IS NO LONGER DISCARDED. The
        // live bars set it deliberately: IDC_CONVERGENCE is 5..100 by 5 and
        // IDC_LODBIAS is -10..10 by 1, which are 20 and 21 ticks. Ticking
        // every unit instead -- the Win32 default this port would otherwise
        // have used -- would put 96 marks on the first one.
        if ((style & TBS_AUTOTICKS) && !(style & TBS_NOTICKS) && span > 0) {
            const int freq = std::max(1, orbiter_GetTicFreq(h));
            const ImU32 tickCol = IM_COL32(96, 96, 96, 255);
            const float len = 3.0f;
            const bool both = (style & TBS_BOTH) != 0;
            const bool topSide = (style & TBS_TOP) != 0;

            // Guard against a range so wide that ticking it would draw a
            // solid band: past one tick every two pixels there is nothing to
            // read, and Windows' own bar is illegible there too.
            const int nticks = span / freq;
            if (nticks > 0 && (float)nticks <= travel * 0.5f) {
                for (int i = 0; i <= nticks; ++i) {
                    const float f = (float)(i * freq) / (float)span;
                    if (vertical) {
                        const float ty = pos.y + 6 + travel * f;
                        if (both || topSide)
                            dl->AddLine(ImVec2(gMin.x - 2 - len, ty),
                                        ImVec2(gMin.x - 2, ty), tickCol);
                        if (both || !topSide)
                            dl->AddLine(ImVec2(gMax.x + 2, ty),
                                        ImVec2(gMax.x + 2 + len, ty), tickCol);
                    } else {
                        const float tx = pos.x + 6 + travel * f;
                        if (both || topSide)
                            dl->AddLine(ImVec2(tx, gMin.y - 2 - len),
                                        ImVec2(tx, gMin.y - 2), tickCol);
                        if (both || !topSide)
                            dl->AddLine(ImVec2(tx, gMax.y + 2),
                                        ImVec2(tx, gMax.y + 2 + len), tickCol);
                    }
                }
            }
        }

        ImGui::SetCursorScreenPos(pos);
        const bool active = ImGui::InvisibleButton("##track", size);
        const bool dragging = ImGui::IsItemActive();

        // The thumb: a slab ACROSS the groove, as the Windows control draws
        // it -- tall and narrow on a horizontal bar, wide and short on a
        // vertical one.
        const ImU32 thumb = !enabled ? IM_COL32(204,204,204,255)
                          : dragging ? kFacePushed
                          : ImGui::IsItemHovered() ? kFaceHot : kFace;
        ImVec2 tMin, tMax;
        if (vertical) {
            const float ty = pos.y + 6 + travel * frac;
            tMin = ImVec2(pos.x + 2, ty - 5);
            tMax = ImVec2(posMax.x - 2, ty + 5);
        } else {
            const float tx = pos.x + 6 + travel * frac;
            tMin = ImVec2(tx - 5, pos.y + 2);
            tMax = ImVec2(tx + 5, posMax.y - 2);
        }
        dl->AddRectFilled(tMin, tMax, thumb);
        dl->AddRect(tMin, tMax, kBorder);

        if ((dragging || active) && enabled && travel > 0.0f) {
            const ImVec2 m = ImGui::GetIO().MousePos;
            const float rel = vertical ? (m.y - pos.y - 6) / travel
                                       : (m.x - pos.x - 6) / travel;
            orbiter_SetTrackPos(h, lo + (int)(rel * span + 0.5f));
        }

    } else if (cname == "msctls_progress32") {
        // A progress bar: a sunken trough with a filled portion.
        //
        // LIVE ON THE LAUNCHPAD. IDC_PROGRESS1 on the wait page is driven by
        // LaunchpadDialog::UpdateWaitProgress while a scenario loads -- range
        // 0..1000, position tracking heap reclaimed. With no branch here it
        // fell through to the "unimplemented" outline at the bottom, so the
        // one piece of feedback the user gets during a slow load was a pink
        // rectangle.
        int lo = 0, hi = 0, val = 0;
        orbiter_GetRange(h, &lo, &hi, &val);
        const int span = (hi > lo) ? (hi - lo) : 1;
        const float frac = std::clamp((float)(val - lo) / (float)span, 0.0f, 1.0f);

        // The trough, drawn like the other sunken fields (kWindow inside a
        // kFieldEdge border, the same pair Edit and the list boxes use) so it
        // sits with them rather than inventing a third sunken look.
        dl->AddRectFilled(pos, posMax, kWindow);
        dl->AddRect(pos, posMax, kFieldEdge);

        // The bar. Inset by one pixel so the border stays visible, and only
        // drawn when there is something to show -- a zero-width filled rect
        // would still paint a sliver.
        const float w = (size.x - 2.0f) * frac;
        if (w >= 1.0f)
            dl->AddRectFilled(ImVec2(pos.x + 1, pos.y + 1),
                              ImVec2(pos.x + 1 + w, posMax.y - 1),
                              IM_COL32(6, 176, 37, 255));   // the Windows green

    } else if (cname == "msctls_updown32") {
        // A spin control: two stacked arrow buttons. It is usually attached to
        // an edit field as a buddy, but the pages read its position directly.
        int lo = 0, hi = 0, val = 0;
        orbiter_GetRange(h, &lo, &hi, &val);

        // UDS_HORZ: "Causes the control's arrows to point left and right
        // instead of up and down."  Same control, rotated -- and never tested
        // here, so the Scenario Editor's IDC_SPIN5 and IDC_SPIN6 drew as a
        // stack of up/down arrows inside a box wider than it is tall (29x12
        // dialog units). Exactly the TBS_VERT defect with the axes the other
        // way round.
        //
        // Those two are the only LIVE users. Orbiter.rc's IDC_MAP_ZOOM is
        // also UDS_HORZ but its dialog is dead template: Src/Orbiter/DlgMap.h
        // declares `class DlgMap : public ImGuiDialog`, so IDD_MAP is no
        // longer built from the .rc at all. Checked rather than assumed --
        // the audit counts template declarations, not live controls, and the
        // two are not the same thing.
        //
        // RIGHT IS "UP". That is the Win32 mapping for a horizontal up-down,
        // and it keeps the sign convention the vertical case already
        // established below rather than inventing a second one.
        const bool horz = (style & UDS_HORZ) != 0;

        const float halfH = horz ? size.x * 0.5f : size.y * 0.5f;
        // "up" is the top half, or the RIGHT half when horizontal.
        const ImVec2 upMin = horz ? ImVec2(pos.x + halfH, pos.y) : pos;
        const ImVec2 upMax = horz ? posMax : ImVec2(posMax.x, pos.y + halfH);
        const ImVec2 dnMin = pos;
        const ImVec2 dnMax = horz ? ImVec2(pos.x + halfH, posMax.y)
                                  : posMax;
        const ImVec2 upSize = horz ? ImVec2(halfH, size.y) : ImVec2(size.x, halfH);
        const ImVec2 dnSize = upSize;

        ImGui::SetCursorScreenPos(upMin);
        const bool upHit = ImGui::InvisibleButton("##up", upSize);
        const bool upHot = ImGui::IsItemHovered();
        ImGui::SetCursorScreenPos(dnMin);
        const bool dnHit = ImGui::InvisibleButton("##dn", dnSize);
        const bool dnHot = ImGui::IsItemHovered();

        dl->AddRectFilled(upMin, upMax, upHot ? kFaceHot : kFace);
        dl->AddRectFilled(dnMin, dnMax, dnHot ? kFaceHot : kFace);
        dl->AddRect(pos, posMax, kBorder);
        if (horz)
            dl->AddLine(ImVec2(pos.x + halfH, pos.y),
                        ImVec2(pos.x + halfH, posMax.y), kBorder);
        else
            dl->AddLine(ImVec2(pos.x, pos.y + halfH),
                        ImVec2(posMax.x, pos.y + halfH), kBorder);

        if (horz) {
            // Arrows pointing left and right, centred in their halves.
            const float cy = (pos.y + posMax.y) * 0.5f;
            const float lx = pos.x + halfH * 0.5f;
            const float rx = pos.x + halfH * 1.5f;
            dl->AddTriangleFilled(ImVec2(lx + 3, cy - 3), ImVec2(lx + 3, cy + 3),
                                  ImVec2(lx - 2, cy), labelColour);
            dl->AddTriangleFilled(ImVec2(rx - 3, cy - 3), ImVec2(rx - 3, cy + 3),
                                  ImVec2(rx + 2, cy), labelColour);
        } else {
            const float cx = (pos.x + posMax.x) * 0.5f;
            dl->AddTriangleFilled(ImVec2(cx - 3, pos.y + halfH - 4),
                                  ImVec2(cx + 3, pos.y + halfH - 4),
                                  ImVec2(cx, pos.y + halfH - 8), labelColour);
            dl->AddTriangleFilled(ImVec2(cx - 3, pos.y + halfH + 4),
                                  ImVec2(cx + 3, pos.y + halfH + 4),
                                  ImVec2(cx, pos.y + halfH + 8), labelColour);
        }

        // UDN_DELTAPOS is sent before the change so the parent can veto it;
        // nothing in this tree vetoes, so the position moves and the page is
        // told through the same notification.
        //
        // The sign matters and is counter-intuitive. A vertical up-down
        // reports iDelta = -1 for the UP arrow, and consumers negate it:
        //
        //     int delta = -nmud->iDelta;                    // OptionsPages.cpp
        //     Cfg()->CfgLogicPrm.MFDSize = ... + delta;
        //
        // so that up increases the value. Sending +1 for up would invert every
        // spin button on the options pages.
        if (enabled && (upHit || dnHit)) {
            const int delta = upHit ? -1 : +1;
            orbiter_SetSpinPos(h, val - delta, delta);
        }

    } else if (cname == "SysTreeView32") {
        // The scenario list. TabScenario.cpp builds it with TVM_INSERTITEM and
        // reads the selection back by walking parents, so the item tree is
        // real; this only draws the visible rows and turns clicks into the
        // same messages a real tree view sends.
        dl->AddRectFilled(pos, posMax, enabled ? kWindow : kFace);
        dl->AddRect(pos, posMax, kFieldEdge);

        // Clip to the control. A Win32 control never paints outside its own
        // rectangle, and long scenario names are wider than the tree pane --
        // without this they spill across the description pane, and only look
        // correct when a later sibling happens to overpaint them.
        ImGui::PushClipRect(ImVec2(pos.x + 1, pos.y + 1),
                            ImVec2(posMax.x - 1, posMax.y - 1), true);

        const float rowH   = ImGui::GetTextLineHeight() + 2.0f;
        const float indent = 16.0f;
        const int   rows   = orbiter_TreeVisibleCount(h);

        int clickedRow = -1, onExpander = 0, dbl = 0;

        for (int i = 0; i < rows; ++i) {
            const char *itext = nullptr;
            int depth = 0, image = 0, sel = 0, kids = 0, open = 0;
            int check = 0;
            if (!orbiter_TreeGetRow(h, i, &itext, &depth, &image, &sel,
                                    &kids, &open, &check))
                continue;

            const float ry = pos.y + 2 + i * rowH;
            if (ry + rowH > posMax.y) break;      // clipped by the frame

            const float rx = pos.x + 2 + depth * indent;

            // Selection highlight. Without TVS_FULLROWSELECT it covers the
            // LABEL only, not the row -- the scenario list is 0x910022 and
            // the options page list 0x910020, neither of which sets that bit
            // (0x1000). Filling the row painted a blue band clear across the
            // pane where Windows shows a short bar behind the text.
            //
            // The rectangle is computed after the icon offset below, so this
            // records the request and defers the drawing.
            const bool fullRow = (style & TVS_FULLROWSELECT) != 0;
            if (sel && fullRow)
                dl->AddRectFilled(ImVec2(pos.x + 1, ry),
                                  ImVec2(posMax.x - 1, ry + rowH),
                                  IM_COL32(0, 120, 215, 255));

            // Expander button and linking lines, both driven by the style
            // word the template carries. From Orbiter.rc:
            //
            //   IDC_EXT_LIST      TVS_HASBUTTONS|TVS_HASLINES|TVS_LINESATROOT
            //                     |TVS_SHOWSELALWAYS
            //   IDC_SCN_LIST      TVS_HASLINES|TVS_SHOWSELALWAYS
            //   IDC_OPT_PAGELIST  TVS_SHOWSELALWAYS
            //   IDC_MOD_TREE      TVS_DISABLEDRAGDROP|TVS_SHOWSELALWAYS
            //                     |TVS_NOTOOLTIPS|TVS_CHECKBOXES
            //
            // So only the Extra list gets buttons, and only it gets lines at
            // the root level. The button is a square [+] / [-] box, which is
            // what the common control draws -- not a triangle.
            const float lineX = rx + 5;
            const float midY  = ry + rowH * 0.5f;
            const ImU32 lineC = IM_COL32(128, 128, 128, 255);

            // A dotted line is drawn as alternating pixels, as the control
            // does; a solid stroke reads as a box rather than a tree line.
            auto dottedV = [&](float x, float y0, float y1) {
                for (float y = y0; y < y1; y += 2.0f)
                    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + 1, y + 1), lineC);
            };
            auto dottedH = [&](float y, float x0, float x1) {
                for (float x = x0; x < x1; x += 2.0f)
                    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + 1, y + 1), lineC);
            };

            // TVS_LINESATROOT governs the root level only; deeper levels are
            // linked whenever TVS_HASLINES is set.
            const bool linesHere =
                (style & TVS_HASLINES) &&
                (depth > 0 || (style & TVS_LINESATROOT));

            if (linesHere) {
                // Stub out to the item, and the trunk this row hangs from.
                dottedH(midY, lineX, rx + 14);
                dottedV(lineX, ry, midY);

                // The trunk continues past this row only while another item
                // follows at the same level or deeper.
                int nextDepth = -1;
                const char *nt = nullptr;
                if (i + 1 < rows &&
                    orbiter_TreeGetRow(h, i + 1, &nt, &nextDepth, nullptr,
                                       nullptr, nullptr, nullptr, nullptr) &&
                    nextDepth >= depth)
                    dottedV(lineX, midY, ry + rowH);
            }

            if (kids && (style & TVS_HASBUTTONS)) {
                // [+] when collapsed, [-] when expanded.
                const float bs = 8.0f;
                const ImVec2 bMin(lineX - bs * 0.5f, midY - bs * 0.5f);
                const ImVec2 bMax(bMin.x + bs, bMin.y + bs);
                dl->AddRectFilled(bMin, bMax, kWindow);
                dl->AddRect(bMin, bMax, IM_COL32(128, 128, 128, 255));
                dl->AddLine(ImVec2(bMin.x + 2, midY), ImVec2(bMax.x - 2, midY),
                            kText);
                if (!open)
                    dl->AddLine(ImVec2(lineX, bMin.y + 2),
                                ImVec2(lineX, bMax.y - 2), kText);
            }

            // Check box, when the row carries a state image. TabModule gives
            // modules index 1 or 2 and clears categories to 0, so a zero here
            // means the row genuinely has no box rather than an unset field.
            float ix = rx + 14;
            if (check) {
                const float bs = 13.0f;
                const ImVec2 bMin(rx + 14, ry + (rowH - bs) * 0.5f);
                const ImVec2 bMax(bMin.x + bs, bMin.y + bs);
                dl->AddRectFilled(bMin, bMax, kWindow);
                dl->AddRect(bMin, bMax, kFieldEdge);
                if (check == 2) {
                    dl->AddLine(ImVec2(bMin.x + 3, bMin.y + 6),
                                ImVec2(bMin.x + 5, bMin.y + 9), kText, 1.6f);
                    dl->AddLine(ImVec2(bMin.x + 5, bMin.y + 9),
                                ImVec2(bMin.x + 10, bMin.y + 3), kText, 1.6f);
                }
                ix = bMax.x + 3;
            }

            // Icon from the image list the tab attached with TVM_SETIMAGELIST.
            float u0, v0, u1, v1;
            const unsigned long long tex =
                orbiter_ImageListTexture(h, image, &u0, &v0, &u1, &v1);
            if (tex)
                dl->AddImage((ImTextureID)tex, ImVec2(ix, ry),
                             ImVec2(ix + 16, ry + 16),
                             ImVec2(u0, v0), ImVec2(u1, v1));

            if (itext) {
                // The label follows the icon only when there IS one. The
                // Extra and Options trees never call TVM_SETIMAGELIST, so
                // adding the icon width unconditionally pushed their labels
                // an icon's worth to the right of where Windows puts them.
                const float tx = tex ? ix + 18 : ix;
                if (sel && !fullRow) {
                    const float tw = ImGui::CalcTextSize(itext).x;
                    dl->AddRectFilled(ImVec2(tx - 2, ry),
                                      ImVec2(tx + tw + 2, ry + rowH),
                                      IM_COL32(0, 120, 215, 255));
                }
                dl->AddText(ImVec2(tx, ry),
                            sel ? IM_COL32(255,255,255,255) : labelColour,
                            itext);
            }
        }

        ImGui::SetCursorScreenPos(pos);
        if (ImGui::InvisibleButton("##tree", size) && enabled) {
            const ImVec2 m = ImGui::GetIO().MousePos;
            clickedRow = (int)((m.y - pos.y - 2) / rowH);
            int depth = 0;
            const char *t = nullptr;
            int check = 0;
            if (orbiter_TreeGetRow(h, clickedRow, &t, &depth, nullptr,
                                   nullptr, nullptr, nullptr, &check)) {
                const float rx = pos.x + 2 + depth * indent;
                // Only a control that draws expander buttons has anything to
                // hit there; otherwise the whole row selects.
                onExpander = ((style & TVS_HASBUTTONS) &&
                              m.x >= rx && m.x < rx + 12) ? 1 : 0;
                // A row with a state image has a check box between the
                // expander and the icon. Clicking it toggles the check --
                // and ALSO selects the row, which is what the Windows tree
                // view does.
                //
                // Suppressing the selection here left the Modules tab with a
                // permanently empty description pane: ModuleTab shows a
                // module's info text from TVN_SELCHANGED, so ticking a box
                // without selecting sends no notification and the pane keeps
                // its default "Optional Orbiter plugin modules" blurb. The
                // strings were loading correctly the whole time -- nothing was
                // ever asking for them.
                if (check && m.x >= rx + 13 && m.x < rx + 27)
                    orbiter_TreeToggleCheck(h, clickedRow);
            }
        }
        if (ImGui::IsItemHovered() &&
            ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            const ImVec2 m = ImGui::GetIO().MousePos;
            clickedRow = (int)((m.y - pos.y - 2) / rowH);
            dbl = 1;

            // A double-click on an item that has children expands or
            // collapses it. That is the default tree view behaviour, and with
            // TVS_HASBUTTONS absent it is the ONLY way to open a folder --
            // which is how the scenario list is meant to be used. The
            // notification is still sent, because TabScenario also launches
            // the selected scenario on double-click; a folder is not a
            // scenario, so the two do not collide.
            int kids = 0;
            const char *t = nullptr;
            if (orbiter_TreeGetRow(h, clickedRow, &t, nullptr, nullptr,
                                   nullptr, &kids, nullptr, nullptr) && kids)
                onExpander = 1;
        }
        if (clickedRow >= 0)
            orbiter_TreeClickRow(h, clickedRow, onExpander, dbl);

        // A real tree view notifies its parent on every paint. ModuleTab
        // counts these to run InitActivation() and ActivateFromList(); see
        // orbiter_NotifyTreeCustomDraw.
        orbiter_NotifyTreeCustomDraw(h);

        ImGui::PopClipRect();

    } else if (orbiter_HasWndProc(h)) {
        // A class Orbiter registered: the DlgCtrl gauges, switches and
        // property list, and the CustomControls. These paint themselves with
        // GDI in response to WM_PAINT, so the appearance is Orbiter's own --
        // there is nothing here to imitate. Send the message and replay what
        // the control recorded.

        // BUT FIRST, IF A CUSTOM SWAP CHAIN PRESENTS HERE.
        //
        // Drawn BEFORE the WM_PAINT replay because that is the order Windows
        // produces: the swap chain's Present puts the picture in the window
        // and the window procedure's GDI goes on top of it. DX9ExtMFD's
        // RepaintDisplay draws nothing, so nothing is painted over the MFD --
        // but a control that presented AND drew would compose the same way
        // here as there. See g_controlImages above.
        {
            auto ci = g_controlImages.find(h);
            if (ci != g_controlImages.end() && ci->second.view) {
                const unsigned long long tex =
                    orbiter_ImGuiTextureFromView(ci->second.view);
                if (tex)
                    dl->AddImage((ImTextureID)tex, pos, posMax);
            }
        }

        orbiter_SendPaint(h);
        if (HDC dc = orbiter_GetPaintDC(h)) {
            orbiter_ReplayDC(dc, pos.x, pos.y);
            orbiter_ResetDC(dc);
        }

        // Input has to reach it as a real press/drag/release sequence, not a
        // single click. DlgCtrl's gauge captures the mouse on WM_LBUTTONDOWN
        // and tracks the slider through WM_MOUSEMOVE until WM_LBUTTONUP;
        // sending down and up together would make the arrow buttons step once
        // and make the slider undraggable.
        //
        // But the hit area must exclude any sibling that sits over this
        // control. A SplitterCtrl spans BOTH of its panes -- IDC_OPT_SPLIT
        // covers the options page list and the whole page container,
        // IDC_SCN_SPLIT1 covers the scenario tree and the description pane --
        // and in Win32 the panes are separate windows above it, so only the
        // divider gap between them belongs to the splitter. Claiming the whole
        // rectangle swallows every click on the tab.
        bool coveredBySibling = false;
        {
            const ImVec2 m = ImGui::GetIO().MousePos;
            HWND parent = orbiter_GetParentWnd(h);
            if (parent) {
                HWND sib[256];
                const int ns = orbiter_EnumChildren(parent, sib, 256);
                for (int i = 0; i < ns && !coveredBySibling; ++i) {
                    if (sib[i] == h) continue;
                    const char *scls = nullptr, *stxt = nullptr;
                    int sid = 0, sx = 0, sy = 0, scx = 0, scy = 0, svis = 0, sen = 0;
                    unsigned sst = 0;
                    orbiter_GetControlInfo(sib[i], &scls, &stxt, &sid, &sx, &sy,
                                           &scx, &scy, &sst, &svis, &sen);
                    if (!svis) continue;
                    const float ax = originX + (float)sx, ay = originY + (float)sy;
                    if (m.x >= ax && m.x < ax + (float)scx &&
                        m.y >= ay && m.y < ay + (float)scy)
                        coveredBySibling = true;
                }
            }
        }

        ImGui::SetCursorScreenPos(pos);
        if (!coveredBySibling)
            ImGui::InvisibleButton("##custom", size,
                                   ImGuiButtonFlags_MouseButtonLeft);

        if (enabled && !coveredBySibling) {
            const ImVec2 m = ImGui::GetIO().MousePos;
            const LPARAM pt = MAKELPARAM((int)(m.x - pos.x),
                                         (int)(m.y - pos.y));

            if (ImGui::IsItemActivated())
                SendMessageA(h, WM_LBUTTONDOWN, 0, pt);
            else if (ImGui::IsItemActive())
                SendMessageA(h, WM_MOUSEMOVE, 0, pt);
            else if (ImGui::IsItemDeactivated())
                SendMessageA(h, WM_LBUTTONUP, 0, pt);
        }

    } else {
        // A class with no renderer. Outlined in pink so the layout is visible
        // and obviously unfinished rather than silently absent -- which is the
        // right default and is how the progress bar was found.
        //
        // THE LIST THIS COMMENT USED TO GIVE WAS STALE: it named the tree
        // view, trackbar and up-down, all three of which have had renderers
        // above for some time, and the progress bar has one now too. What
        // actually reaches here, of everything the dialog templates
        // instantiate, is:
        //
        //   SysTabControl32   IDD_CAMERA, IDD_JUMPVESSEL
        //   MapWindow         IDD_MAP
        //   OrbiterHelp       IDD_ORBITERHELP, IDD_HELP
        //
        // and the tab control is deliberate rather than pending: NOTHING IN
        // THE CORE SENDS A SINGLE TCM_ MESSAGE. Nobody inserts a tab, so a
        // renderer would draw an empty strip that no code drives. The only
        // TCM_INSERTITEM in the tree is in TrackIRconfig, a plugin. Building
        // it now would be inventing behaviour, not porting it.
        dl->AddRect(pos, posMax, IM_COL32(200, 150, 150, 255));
        if (!label.empty())
            dl->AddText(ImVec2(pos.x + 3, pos.y + 2), kTextGrey, label.c_str());
    }
    }   // end of the non-dialog chrome

    // GDI THAT WAS DRAWN INTO THIS CONTROL FROM OUTSIDE WM_PAINT.
    //
    // The branch above imitates the stock class; this replays anything the
    // application drew into the control itself through GetDC. On Windows the
    // two compose in exactly this order -- the control paints its own
    // background, and the app's GetDC drawing goes on top of it -- and both
    // sit under the control's children, which are separate windows.
    //
    // ScnEditor's vessel preview is the case this exists for: IDC_VESSELBMP is
    // a stock SS_BITMAP static whose template names no bitmap, so the branch
    // above draws nothing, and DrawVesselBmp StretchBlts the vessel image into
    // it by hand whenever the type selection changes.
    //
    // NOT RESET AFTER REPLAYING, unlike the two paths that send WM_PAINT
    // first. Those re-record the whole control every frame, so keeping the old
    // commands would stack them. Nothing re-issues a GetDC drawing -- it
    // happened once, when the app decided something had changed -- so the
    // recording has to survive until InvalidateRect(h, NULL, TRUE) erases it.
    //
    // orbiter_FindPaintDC does not create a DC and returns null for an empty
    // one, so a control nobody has drawn into costs one map lookup.
    //
    // The wndproc and SS_OWNERDRAW paths already replayed and RESET their DC,
    // so this finds nothing for them and cannot double-draw.
    if (HDC extraDC = orbiter_FindPaintDC(h))
        orbiter_ReplayDC(extraDC, pos.x, pos.y);

    // Any window may have child windows, and they are positioned relative to
    // it. This is what nests a Launchpad tab page inside
    // IDC_MNU_PAGECONTAINER: LpadTab.cpp parents every page to that control,
    // which is an SS_OWNERDRAW static. Returning after painting the static
    // left every page and all of its controls undrawn.
    //
    // Template order, which is also Win32 z-order: the dialog manager creates
    // controls in the order they appear and each new sibling goes to the top,
    // so the LAST control in the template is topmost. ImGui gives hit-testing
    // priority to the item submitted last, so submitting in template order
    // reproduces that exactly.
    //
    // The Options page proves the direction. Its template is
    //     OrbiterDlgCtrl 1002 (0,0) 277x254   <- IDC_OPT_SPLIT, the splitter
    //     SysTreeView32  1000 (0,0) 120x254   <- page list
    //     OrbiterDlgCtrl 1001 (130,0) 147x254 <- page container
    // The splitter is listed first and spans both panes. Were first topmost,
    // it would cover the list and the container on Windows too and the tab
    // would be unusable there -- so first is bottom, and drawing forwards is
    // correct. The splitter is additionally kept from claiming area covered by
    // a sibling; see the custom-control branch.
    HWND kids[256];
    const int nkids = orbiter_EnumChildren(h, kids, 256);
    for (int i = 0; i < nkids; ++i)
        drawControl(kids[i], pos.x, pos.y);

    ImGui::PopID();
}

void drawDialog(HWND dlg)
{
    const char *cls = nullptr, *caption = nullptr;
    int id = 0, x = 0, y = 0, cx = 0, cy = 0, visible = 0, enabled = 0;
    unsigned style = 0;
    orbiter_GetControlInfo(dlg, &cls, &caption, &id, &x, &y, &cx, &cy,
                           &style, &visible, &enabled);
    if (!visible) return;

    const ImGuiViewport *vp = ImGui::GetMainViewport();

    // An owned popup -- every modal in this tree -- is a window in its own
    // right: caption bar, centred over its owner, dismissable. The Launchpad
    // instead fills the host window with no chrome.
    //
    // Having an owner is the only thing that distinguishes the two. Testing
    // WS_POPUP as well is wrong: IDD_MAIN carries WS_POPUP too (it is a
    // top-level window, not a child), so that test caught the Launchpad and
    // drew it inside a second title bar, shrunk by the caption height.
    const bool popup = orbiter_GetOwner(dlg) != nullptr;

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;

    std::string title;
    if (popup) {
        ImGui::SetNextWindowPos(
            ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f,
                   vp->WorkPos.y + vp->WorkSize.y * 0.5f),
            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        // THE TEMPLATE SIZE IS THE CLIENT AREA, and the decoration adds to it.
        // That is what Windows does: CreateDialog runs AdjustWindowRectEx over
        // the template's cx/cy, so the caption and borders grow the window and
        // the client ends up exactly the size the designer drew.
        //
        // The eight pixels that stood here were not that measurement, and the
        // shortfall was visible. ImGui's title bar is
        //
        //     window->TitleBarHeight = FontSize + FramePadding.y * 2
        //
        // (imgui.cpp:7988) -- about 21 px with this font, not 8 -- and the
        // content region begins below it (imgui.cpp:8398), so every popup
        // dialog's client was some thirteen pixels shorter than its template.
        // The Scenario Editor showed it plainly: its bottom row, Close / Save
        // ... / Date ... / Help, sits at dialog units y 221..235 of a 239-unit
        // template, which is px 359..381 of 388, and it was cut in half.
        //
        // The border is counted too. WorkRect starts at
        // max(WindowPadding, WindowBorderSize) inside InnerRect
        // (imgui.cpp:8386-8387) and the clip ends half a border short of it
        // (imgui.cpp:8322-8323); with WindowPadding pushed to zero below, that
        // is a whole border at the top-left and half at the bottom-right.
        // Allowing one on each side covers both.
        const float captionH = ImGui::GetFrameHeight();
        const float border   = ImGui::GetStyle().WindowBorderSize;

        // A DIALOG THE APPLICATION RESIZES AFTER IT IS OPEN.
        //
        // ImGuiCond_Appearing applies the size once, when the window first
        // appears, and that is right for the initial placement -- it lets a
        // saved imgui.ini and the user's own drag survive instead of being
        // stamped back every frame. But it also means a later SetWindowPos
        // never reaches ImGui, and Win32 code resizes its own dialogs:
        //
        //     case IDC_DBG_MORE:                      // DebugControls.cpp
        //         GetWindowRect(hDlg, &rect);
        //         SetWindowPos(hDlg, NULL, rect.left, rect.top,
        //                      isOpen ? 298 : origwidth, ..., SWP_SHOWWINDOW);
        //
        // -- the >>> button on Vulkan Debug Controls, which widens the dialog
        // to reveal a whole second column. The controls in that column were
        // being created, reported visible by childdump and positioned
        // correctly; they simply fell outside a window that never grew, so
        // the button appeared to do nothing but change its own caption.
        //
        // So: honour the template size on first appearance, and force the new
        // size on any frame where the APPLICATION has changed it since the
        // last one. A user's own resize is not touched, because it does not
        // change the Win32 window's cx/cy that this compares against.
        static std::map<HWND, ImVec2> lastAppSize;
        ImGuiCond sizeCond = ImGuiCond_Appearing;
        {
            const ImVec2 want((float)cx, (float)cy);
            auto it = lastAppSize.find(dlg);
            if (it == lastAppSize.end()) {
                lastAppSize.emplace(dlg, want);
            } else if (it->second.x != want.x || it->second.y != want.y) {
                it->second = want;
                sizeCond   = ImGuiCond_Always;
            }
        }

        ImGui::SetNextWindowSize(ImVec2((float)cx + border * 2.0f,
                                        (float)cy + captionH + border * 2.0f),
                                 sizeCond);
        title = (caption && *caption) ? caption : "Orbiter";
        title += "###dlg";
        title += std::to_string(id);
    } else {
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoBringToFrontOnFocus;
        title = "##dialog";
    }

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, kDialogBk);
    ImGui::PushStyleColor(ImGuiCol_TitleBg,       IM_COL32(0, 60, 130, 255));
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive, IM_COL32(0, 90, 180, 255));
    ImGui::PushStyleColor(ImGuiCol_Text,          IM_COL32(0, 0, 0, 255));

    // WS_SYSMENU IS THE CLOSE BOX, and for most module dialogs it is the ONLY
    // way to shut them.
    //
    // Meshdebug's template asks for one (Meshdebug.rc:33):
    //
    //     STYLE DS_SETFONT | DS_MODALFRAME | WS_POPUP | WS_CAPTION | WS_SYSMENU
    //
    // and its procedure is waiting for the result (Meshdebug.cpp:250):
    //
    //     case IDCANCEL:   // dialog closed by user
    //         CloseDlg (hDlg);
    //
    // On Windows the chain is: the caption X sends WM_SYSCOMMAND SC_CLOSE,
    // which becomes WM_CLOSE, and DefDlgProc turns WM_CLOSE into
    // WM_COMMAND(IDCANCEL) for a dialog. Begin() was passing nullptr for
    // p_open, so no box was ever drawn and nothing could send it -- the Mesh
    // Debugger and every other WS_SYSMENU dialog in the tree could be opened
    // and then never closed.
    //
    // Only for a popup: the Launchpad fills the host window and has no caption
    // at all (see the `popup` note above).
    bool open = true;
    bool *pOpen = (popup && (style & WS_SYSMENU)) ? &open : nullptr;

    if (ImGui::Begin(title.c_str(), pOpen, flags)) {
        const ImVec2 origin = ImGui::GetCursorScreenPos();

        // Template order: see the note at the child pass in drawControl.
        HWND children[256];
        const int n = orbiter_EnumChildren(dlg, children, 256);
        for (int i = 0; i < n; ++i)
            drawControl(children[i], origin.x, origin.y);
    }
    ImGui::End();

    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar();

    // IDCANCEL IS POSTED, NOT WM_CLOSE -- and the difference matters.
    //
    // The shim's default window handler answers WM_CLOSE with DestroyWindow
    // (Win32Dlg.cpp:1964). That is correct for a plain window and wrong for a
    // dialog: it would tear the window down behind the module's back, so
    // Meshdebug's CloseDlg would never run, its g_hDlg would be left dangling
    // at a destroyed window, and oapiCloseDialog would never be called.
    // DefDlgProc's WM_CLOSE -> WM_COMMAND(IDCANCEL) translation is the
    // behaviour being converted, and this is the only place it can happen.
    //
    // Posted rather than sent, and after End(), because the handler will
    // usually destroy this dialog -- which must not happen while ImGui is
    // still inside the window it is drawing, nor while drawDialog's caller is
    // walking the dialog list.
    if (pOpen && !open)
        PostMessageA(dlg, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0);
}

// ---------------------------------------------------------------------------
// Initialisation
// ---------------------------------------------------------------------------

bool initialise()
{
    if (g_initFailed) return false;
    if (g_initialised) return true;

    // Display backend selection.
    //
    // GLFW prefers Wayland when a compositor is present, and falls back to the
    // default wayland-0 socket even if WAYLAND_DISPLAY is unset -- so there is
    // no way to ask for X11 through the environment alone. ORBITER_GLFW_PLATFORM
    // provides that: "x11" or "wayland". It matters beyond testing, because a
    // user whose compositor misbehaves has no other way to force XWayland.
    if (const char *plat = getenv("ORBITER_GLFW_PLATFORM")) {
        if (strcmp(plat, "x11") == 0)
            glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);
        else if (strcmp(plat, "wayland") == 0)
            glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_WAYLAND);
    }

    // GLFW reports every failure through this callback and through nothing
    // else. Without one installed it is silent, so a call that quietly does
    // nothing -- glfwSetWindowIcon on a platform that does not support it, for
    // instance -- looks identical to one that worked.
    glfwSetErrorCallback([](int code, const char *desc) {
        fprintf(stderr, "Orbiter: GLFW error %d: %s\n", code,
                desc ? desc : "?");
    });

    if (!glfwInit()) {
        fprintf(stderr, "Orbiter: glfwInit failed -- no display?\n");
        g_initFailed = true;
        return false;
    }

    if (!glfwVulkanSupported()) {
        fprintf(stderr, "Orbiter: Vulkan not supported by this GLFW build\n");
        g_initFailed = true;
        return false;
    }

    // No OpenGL context: the surface is Vulkan's.
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

    // THE APPLICATION IDENTITY, which is what carries the icon on Wayland.
    //
    // Windows takes the icon from the window class (wndClass.hIcon). X11 takes
    // it from a property on the window, which glfwSetWindowIcon sets below.
    // WAYLAND HAS NEITHER: there is no protocol for handing a compositor
    // pixels for a window. What it has is xdg-shell's app_id, which the
    // compositor matches against an installed .desktop file and takes the icon
    // from that.
    //
    // So the app_id is set here and must agree with the basename of the
    // .desktop file. Setting the X11 class name to the same string costs
    // nothing and makes both platforms identify the program the same way,
    // which is also what a task switcher groups on.
    glfwWindowHintString(GLFW_WAYLAND_APP_ID,     "openorbiter");
    glfwWindowHintString(GLFW_X11_CLASS_NAME,     "openorbiter");
    glfwWindowHintString(GLFW_X11_INSTANCE_NAME,  "openorbiter");

    // The window is sized to the Launchpad template: IDD_MAIN is 400x333
    // dialog units, which at the (6,13) base unit pair is 600x541 pixels.
    // Sizing to the dialog rather than to an arbitrary default is what makes
    // the layout land where Windows puts it, with no scrollbars or clipping.
    // IDD_MAIN is 400x333 dialog units; at the (6,13) base unit pair that is
    // 600x541 pixels. WM_GETMINMAXINFO in Launchpad.cpp floors the window at
    // 550x350, so this satisfies its own minimum.
    const int width  = 400 * 6 / 4;
    const int height = 333 * 13 / 8;
    g_window = glfwCreateWindow(width, height, "OpenOrbiter Launchpad",
                                nullptr, nullptr);
    if (!g_window) {
        fprintf(stderr, "Orbiter: could not create window\n");
        g_initFailed = true;
        return false;
    }
    orbiter_SetGLFWWindow(g_window);

    // The window icon.
    //
    // On Windows this arrives through the window CLASS: Orbiter::Create sets
    //     wndClass.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_MAIN_ICON));
    // and RegisterClass carries it to every window of that class. X11 has no
    // class-level icon -- _NET_WM_ICON is a property on the window itself and
    // carries the pixels -- so the same resource is decoded and handed over
    // here instead. Without it the title bar shows the window manager's
    // placeholder.
    //
    // IDI_MAIN_ICON is 101, from Src/Orbiter/resource.h. The value is written
    // out rather than included because this file deliberately pulls in none of
    // Orbiter's own headers.
    //
    // NOT ATTEMPTED ON WAYLAND. There is no protocol for it, and calling
    // anyway produces
    //     GLFW error 65548: Wayland: The platform does not support setting
    //                       the window icon
    // on every start -- an error for something that was never going to work,
    // which trains the reader to ignore errors. The app_id set above is the
    // Wayland route; see the note there.
    if (glfwGetPlatform() == GLFW_PLATFORM_WAYLAND) {
        fprintf(stderr,
                "Orbiter: running on Wayland, which has no window-icon "
                "protocol.\n"
                "         The icon comes from an installed .desktop file "
                "named openorbiter.desktop\n"
                "         (see Src/Orbiter/Linux/openorbiter.desktop), or run "
                "with\n"
                "         ORBITER_GLFW_PLATFORM=x11 to use the X11 path.\n");
    }
    else {
        int   iw[8] = {0}, ih[8] = {0};
        unsigned char *ipx[8] = {nullptr};
        const int n = orbiter_GetWindowIconImages(101, 8, iw, ih, ipx);
        if (n > 0) {
            GLFWimage imgs[8];
            for (int i = 0; i < n; ++i) {
                imgs[i].width  = iw[i];
                imgs[i].height = ih[i];
                imgs[i].pixels = ipx[i];
            }
            glfwSetWindowIcon(g_window, n, imgs);
            if (getenv("ORBITER_TRACE_MODULES")) {
                fprintf(stderr, "[icon] set %d image(s):", n);
                for (int i = 0; i < n; ++i)
                    fprintf(stderr, " %dx%d", iw[i], ih[i]);
                fputc('\n', stderr);
            }
        } else {
            fprintf(stderr, "Orbiter: no window icon (IDI_MAIN_ICON "
                            "not found or not decodable)\n");
        }
    }

    if (!createVulkanInstance() || !pickPhysicalDevice() ||
        !createLogicalDevice()) {
        g_initFailed = true;
        return false;
    }

    int fbw = 0, fbh = 0;
    glfwGetFramebufferSize(g_window, &fbw, &fbh);
    if (!setupSurface(fbw, fbh)) { g_initFailed = true; return false; }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // Orbiter.rc specifies "MS Shell Dlg" at 8pt for every dialog. Windows
    // maps that to Tahoma, which is present here through the wine font
    // package.
    //
    // The size argument is NOT the point size and NOT the em size. ImGui
    // passes it to stbtt_ScaleForPixelHeight, which scales the face so that
    // ascent+descent equals it -- the whole line box, not the em square.
    // Tahoma has unitsPerEm 2048 with ascent 2049 and descent -423, so its
    // line box is 2472/2048 = 1.207 em.
    //
    // Windows creates the dialog font as
    //     CreateFont(-MulDiv(8, 96, 72), ...)   ->  em = 10.667 px
    // so to get that same em here the line box must be
    //     10.667 * 1.207 = 12.88 px
    // Passing 11.0 gave an em of 2048*11/2472 = 9.1 px -- every caption,
    // tree row and control glyph about 15% smaller than Windows, and the
    // row pitch and expander boxes shrank with it.
    const float kEmPx      = 8.0f * 96.0f / 72.0f;      // 10.667
    const float kLineBoxEm = 2472.0f / 2048.0f;         // 1.2070
    const float kFontPx    = kEmPx * kLineBoxEm;        // 12.88

    ImGuiIO &io = ImGui::GetIO();
    const char *tahoma = "/usr/share/fonts/truetype/wine/tahoma.ttf";
    if (!io.Fonts->AddFontFromFileTTF(tahoma, kFontPx)) {
        fprintf(stderr, "Orbiter: could not load %s -- dialog text metrics "
                        "will not match Windows\n", tahoma);
        io.Fonts->AddFontDefault();
    }

    // FONT AWESOME, MERGED INTO THAT FACE, and the reason is an ordering
    // difference this port introduces on its own.
    //
    // The core merges the icon set in DialogManager::InitImGui:
    //
    //     defaultFont = loadFont(prm.ImGui_DefaultFontFile, ...);   // Roboto
    //     if (exists("Fonts/fa-solid-900.ttf"))
    //         io.Fonts->AddFontFromFileTTF("Fonts/fa-solid-900.ttf",
    //                                      prm.ImGui_FontSize, &icons_config);
    //
    // On Windows that IS the atlas: InitImGui creates the context, Roboto is
    // font[0], the icons merge into it, and every dialog draws with it.
    //
    // Here the Launchpad's UIHost builds the context and the atlas long
    // before any graphics client attaches, so THIS Tahoma is font[0] and the
    // core's Roboto arrives later as font[2]. ImGui draws a window that has
    // pushed no font with font[0] -- so every caption, button and tree row in
    // the program uses Tahoma, and the icons the core merged into Roboto are
    // in the atlas but attached to a face nothing draws with.
    //
    // The visible symptom is a question mark. Captions are built as
    // ICON_FA_PUZZLE_PIECE " Orbiter: Custom functions" -- U+F12E -- and a
    // codepoint absent from the font in use comes out as ImGui's fallback
    // glyph, '?'. Every FontAwesome icon in the UI was that '?'.
    //
    // Merging here rather than repointing io.FontDefault at Roboto is
    // deliberate: the Win32 shim dialogs must keep Tahoma, because their
    // metrics are matched to the reference build above and swapping the face
    // would move every control. This gives font[0] the icons without touching
    // the face.
    {
        const char *fa = "Fonts/fa-solid-900.ttf";
        if (std::filesystem::exists(fa)) {
            ImFontConfig cfg;
            cfg.MergeMode  = true;   // into the face just added
            cfg.PixelSnapH = true;
            if (!io.Fonts->AddFontFromFileTTF(fa, kFontPx, &cfg))
                fprintf(stderr, "Orbiter: could not merge %s -- FontAwesome "
                                "icons will draw as '?'\n", fa);
        } else {
            fprintf(stderr, "Orbiter: %s not found -- FontAwesome icons will "
                            "draw as '?'\n", fa);
        }
    }

    // The splash screen's font. See the note by g_splashFont.
    //
    // The size passed here is only a default: since 1.92 an ImFont is
    // size-independent and ImDrawList::AddText(font, size, ...) rasterises at
    // whatever size it is given, which is what lets the splash use the
    // reference's 18 px and 24+x px without loading a face per size.
    //
    // Added at startup rather than on first use so that the one thing that
    // could fail -- reading a font file -- fails once, here, where it is
    // reported, and not in the middle of a session start.
    {
        static const char *const kMono[] = {
            // Metric-compatible with Courier New, and has a bold face, which
            // is what CreateFont(..., 700, ...) asks for.
            "/usr/share/fonts/truetype/liberation/LiberationMono-Bold.ttf",
            "/usr/share/fonts/truetype/wine/courier.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf",
        };
        for (const char *path : kMono) {
            g_splashFont = io.Fonts->AddFontFromFileTTF(path, 18.0f);
            if (g_splashFont) break;
        }
        if (!g_splashFont)
            fprintf(stderr, "Orbiter: no monospace font found for the splash "
                            "screen; falling back to the dialog font\n");
    }

    // Square everything off: Win32 controls have no rounded corners, and the
    // dialog background is the system face colour rather than a dark theme.
    ImGuiStyle &style = ImGui::GetStyle();
    style.WindowRounding    = 0.0f;
    style.FrameRounding     = 0.0f;
    style.PopupRounding     = 0.0f;
    style.ScrollbarRounding = 0.0f;
    style.WindowBorderSize  = 0.0f;
    style.FrameBorderSize   = 0.0f;
    style.Colors[ImGuiCol_Text]       = ImVec4(0, 0, 0, 1);
    style.Colors[ImGuiCol_PopupBg]    = ImVec4(1, 1, 1, 1);
    style.Colors[ImGuiCol_Header]     = ImVec4(0.0f, 0.47f, 0.84f, 1.0f);
    style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.0f, 0.47f, 0.84f, 0.7f);

    ImGui_ImplGlfw_InitForVulkan(g_window, true);

    ImGui_ImplVulkan_InitInfo init{};
    // Must match the instance's apiVersion above, or the backend loads entry
    // points for a different version of the API than the one in use.
    init.ApiVersion      = VK_API_VERSION_1_2;
    init.Instance        = g_instance;
    init.PhysicalDevice  = g_physicalDevice;
    init.Device          = g_device;
    init.QueueFamily     = g_queueFamily;
    init.Queue           = g_queue;
    init.DescriptorPool  = g_descriptorPool;
    init.MinImageCount   = (uint32_t)g_minImageCount;
    init.ImageCount      = g_mainWindowData.ImageCount;
    // As of the 2025-09-26 backend change these live on PipelineInfoMain
    // rather than on InitInfo directly; the header keeps the old names
    // commented out at their former positions to point at the move.
    init.PipelineInfoMain.RenderPass  = g_mainWindowData.RenderPass;
    init.PipelineInfoMain.Subpass     = 0;
    init.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    ImGui_ImplVulkan_Init(&init);

    g_initialised = true;
    return true;
}

// ---------------------------------------------------------------------------
// The scene hook.
//
// renderFrame owns the only command buffer that reaches the swapchain, and it
// used to record exactly one thing: ImGui's draw data. A graphics client had
// nowhere to put a scene -- orbiter_BeginSceneFrame only set a flag and chose
// a clear colour, and clbkRenderScene had no command buffer to record into.
// This is the entry point that was missing, and it is what the comment above
// orbiter_BeginSceneFrame meant by "the scene itself is not drawn yet: this
// stage establishes the frame path".
//
// The client registers a callback and is invoked INSIDE the open render pass,
// BEFORE ImGui's draws, so the scene lands under the dialogs in the same
// swapchain image and they present together.
//
// It is a callback rather than a getter because the two ends run at different
// times: clbkRenderScene is called from Orbiter::Render3DEnvironment, while
// this frame is built later from the message loop's pump. There is no moment
// at which the client could simply ask for the command buffer.
//
// The render pass and its dimensions are passed too: a client cannot create a
// compatible VkPipeline without the VkRenderPass it will be used with, and the
// extent changes whenever the window is resized.
// ---------------------------------------------------------------------------
extern "C" {
typedef void (*OrbiterSceneRenderFn)(void *cmdBuf, void *renderPass,
                                     unsigned width, unsigned height,
                                     void *user);
}
static OrbiterSceneRenderFn g_sceneRenderFn   = nullptr;
static void               *g_sceneRenderUser = nullptr;

extern "C" void orbiter_SetSceneRenderCallback(OrbiterSceneRenderFn fn, void *user)
{
    g_sceneRenderFn   = fn;
    g_sceneRenderUser = user;
}

// ---------------------------------------------------------------------------
// READING THE BACK BUFFER BACK -- the exit screenshot, oapiScreenshot, and
// gcCore's surface saves.
//
// WHY THIS IS HERE AND NOT IN THE CLIENT. D3D9Client::clbkSaveSurfaceToImage
// is handed a NULL surface for "the back buffer" and reaches it with
// pDevice->GetRenderTarget(0) -- a surface the D3D9 client OWNS, because it
// created the device and with it the swap chain. This client owns neither:
// the swapchain images are made here, and CVulkanFramework's back-buffer
// SurfNative is an attachment PROXY with vkImage == VK_NULL_HANDLE, which is
// why VulkanDevice::ReadTexture on it could only fail. It did, once per
// session:
//
//     VulkanERROR: clbkSaveSurfaceToImage: readback failed
//
// and Orbiter::PreCloseSession's Images/CurrentState.jpg -- the thumbnail the
// Launchpad shows for the current state -- was never written.
//
// AND WHY IT IS RECORDED INSIDE THE FRAME rather than done afterwards. A
// presented image belongs to the presentation engine until it is acquired
// again; copying out of it after vkQueuePresentKHR is reading a resource the
// application does not own at that moment. The copy therefore goes into the
// SAME command buffer, immediately after the render pass ends, while the
// image is still this frame's -- which also makes the captured pixels exactly
// the pixels presented, rather than "whatever is there a moment later".
//
// The staging buffer is grown on demand and kept; a screenshot is rare and a
// second one is usually the same size.
// ---------------------------------------------------------------------------
// Already inside the unnamed namespace opened near the top of the file, so
// these are internal linkage without a second `namespace {` -- which would be
// a NESTED unnamed namespace and a different scope from the one the rest of
// this file's helpers live in.

bool           g_captureArmed = false;   ///< record the copy in the next frame
bool           g_captureDone  = false;   ///< the staging buffer holds a frame
VkBuffer       g_captureBuf   = VK_NULL_HANDLE;
VkDeviceMemory g_captureMem   = VK_NULL_HANDLE;
VkDeviceSize   g_captureSize  = 0;
uint32_t       g_captureW = 0, g_captureH = 0;

// Make sure the staging buffer can hold one frame. Returns false and leaves
// the capture disarmed if it cannot.
bool ensureCaptureBuffer(VkDeviceSize bytes)
{
    if (g_captureBuf != VK_NULL_HANDLE && g_captureSize >= bytes) return true;

    if (g_captureBuf) { vkDestroyBuffer(g_device, g_captureBuf, g_allocator); g_captureBuf = VK_NULL_HANDLE; }
    if (g_captureMem) { vkFreeMemory(g_device, g_captureMem, g_allocator);    g_captureMem = VK_NULL_HANDLE; }
    g_captureSize = 0;

    VkBufferCreateInfo bi{};
    bi.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size        = bytes;
    bi.usage       = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(g_device, &bi, g_allocator, &g_captureBuf) != VK_SUCCESS) {
        g_captureBuf = VK_NULL_HANDLE;
        return false;
    }

    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(g_device, g_captureBuf, &req);

    // The memory-type lookup, written out rather than calling the file's
    // findMemoryType: that one is defined much further down, in a nested
    // unnamed namespace of its own, and forward-declaring it from here makes
    // two different functions of the same name.
    uint32_t typeIndex = UINT32_MAX;
    {
        VkPhysicalDeviceMemoryProperties props;
        vkGetPhysicalDeviceMemoryProperties(g_physicalDevice, &props);
        for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
            if ((req.memoryTypeBits & (1u << i)) &&
                (props.memoryTypes[i].propertyFlags &
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
                { typeIndex = i; break; }
        }
    }

    VkMemoryAllocateInfo ai{};
    ai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize  = req.size;
    ai.memoryTypeIndex = typeIndex;
    if (ai.memoryTypeIndex == UINT32_MAX ||
        vkAllocateMemory(g_device, &ai, g_allocator, &g_captureMem) != VK_SUCCESS) {
        vkDestroyBuffer(g_device, g_captureBuf, g_allocator);
        g_captureBuf = VK_NULL_HANDLE;
        g_captureMem = VK_NULL_HANDLE;
        return false;
    }
    vkBindBufferMemory(g_device, g_captureBuf, g_captureMem, 0);
    g_captureSize = bytes;
    return true;
}

// Record the copy, between the end of the render pass and the end of the
// command buffer. The image is in PRESENT_SRC_KHR at that point -- that is
// the render pass's finalLayout -- so it is moved to TRANSFER_SRC and back,
// because vkQueuePresentKHR after this submit requires PRESENT_SRC again.
void recordFrameCapture(VkCommandBuffer cmd, ImGui_ImplVulkanH_Window *wd)
{
    const uint32_t w = (uint32_t)wd->Width;
    const uint32_t h = (uint32_t)wd->Height;
    if (!w || !h) return;

    if (!ensureCaptureBuffer(VkDeviceSize(w) * h * 4)) {
        g_captureArmed = false;
        return;
    }

    VkImage img = wd->Frames[wd->FrameIndex].Backbuffer;

    VkImageMemoryBarrier toSrc{};
    toSrc.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toSrc.oldLayout           = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    toSrc.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toSrc.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toSrc.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toSrc.image               = img;
    toSrc.srcAccessMask       = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    toSrc.dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;
    toSrc.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    toSrc.subresourceRange.levelCount = 1;
    toSrc.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &toSrc);

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent.width  = w;
    region.imageExtent.height = h;
    region.imageExtent.depth  = 1;
    vkCmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           g_captureBuf, 1, &region);

    VkImageMemoryBarrier back = toSrc;
    back.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    back.newLayout     = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    back.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    back.dstAccessMask = 0;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &back);

    g_captureW     = w;
    g_captureH     = h;
    g_captureArmed = false;
    g_captureDone  = true;
}

void renderFrame(ImDrawData *drawData)
{
    ImGui_ImplVulkanH_Window *wd = &g_mainWindowData;

    // THE TWO SEMAPHORES ARE INDEXED DIFFERENTLY, AND THAT IS THE WHOLE POINT.
    //
    // The acquire semaphore has to be picked BEFORE the acquire, so it can
    // only be indexed by a rotating counter -- there is no way to know which
    // image is coming. ImGui sizes FrameSemaphores at ImageCount + 1 exactly
    // so that rotation always finds a free one.
    //
    // The RENDER-COMPLETE semaphore is different. It is the one
    // vkQueuePresentKHR waits on, so it belongs to the IMAGE being presented,
    // not to the frame counter. Reusing it on a rotating index is a real
    // hazard and the validation layer says so by name:
    //
    //   VUID-vkQueueSubmit-pSignalSemaphores-00067
    //   ... is being signaled by VkQueue ..., but it may still be in use by
    //   VkSwapchainKHR ...
    //   Most recently acquired image indices: 2, 1, [0], 2, 1, 2, 1.
    //   Swapchain image 0 was presented but was not re-acquired, so
    //   VkSemaphore ... may still be in use and cannot be safely reused
    //   with image index 1.
    //
    // The presentation engine does NOT return images round-robin -- the
    // history above shows it bouncing between two of three -- so the frame
    // counter and the image index drift apart within a few frames, and the
    // counter then hands back a semaphore that a still-pending present is
    // waiting on.
    //
    // Indexing by image index is the layer's own first recommendation, and it
    // is safe for a reason worth stating: vkAcquireNextImageKHR returning
    // image i means the presentation engine has finished with image i, which
    // means the present that waited on RenderCompleteSemaphore[i] has
    // completed, which means that semaphore is unsignalled. Nothing weaker
    // than re-acquisition proves that.
    //
    // It costs nothing: FrameSemaphores already has ImageCount + 1 entries,
    // and FrameIndex < ImageCount, so entry [ImageCount] simply goes unused
    // for its render-complete half.
    VkSemaphore imageAcquired =
        wd->FrameSemaphores[wd->SemaphoreIndex].ImageAcquiredSemaphore;

    VkResult err = vkAcquireNextImageKHR(g_device, wd->Swapchain, UINT64_MAX,
                                         imageAcquired, VK_NULL_HANDLE,
                                         &wd->FrameIndex);
    if (err == VK_ERROR_OUT_OF_DATE_KHR) {
        // Nothing was acquired and the semaphore was not signalled. Rebuild
        // and skip the frame entirely -- g_framePending stays false, so
        // presentFrame knows there is nothing to present.
        //
        // The fourth unpaced exit, and the most dangerous of them, because
        // this branch RETURNS WITHOUT BLOCKING: the acquire answered
        // immediately instead of waiting for an image. A surface that stays
        // out of date therefore spins rebuild-acquire-rebuild at CPU speed
        // with nothing on screen. No sleep is added here -- the rebuild it
        // schedules calls vkDeviceWaitIdle, which is a real block -- but it
        // says so now. See noteFrameSkipped.
        g_swapChainRebuild = true;
        noteFrameSkipped("vkAcquireNextImageKHR answered OUT_OF_DATE -- the "
                         "swapchain is being rebuilt");
        return;
    }
    if (err == VK_SUBOPTIMAL_KHR) {
        // An image WAS acquired and the semaphore WAS signalled. This frame
        // must therefore be finished and presented; the rebuild happens on
        // the next pump.
        g_swapChainRebuild = true;
    }
    else if (err != VK_SUCCESS) {
        // SURFACE_LOST, DEVICE_LOST, OUT_OF_MEMORY. FrameIndex is unchanged
        // and meaningless; carrying on would record into the previous frame's
        // framebuffer while the last submission may still be reading it.
        //
        // check() names the error, but it does not say that the SIMULATION is
        // now unpaced, and this branch does not block either.
        check(err, "vkAcquireNextImageKHR");
        noteFrameSkipped("vkAcquireNextImageKHR failed");
        return;
    }

    // Only now, with a real image index in hand.
    VkSemaphore renderComplete =
        wd->FrameSemaphores[wd->FrameIndex].RenderCompleteSemaphore;

    ImGui_ImplVulkanH_Frame *fd = &wd->Frames[wd->FrameIndex];

    // Wait for this frame's previous submission before reusing its command
    // buffer; the fence is what makes the per-frame resources safe to touch.
    vkWaitForFences(g_device, 1, &fd->Fence, VK_TRUE, UINT64_MAX);
    vkResetFences(g_device, 1, &fd->Fence);
    vkResetCommandPool(g_device, fd->CommandPool, 0);

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(fd->CommandBuffer, &begin);

    VkRenderPassBeginInfo rp{};
    rp.sType                    = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass               = wd->RenderPass;
    rp.framebuffer              = fd->Framebuffer;
    rp.renderArea.extent.width  = (uint32_t)wd->Width;
    rp.renderArea.extent.height = (uint32_t)wd->Height;

    // TWO clear values now, one per attachment, IN ATTACHMENT ORDER. The count
    // must be at least the index of the highest attachment that uses
    // LOAD_OP_CLEAR -- pass one here against a two-attachment pass and the
    // depth clear reads past the end of the array.
    //
    // Depth clears to 1.0: standard depth, cleared to the far plane, LESS
    // comparison. That is convention 7 in the architecture doc, and it is the
    // matching half of it -- reverse-Z would clear to 0.0 and this is where
    // that decision becomes real.
    VkClearValue clears[2];
    clears[0] = wd->ClearValue;
    clears[1].depthStencil.depth   = 1.0f;
    clears[1].depthStencil.stencil = 0;
    rp.clearValueCount          = (g_depthView != VK_NULL_HANDLE) ? 2 : 1;
    rp.pClearValues             = clears;
    vkCmdBeginRenderPass(fd->CommandBuffer, &rp, VK_SUBPASS_CONTENTS_INLINE);

    // The scene goes first, so the dialogs composite on top of it.
    //
    // GATED ON g_sessionActive, NOT g_inSceneFrame -- and the difference is
    // the whole bug. THERE ARE TWO PUMP PATHS: orbiter_EndSceneFrame calls
    // orbiter_PumpFrame with g_inSceneFrame true, and the message loop in
    // Win32Dlg.cpp calls it independently with the flag false. Both build and
    // PRESENT a frame. Gating on the per-frame flag drew the scene on one
    // path and black on the other, so the two alternated and a screenshot
    // caught whichever landed last -- black, as it turned out.
    //
    // g_sessionActive is set at the first BeginSceneFrame and stays set until
    // the session ends, which is exactly "should this window be showing a
    // scene right now". UIHost's own comment on the two variables says as
    // much: "per-frame only; g_sessionActive persists".
    if (g_sceneRenderFn && g_sessionActive)
        g_sceneRenderFn(fd->CommandBuffer, wd->RenderPass,
                        (unsigned)wd->Width, (unsigned)wd->Height,
                        g_sceneRenderUser);

    // UNDER THE DEVICE LOCK, AND THIS IS NOT BELT AND BRACES.
    //
    // ImGui_ImplVulkan_RenderDrawData looks harmless -- it records vertex
    // buffers and draws -- but since ImGui's dynamic-texture backend it also
    // calls ImGui_ImplVulkan_UpdateTexture for any texture whose Status is
    // WantCreate or WantUpdates, and THAT function does its upload with its
    // own vkQueueSubmit followed by vkQueueWaitIdle, on this same queue.
    //
    // The queue is shared with the graphics client, whose tile loaders submit
    // from Load_ThreadProc through VulkanDevice::EndOneShot. Both were locked;
    // this one was not, and the layer named it exactly:
    //
    //   UNASSIGNED-Threading-MultipleThreads-Write
    //   vkQueueSubmit(): THREADING ERROR : object of type VkQueue is
    //   simultaneously used in current thread A and thread B
    //
    // followed by VK_ERROR_DEVICE_LOST from the next submit. Two per run of
    // the ISS scenario, at the point the font atlas or a menu-bar icon is
    // first uploaded while tiles are still streaming in.
    //
    // The whole call is inside the section rather than only the upload,
    // because the upload is ImGui's to make and there is no hook between the
    // two. It costs the loader thread a few hundred microseconds on the
    // frames that actually upload something, and nothing at all on the rest.
    {
        std::lock_guard<std::recursive_mutex> lk(g_deviceMutex);
        ImGui_ImplVulkan_RenderDrawData(drawData, fd->CommandBuffer);
    }

    vkCmdEndRenderPass(fd->CommandBuffer);

    // The back-buffer readback, if one was asked for. Inside this command
    // buffer and after the pass, while the image is still ours; see the note
    // by recordFrameCapture.
    if (g_captureArmed) recordFrameCapture(fd->CommandBuffer, wd);

    vkEndCommandBuffer(fd->CommandBuffer);

    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit{};
    submit.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount   = 1;
    submit.pWaitSemaphores      = &imageAcquired;
    submit.pWaitDstStageMask    = &stage;
    submit.commandBufferCount   = 1;
    submit.pCommandBuffers      = &fd->CommandBuffer;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores    = &renderComplete;

    // The queue is shared with the graphics client, whose tile-loader threads
    // submit uploads on it. See g_deviceMutex at the top of this file.
    VkResult sub;
    {
        std::lock_guard<std::recursive_mutex> lk(g_deviceMutex);
        sub = vkQueueSubmit(g_queue, 1, &submit, fd->Fence);
    }
    check(sub, "vkQueueSubmit");

    // Only a SUBMITTED frame is a pending frame. If the submit failed nothing
    // will ever signal renderComplete, and presenting would wait forever.
    if (sub == VK_SUCCESS) g_framePending = true;
}

void presentFrame()
{
    // GATED ON WHETHER A FRAME WAS SUBMITTED, NOT ON g_swapChainRebuild.
    //
    // This line used to read `if (g_swapChainRebuild) return;`, which is the
    // ImGui example's shape and carries the example's bug: renderFrame sets
    // that flag on VK_SUBOPTIMAL_KHR and then submits anyway, so the flag made
    // this function abandon a frame that was already on the queue. The image
    // was never handed back to the presentation engine and its render-complete
    // semaphore was left signalled with nothing to wait on it -- so the next
    // submit that reached it signalled an already-signalled binary semaphore.
    //
    // A rebuild is scheduled for the next frame. It is not a reason to drop
    // this one on the floor.
    //
    // The fifth unpaced exit -- but a DERIVED one, not an independent cause:
    // g_framePending is false only because renderFrame took one of its own
    // early returns, and each of those reports for itself. Reporting again
    // here would double-count, so it does not; see noteFrameSkipped.
    if (!g_framePending) return;
    g_framePending = false;

    ImGui_ImplVulkanH_Window *wd = &g_mainWindowData;

    // Indexed by IMAGE, matching the submit that signalled it. See the long
    // note in renderFrame.
    VkSemaphore renderComplete =
        wd->FrameSemaphores[wd->FrameIndex].RenderCompleteSemaphore;

    VkPresentInfoKHR info{};
    info.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    info.waitSemaphoreCount = 1;
    info.pWaitSemaphores    = &renderComplete;
    info.swapchainCount     = 1;
    info.pSwapchains        = &wd->Swapchain;
    info.pImageIndices      = &wd->FrameIndex;

    // Shared queue; see g_deviceMutex at the top of this file.
    VkResult err;
    {
        std::lock_guard<std::recursive_mutex> lk(g_deviceMutex);
        err = vkQueuePresentKHR(g_queue, &info);
    }
    if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR)
        g_swapChainRebuild = true;
    else
        check(err, "vkQueuePresentKHR");

    // THE ONE PLACE A FRAME REACHES THE SCREEN, so it is the one place that
    // can close a skip report. Anything earlier would clear the flag on a
    // frame that was built and then dropped. See noteFrameSkipped.
    noteFramePresented();

    // FRAME PACING, and it is measured HERE for a reason.
    //
    // The reported symptom was that the picture looks "frame'ish" -- that is a
    // statement about the WORST frames, and an average frame rate cannot
    // answer it. 95 fps mean is equally consistent with a perfectly even 10.5
    // ms frame and with 90 frames at 8 ms plus five at 100 ms, and only the
    // second one is visible to a person.
    //
    // One vkQueuePresentKHR is one frame the user actually sees, which the
    // pump is not: orbiter_PumpFrame is called both from the message loop and
    // from orbiter_EndSceneFrame, and skips presenting entirely when nothing
    // is visible. Timing the pump would count frames that never reached the
    // screen.
    //
    // Reported once a second, sorted, so median/p99/worst are exact rather
    // than estimated from a running max.
    if (getenv("ORBITER_TRACE_PACING")) {
        using clk = std::chrono::steady_clock;
        static clk::time_point prev, mark;
        static std::vector<double> ms;
        static bool first = true;

        const clk::time_point now = clk::now();
        if (first) { first = false; prev = mark = now; }
        else {
            ms.push_back(std::chrono::duration<double, std::milli>(now - prev).count());
            prev = now;
            if (std::chrono::duration<double>(now - mark).count() >= 1.0 && !ms.empty()) {
                std::vector<double> s = ms;
                std::sort(s.begin(), s.end());
                double sum = 0.0;
                int over33 = 0, over100 = 0;
                for (double v : s) { sum += v; if (v > 33.0) over33++; if (v > 100.0) over100++; }
                fprintf(stderr,
                        "pacing: %zu frames  mean %.1f ms (%.0f fps)  median %.1f  "
                        "p99 %.1f  worst %.1f  |  >33ms %d  >100ms %d\n",
                        s.size(), sum / s.size(), 1000.0 * s.size() / sum,
                        s[s.size() / 2], s[(size_t)(s.size() * 0.99)], s.back(),
                        over33, over100);
                ms.clear();
                mark = now;
            }
        }
    }

    // Advanced unconditionally, including after OUT_OF_DATE. The acquire
    // semaphore for this slot was consumed by the submit either way, and a
    // rebuild destroys and recreates every semaphore regardless.
    wd->SemaphoreIndex = (wd->SemaphoreIndex + 1) % wd->SemaphoreCount;
}

// ---------------------------------------------------------------------------
// Modal message box
// ---------------------------------------------------------------------------

struct MessageBoxState {
    bool        active = false;
    std::string text;
    std::string caption;
    unsigned    type = 0;
    int         result = 0;

    // NO CALLER IS WAITING FOR THE ANSWER.
    //
    // MessageBoxA normally spins the message loop until result becomes
    // non-zero and then calls orbiter_ClearMessageBox itself, which is what
    // makes it modal. There are two situations where it cannot do that and
    // has to raise the box and return -- a session owns the frames, or the
    // call came from inside the pump; the note above MessageBoxA in
    // Win32Dlg.cpp is where they are set out. A box raised that way has
    // nobody to take the result down, so it takes itself down.
    bool        deferred = false;
};
MessageBoxState g_msgBox;

void drawMessageBox()
{
    if (!g_msgBox.active) return;

    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f,
                                   vp->WorkPos.y + vp->WorkSize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));

    ImGui::PushStyleColor(ImGuiCol_WindowBg, kDialogBk);
    ImGui::PushStyleColor(ImGuiCol_TitleBg, IM_COL32(0, 60, 130, 255));
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive, IM_COL32(0, 90, 180, 255));

    ImGui::Begin(g_msgBox.caption.c_str(), nullptr,
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize |
                 ImGuiWindowFlags_NoSavedSettings);

    ImGui::TextUnformatted(g_msgBox.text.c_str());
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Button set from the MB_* type, matching what the caller asked for: a
    // Yes/No prompt must not offer OK, because the caller compares the
    // result against IDYES.
    const unsigned kind = g_msgBox.type & 0x0F;
    const ImVec2 bsz(88, 24);

    if (kind == MB_YESNO || kind == MB_YESNOCANCEL) {
        if (ImGui::Button("Yes", bsz)) g_msgBox.result = IDYES;
        ImGui::SameLine();
        if (ImGui::Button("No", bsz))  g_msgBox.result = IDNO;
        if (kind == MB_YESNOCANCEL) {
            ImGui::SameLine();
            if (ImGui::Button("Cancel", bsz)) g_msgBox.result = IDCANCEL;
        }
    } else if (kind == MB_OKCANCEL) {
        if (ImGui::Button("OK", bsz)) g_msgBox.result = IDOK;
        ImGui::SameLine();
        if (ImGui::Button("Cancel", bsz)) g_msgBox.result = IDCANCEL;
    } else {
        if (ImGui::Button("OK", bsz)) g_msgBox.result = IDOK;
    }

    ImGui::End();
    ImGui::PopStyleColor(3);

    // A deferred box outlives the call that raised it, so the button press is
    // what closes it. Without this it would be drawn for the rest of the
    // session: the spin loop that normally calls orbiter_ClearMessageBox was
    // the very thing that could not be run.
    if (g_msgBox.deferred && g_msgBox.result != 0)
        g_msgBox.active = false;
}

// ---------------------------------------------------------------------------
// Keyboard message injection
//
// Nothing else posts keyboard messages. ImGui takes GLFW input through its own
// callbacks, so unless key events are turned back into WM_KEYDOWN and queued,
// Orbiter's dialog keyboard handling never runs at all:
// LaunchpadDialog::ConsumeMessage passes every message to IsDialogMessage,
// which is what implements Tab order, Enter for the default button, Escape for
// cancel and Alt+mnemonic. With nothing arriving, all of that is dead code.
//
// Keys are read from ImGui rather than from a GLFW callback because ImGui
// already owns the callback chain; installing another would displace it.
// ---------------------------------------------------------------------------

struct KeyMapping { ImGuiKey imgui; int vkey; };

const KeyMapping kDialogKeys[] = {
    { ImGuiKey_Tab,         VK_TAB },
    { ImGuiKey_Enter,       VK_RETURN },
    { ImGuiKey_KeypadEnter, VK_RETURN },
    { ImGuiKey_Escape,      VK_ESCAPE },
    { ImGuiKey_Space,       VK_SPACE },
    { ImGuiKey_UpArrow,     VK_UP },
    { ImGuiKey_DownArrow,   VK_DOWN },
    { ImGuiKey_LeftArrow,   VK_LEFT },
    { ImGuiKey_RightArrow,  VK_RIGHT },
    { ImGuiKey_Home,        VK_HOME },
    { ImGuiKey_End,         VK_END },
    { ImGuiKey_PageUp,      VK_PRIOR },
    { ImGuiKey_PageDown,    VK_NEXT },
    { ImGuiKey_Delete,      VK_DELETE },
    { ImGuiKey_Backspace,   VK_BACK },
    { ImGuiKey_F1,          VK_F1 },
};

void postKeyboardMessages()
{
    // WHY TAB IS TRACED SEPARATELY. A run with four real Tab presses produced
    // four WM_KEYUP messages (0x0101, wParam 9) and NOT ONE WM_KEYDOWN, so
    // IsDialogMessageA -- which acts only on WM_KEYDOWN/WM_CHAR/WM_SYSKEYDOWN
    // -- never saw them and the Launchpad's focus stayed at -1 for the whole
    // run. The release fires and the press does not, which narrows the cause
    // to something that consumes the DOWN edge specifically.
    if (getenv("ORBITER_TRACE_MSG")) {
        ImGuiIO &io = ImGui::GetIO();
        static bool wasDown = false;
        const bool down = ImGui::IsKeyDown(ImGuiKey_Tab);
        if (down != wasDown) {
            wasDown = down;
            fprintf(stderr, "[kbd] Tab down=%d pressed=%d released=%d | "
                            "WantTextInput=%d WantCaptureKeyboard=%d "
                            "NavActive=%d target=%p\n",
                    (int)down, (int)ImGui::IsKeyPressed(ImGuiKey_Tab, false),
                    (int)ImGui::IsKeyReleased(ImGuiKey_Tab),
                    (int)io.WantTextInput, (int)io.WantCaptureKeyboard,
                    (int)io.NavActive, (void *)orbiter_ActiveDialog());
        }
    }

    // A text field has focus: ImGui owns those keystrokes, and forwarding them
    // would let Tab move the dialog focus out from under an edit in progress.
    if (ImGui::GetIO().WantTextInput) return;

    // Keyboard goes to the active dialog, not to whichever one happens to be
    // first in an address-ordered container.
    HWND target = orbiter_ActiveDialog();
    if (!target) return;

    // AND IT MUST BE VISIBLE. This is a real defect, measured rather than
    // guessed at.
    //
    // orbiter_ActiveDialog() returns the topmost VISIBLE dialog, and when
    // there is none it falls back to g_topLevelOrder.back(). During a session
    // with no dialog open that fallback is the HIDDEN LAUNCHPAD, so every
    // Tab, Enter, Escape and arrow key was being posted into it. Measured with
    // real keys in a Delta-glider session (tests/dlgkbd.sh with a scenario):
    //
    //   dialog=171 focus=0  ->  1000  ->  9  ->  1001  ->  1259
    //
    // -- four Tab presses walking the tab order of a window the user cannot
    // see. Enter is the one that matters: IsDialogMessageA sends BN_CLICKED to
    // the BS_DEFPUSHBUTTON, which on the Launchpad is "Launch Orbiter", and
    // Escape sends IDCANCEL to it.
    //
    // The fallback itself is left alone: orbiter_PumpFrame relies on it to
    // route WM_CLOSE and WM_SIZE to the Launchpad, and those are correct.
    // What is wrong is using it as a KEYBOARD target -- a window nobody can
    // see has no business consuming keystrokes, and during a session those
    // keys belong to the simulation, which reads them through DirectInput
    // independently of this path.
    if (!IsWindowVisible(target)) return;

    for (const KeyMapping &k : kDialogKeys) {
        if (ImGui::IsKeyPressed(k.imgui, false))
            PostMessageA(target, WM_KEYDOWN, (WPARAM)k.vkey, 0);
        else if (ImGui::IsKeyReleased(k.imgui))
            PostMessageA(target, WM_KEYUP, (WPARAM)k.vkey, 0);
    }

    // Alt+letter is the mnemonic the '&' in a caption declares -- "&Launch
    // Orbiter" binds Alt+L. Windows delivers it as WM_SYSKEYDOWN.
    if (ImGui::GetIO().KeyAlt) {
        for (int c = 0; c < 26; ++c) {
            if (ImGui::IsKeyPressed((ImGuiKey)(ImGuiKey_A + c), false)) {
                if (getenv("ORBITER_TRACE_MSG"))
                    fprintf(stderr, "[msg] post WM_SYSKEYDOWN '%c'\n", 'A'+c);
                PostMessageA(target, WM_SYSKEYDOWN, (WPARAM)('A' + c), 0);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// MOUSE INPUT FOR THE SIMULATION, and it did not exist.
// ---------------------------------------------------------------------------
//
// Everything this file does with the mouse serves the DIALOGS: drawControl
// reads ImGui::GetIO().MousePos and posts WM_LBUTTONDOWN to a control. Nothing
// reached the RENDER WINDOW, so on this platform:
//
//   Orbiter::MsgProc                    never saw a mouse message
//     -> Orbiter::MouseEvent            never called
//        -> Camera::ProcessMouse        never called, so mbdown[] stayed false
//           -> Camera::UpdateMouse      ran every frame and did nothing,
//                                       because its whole body is under
//                                       `if (mbdown[1])`
//        -> Pane::ProcessMouse_*        never called: no panel, MFD or virtual
//                                       cockpit click could be delivered
//        -> clbkProcessMouse            no plugin ever saw a mouse event
//
// -- which is the entire mouse, dead, in the simulation. Right-drag to swing
// the external camera, the wheel to zoom or change FOV, and every clickable
// instrument were all inert.
//
// WHY POLLED HERE RATHER THAN FROM A GLFW CALLBACK. ImGui_ImplGlfw_InitForWindow
// was called with install_callbacks = true, so ImGui owns glfwSetMouseButtonCallback
// and friends and CHAINS to whatever was registered before it. Registering ours
// afterwards would displace ImGui's, not add to it. Reading ImGui's own IO
// state once per frame -- which its backend has just refreshed in
// ImGui_ImplGlfw_NewFrame -- is the same shape postKeyboardMessages already
// uses for the keyboard, and cannot fight the callback chain.
//
// WHY GATED ON WantCaptureMouse. On Windows the dialogs are separate OS
// windows, so a click on one simply never reaches the render window's
// procedure -- the window under the pointer gets the message and no policy is
// needed. Here every dialog is drawn INSIDE the render window, and
// io.WantCaptureMouse is exactly "the pointer is over ImGui's content". So it
// is the faithful translation of "which window is under the pointer", not an
// extra rule. Orbiter::MsgProc checks the same flag itself, which after this
// is belt and braces rather than the only guard.
//
// THE DRAG EXCEPTION. A press that began over the 3D view keeps routing there
// until it is released, even if the pointer crosses a dialog on the way. That
// is Win32's implicit capture between button-down and button-up, and without
// it a right-drag that strayed over an open dialog would lose its WM_RBUTTONUP
// and leave the camera stuck in rotation mode forever.

// Defined below, outside this namespace. Declared rather than moved because it
// creates the render window on first call, and the order of definitions in
// this file follows the lifecycle rather than the call graph.
extern "C" HWND orbiter_GetRenderWindow(void);

// ---------------------------------------------------------------------------
// Synthetic pointer state, for the scripted UI driver.
//
// THE SEAM IS DELIBERATE AND IT IS THE ONLY ONE. Everything below the raw
// position and button bits -- edge detection, the drag capture, the wParam
// packing, PostMessage, the dispatch into RenderWndProc, Orbiter::MsgProc,
// MouseEvent, Camera::ProcessMouse -- runs exactly as it does for a hand on a
// mouse. Only the source of "where is the pointer and what is held" is
// replaced, which is the one thing a test cannot supply any other way:
// synthetic X11 pointer events do not reach the GLFW window on this desktop,
// which is the whole reason Linux/UiDriver.cpp exists.
//
// Inert until orbiter_InjectMouse is called, and nothing calls it unless
// ORBITER_UI_SCRIPT names a script.
bool  g_injectMouse   = false;
float g_injX = 0, g_injY = 0;
int   g_injButtons     = 0;      // bit 0 left, 1 right, 2 middle
float g_injWheel       = 0.0f;

void postMouseMessages()
{
    // Only while a session owns the window. Before that the Launchpad is the
    // content and orbiter_GetRenderWindow() would CREATE a render window that
    // nothing has asked for.
    if (!g_sessionActive) return;

    HWND target = orbiter_GetRenderWindow();
    if (!target) return;

    ImGuiIO &io = ImGui::GetIO();

    const bool inject = g_injectMouse;

    // THE POSITION POSTED MUST BE THE ONE Camera::UpdateMouse READS, bias and
    // all, and that is not a detail -- it is what makes the recentring work.
    //
    // Camera::ProcessMouse sets mx,my from every WM_MOUSEMOVE; UpdateMouse
    // then computes dx = GetCursorPos() - mx. The warp closes that loop: it
    // moves the reported pointer back, this posts the corrective WM_MOUSEMOVE
    // for the new position, and mx resynchronises to it. Post the RAW position
    // while the camera reads a biased one and the two drift apart by exactly
    // the bias, so dx comes out zero and the camera never turns.
    //
    // On a platform that can really warp, the bias is always zero and this is
    // the raw position anyway.
    const float mx = inject ? (g_injX + (float)g_cursorBiasX) : (io.MousePos.x + (float)g_cursorBiasX);
    const float my = inject ? (g_injY + (float)g_cursorBiasY) : (io.MousePos.y + (float)g_cursorBiasY);
    const bool  btn[3] = {
        inject ? ((g_injButtons & 1) != 0) : io.MouseDown[0],
        inject ? ((g_injButtons & 2) != 0) : io.MouseDown[1],
        inject ? ((g_injButtons & 4) != 0) : io.MouseDown[2],
    };
    const float wheel = inject ? g_injWheel : io.MouseWheel;
    g_injWheel = 0.0f;                       // one notch per injection

    // Which buttons this window is currently dragging with. Static because a
    // drag spans frames; that is what makes it a capture.
    static bool captured[3] = { false, false, false };
    const bool anyCaptured = captured[0] || captured[1] || captured[2];

    // WantCaptureMouse answers "is the REAL pointer over a dialog", and an
    // injected run has no real pointer -- it sits wherever the harness parked
    // it. Skipped there, and only there.
    if (!inject && io.WantCaptureMouse && !anyCaptured) {
        // The pointer is over a dialog and no drag is in flight. Releases
        // still have to be delivered, but there are none outstanding, so
        // there is nothing to do -- and the previous position is left alone
        // so the first move back over the view is not read as a huge jump.
        return;
    }

    // ===================================================================
    // A BUTTON EDGE IS NEVER DROPPED FOR WANT OF A POSITION.
    // ===================================================================
    //
    // This used to be
    //
    //     if (mx < 0.0f || my < 0.0f) return;
    //
    // -- the whole frame abandoned, BEFORE the press/release loop below.
    // The reasoning was right about the position and wrong about the return:
    // ImGui reports -FLT_MAX for "outside the window" and LOWORD of that is
    // not a coordinate, so it must not be packed into an lParam. But dropping
    // the frame drops the button transition with it, and a release is the one
    // message that must never be lost.
    //
    // It is lost exactly when it matters. Camera::ProcessMouse answers a
    // WM_RBUTTONDOWN by entering rotation mode, which is ShowCursor(FALSE) +
    // ClipCursor -- GLFW_CURSOR_DISABLED here -- and in that mode GLFW
    // reports an UNBOUNDED VIRTUAL position that goes negative as soon as the
    // hand moves up or left of where the drag began. So: right-drag upwards,
    // release, and the WM_RBUTTONUP is thrown away here. mbdown[1] stays set,
    // Camera::UpdateMouse keeps recentring the pointer every frame, and the
    // cursor stays captured -- the simulator appears to freeze, and even the
    // keyboard looks dead because the pointer is grabbed. Reproduced twice
    // before it was understood.
    //
    // The reference names this failure in the line this code replaces:
    // ClipCursor is called with the comment "so we don't miss the button up
    // event" (Win32Dlg.cpp:2608 quotes it). Windows confines the pointer so
    // the release cannot escape; the equivalent duty here is to deliver the
    // edge whatever the position reads.
    //
    // So: an unusable position falls back to the last usable one, which is
    // where Windows would have pinned the pointer anyway, and only the
    // MOTION message is suppressed.
    static int lastGoodX = 0, lastGoodY = 0;
    const bool posUsable = (mx >= 0.0f && my >= 0.0f &&
                            mx < 65536.0f && my < 65536.0f);
    if (posUsable) { lastGoodX = (int)mx; lastGoodY = (int)my; }

    const int x = posUsable ? (int)mx : lastGoodX;
    const int y = posUsable ? (int)my : lastGoodY;
    const LPARAM pos = MAKELPARAM(x, y);

    // The virtual-key state word Win32 puts in wParam.
    WPARAM mk = 0;
    if (btn[0])      mk |= MK_LBUTTON;
    if (btn[1])      mk |= MK_RBUTTON;
    if (btn[2])      mk |= MK_MBUTTON;
    if (io.KeyShift) mk |= MK_SHIFT;
    if (io.KeyCtrl)  mk |= MK_CONTROL;

    // Motion first, so a click is delivered at a position the target has
    // already been told about -- the order Windows generates them in.
    static int lastX = -1, lastY = -1;
    if (posUsable && (x != lastX || y != lastY)) {
        lastX = x; lastY = y;
        PostMessageA(target, WM_MOUSEMOVE, mk, pos);
    }

    static const UINT kDown[3] = { WM_LBUTTONDOWN, WM_RBUTTONDOWN, WM_MBUTTONDOWN };
    static const UINT kUp[3]   = { WM_LBUTTONUP,   WM_RBUTTONUP,   WM_MBUTTONUP   };

    for (int b = 0; b < 3; ++b) {
        if (btn[b] == captured[b]) continue;
        captured[b] = btn[b];
        PostMessageA(target, btn[b] ? kDown[b] : kUp[b], mk, pos);
    }

    // THE EMULATED WARP ENDS WITH THE DRAG. Outside a drag the reported cursor
    // must be the real one: Panel2D::GetMouseState, VCockpit, Panel and
    // Defpanel all read GetCursorPos to follow a finger held on a switch, and
    // a stale offset would put every one of those at the wrong place. This is
    // the interval Windows has the pointer pinned for, and nothing wider.
    if (!btn[0] && !btn[1] && !btn[2])
        g_cursorBiasX = g_cursorBiasY = 0.0;

    // The wheel. WM_MOUSEWHEEL's delta lives in the HIGH word of wParam in
    // multiples of WHEEL_DELTA, and its sign convention matches GLFW's: away
    // from the user is positive in both.
    //
    // Orbiter::MsgProc calls ScreenToClient on this one message, because on
    // Windows it carries screen coordinates. Here the render window's origin
    // is (0,0) and it has no parent, so Win32Dlg's ScreenToClient subtracts
    // nothing and the client coordinates pass through unchanged. Sending
    // anything else would be inventing a second coordinate system to satisfy
    // a conversion that is already a no-op.
    if (wheel != 0.0f) {
        const short zDelta = (short)(wheel * WHEEL_DELTA);
        if (zDelta)
            PostMessageA(target, WM_MOUSEWHEEL,
                         MAKEWPARAM((WORD)mk, (WORD)zDelta), pos);
    }

    if (getenv("ORBITER_TRACE_MSG")) {
        // Position changes are reported too, not just buttons and the wheel.
        // A test harness has no other way to learn where the client thinks
        // the pointer is, and without it the only way to aim a click at a
        // cockpit control is to assume the window has no frame inset --
        // which silently puts every click somewhere else if it has one.
        static int prevBtn = -1, prevX = -1, prevY = -1;
        const int nowBtn = (btn[0]?1:0) | (btn[1]?2:0) | (btn[2]?4:0);
        if (nowBtn != prevBtn || wheel != 0.0f || x != prevX || y != prevY) {
            prevBtn = nowBtn; prevX = x; prevY = y;
            fprintf(stderr, "[mouse] -> render wnd  pos=(%d,%d) btn=%d "
                            "wheel=%.1f inject=%d\n",
                    x, y, nowBtn, wheel, int(inject));
        }
    }
}

// Defined at the bottom of this file, next to orbiter_UploadTexture, which is
// the same code without the out-parameters. The splash needs the handles back
// because it is the one texture here that is DESTROYED: a dialog bitmap lives
// for the process lifetime, but a splash is ten megabytes and a session can
// be started and closed all afternoon.
VkDescriptorSet uploadTextureEx(const unsigned char *rgba, int width, int height,
                                VkImage *outImage, VkDeviceMemory *outMemory,
                                VkImageView *outView);

// ---------------------------------------------------------------------------
// The splash screen, drawn. See the note by SplashState.
//
// This is the port of the three lines D3D9Client::SplashScreen() and
// OutputLoadStatus() end with, and of nothing else -- every coordinate here
// was computed by the client from the reference's own expressions and is used
// exactly as given.
//
// The layout, from OutputLoadStatus:
//
//     TextOut(hDC, 2,  2, pLoadLabel, ...)     with hLblFont1 (24 + x)
//     TextOut(hDC, 2, 36, pLoadItem,  ...)     with hLblFont2 (18 + x)
//     MoveToEx(hDC, 0, 32, NULL); LineTo(hDC, loadd_w, 32)
//
// -- all three relative to the top-left of the status panel, because the
// reference draws them into pTextScreen, a surface of exactly that size, and
// then blits it to (loadd_x, loadd_y). Here there is no intermediate surface,
// so the panel origin is added at the point of use.
void drawSplash()
{
    // THE REFERENCE'S GATE IS `!bRunning`, AND THIS IS IT.
    //
    //     bool D3D9Client::clbkDisplayFrame()
    //     {
    //         if (!bRunning && pDevice) {
    //             RECT txt = _RECT(loadd_x, loadd_y, ...);
    //             pDevice->StretchRect(pSplashScreen, NULL, pBackBuffer, NULL, ...);
    //             pDevice->StretchRect(pTextScreen,   NULL, pBackBuffer, &txt, ...);
    //         }
    //         ...
    //
    // The splash is re-blitted over the back buffer on EVERY presented frame
    // for as long as the client is loading, and stops the instant
    // clbkPostCreation raises bRunning -- which is also the instant the scene
    // becomes drawable. There is no moment in between with neither on screen.
    //
    // g_sessionActive is this side's bRunning: clbkPostCreation calls
    // orbiter_BeginSession() at the same line the reference sets the flag.
    // Gating here rather than tearing the splash state down there is what
    // matches the reference, whose pSplashScreen lives untouched until
    // clbkDestroyRenderWindow releases it (D3D9Client.cpp:921).
    //
    // WHY THAT MATTERED, and it was not about the picture. clbkPostCreation
    // used to call orbiter_ClearSplash(), which cleared g_splash.active while
    // g_sessionActive was still false -- and the two of them are exactly what
    // the pump's `anyVisible` is built from. So for the gap between the
    // scenario finishing loading and the first scene frame arriving, nothing
    // wanted the window, and the pump did what it is told to do with a window
    // nothing wants: glfwHideWindow. The splash vanished to the desktop, and
    // then the first scene frame re-showed the window -- and an X11 window
    // that is unmapped and mapped again is PLACED AND FITTED AGAIN by the
    // window manager. On this desktop that turned a 2552x1408 window into a
    // maximised 2560x1372 one at 1920,24, two seconds after Orbiter had
    // already read 1408 and laid the entire generic cockpit out from it.
    //
    // Windows never has the gap because it never has the unmap: the render
    // window is WS_VISIBLE from CreateWindow and stays mapped for the whole
    // session.
    if (g_sessionActive) return;
    if (!g_splash.active) return;

    ImDrawList *dl = ImGui::GetBackgroundDrawList();
    if (!dl) return;

    ImFont *font = g_splashFont ? g_splashFont : ImGui::GetFont();

    if (g_splash.set != VK_NULL_HANDLE && g_splash.imgW > 0 && g_splash.imgH > 0)
        dl->AddImage((ImTextureID)g_splash.set,
                     ImVec2((float)g_splash.imgX, (float)g_splash.imgY),
                     ImVec2((float)(g_splash.imgX + g_splash.imgW),
                            (float)(g_splash.imgY + g_splash.imgH)));

    for (int i = 0; i < 3; ++i) {
        if (g_splash.info[i].empty()) continue;
        dl->AddText(font, (float)g_splash.infoSize,
                    ImVec2((float)g_splash.infoX,
                           (float)(g_splash.infoY + i * g_splash.infoSpacing)),
                    g_splash.colour, g_splash.info[i].c_str());
    }

    if (g_splash.hasStatus) {
        if (!g_splash.label.empty())
            dl->AddText(font, (float)g_splash.labelSize,
                        ImVec2((float)(g_splash.statusX + 2),
                               (float)(g_splash.statusY + 2)),
                        g_splash.colour, g_splash.label.c_str());

        // The rule between the two lines. PS_SOLID width 1, and ImGui's line
        // is centred on the coordinate, so the half-pixel offset puts it on
        // the same row GDI would.
        dl->AddLine(ImVec2((float)g_splash.statusX,
                           (float)g_splash.statusY + 32.5f),
                    ImVec2((float)(g_splash.statusX + g_splash.statusW),
                           (float)g_splash.statusY + 32.5f),
                    g_splash.colour, 1.0f);

        if (!g_splash.item.empty())
            dl->AddText(font, (float)g_splash.itemSize,
                        ImVec2((float)(g_splash.statusX + 2),
                               (float)(g_splash.statusY + 36)),
                        g_splash.colour, g_splash.item.c_str());
    }
}

} // namespace

extern "C" void orbiter_ShowMessageBox(const char *text, const char *caption,
                                       unsigned type, int deferred)
{
    const std::string t = text ? text : "";
    const std::string c = (caption && *caption) ? caption : "Orbiter";

    // AN ALREADY-STANDING DEFERRED BOX IS NOT RE-RAISED.
    //
    // A deferred box is raised by a caller that then returns, so a module
    // which raises it from a per-frame path -- a dialog's OnDraw, a timestep
    // -- reaches here again on the next frame with the same text. Resetting
    // result to 0 each time would discard the click the user had just made
    // and the box could never be dismissed. Same box, still unanswered:
    // leave it alone.
    if (g_msgBox.active && g_msgBox.deferred && deferred &&
        g_msgBox.text == t && g_msgBox.caption == c && g_msgBox.type == type)
        return;

    g_msgBox.active   = true;
    g_msgBox.text     = t;
    g_msgBox.caption  = c;
    g_msgBox.type     = type;
    g_msgBox.result   = 0;
    g_msgBox.deferred = (deferred != 0);
}

extern "C" int  orbiter_MessageBoxResult(void) { return g_msgBox.result; }
extern "C" void orbiter_ClearMessageBox(void)
{
    g_msgBox.active   = false;
    g_msgBox.deferred = false;
}

// ---------------------------------------------------------------------------
// GPU crash diagnostics, exported for the graphics client. The bodies are up
// beside check(); these are the wrappers with external linkage.
// ---------------------------------------------------------------------------
extern "C" int  orbiter_HasVulkanCheckpoints(void) { return g_hasCheckpoints ? 1 : 0; }
extern "C" int  orbiter_HasVulkanDeviceFault(void) { return g_hasDeviceFault ? 1 : 0; }
extern "C" void orbiter_VkCheckpoint(void *cmdBuf, const char *tag)
{
    vkCheckpoint(cmdBuf, tag);
}
extern "C" void orbiter_DumpGpuCheckpoints(const char *where)
{
    dumpGpuCheckpoints(where ? where : "(client)");
}

// ===========================================================================
// The pump
//
// Called from Win32Dlg.cpp whenever Orbiter asks for a message and the queue
// is empty. One call is one frame.
// ===========================================================================

// Publish the Vulkan context for the graphics client. See the declaration
// above for why the client shares this device rather than making its own.
//
// initialise() is called first because the client may ask before the
// Launchpad has drawn a frame; it is idempotent.
extern "C" int orbiter_GetVulkanContext(OrbiterVulkanContext *out)
{
    if (!out) return 0;
    if (!initialise()) return 0;

    out->instance       = (void *)g_instance;
    out->physicalDevice = (void *)g_physicalDevice;
    out->device         = (void *)g_device;
    out->queue          = (void *)g_queue;
    out->queueFamily    = g_queueFamily;
    out->descriptorPool = (void *)g_descriptorPool;
    out->renderPass     = (void *)g_mainWindowData.RenderPass;
    out->window         = (void *)g_window;
    out->minImageCount  = (unsigned)g_minImageCount;
    out->imageCount     = (unsigned)g_mainWindowData.ImageCount;
    return 1;
}

// ---------------------------------------------------------------------------
// The render window, and the scene frame boundary
//
// A graphics client renders into the window UIHost already owns -- the same
// one the Launchpad draws in -- rather than opening a second one. That keeps a
// single Vulkan surface and swapchain, lets ImGui dialogs composite over the
// rendered scene, and makes the move from Launchpad to session seamless.
//
// clbkCreateRenderWindow must return an HWND, so a Window is created to stand
// for the GLFW window. It is a real entry in the dialog layer, which is what
// lets GetClientRect, SetWindowText and the message pump work on it as they do
// on Windows.
// ---------------------------------------------------------------------------

// The true framebuffer size, which is what the client must report as its
// viewport.
//
// The render window is a dialog-layer object created once, and its stored
// size does not follow the GLFW window when the user resizes it. Orbiter lays
// the HUD and MFDs out from the viewport size, so a stale value squeezes the
// whole 2D output into a corner -- which is exactly what a resize produced.
// Resize the window for a session.
//
// On Windows clbkCreateRenderWindow creates a window at the resolution
// configured on the Video tab. Here the session reuses the Launchpad's
// window, which is sized for a dialog -- 600x541 -- so the render area was
// far smaller than the user asked for and the HUD was laid out to match.
// Told by the graphics client which DC holds the composited 2D output.
// The Launchpad window's cursor position, for the Win32 shim's GetCursorPos.
// Kept here because this file owns the GLFW window; Win32Dlg.cpp deals only in
// the dialog layer and has no business including GLFW.
extern "C" void orbiter_GetHostCursorPos(int *x, int *y)
{
    if (x) *x = 0;
    if (y) *y = 0;

    // Under injection THIS is the pointer, not the physical one.
    //
    // Camera::UpdateMouse works from GetCursorPos, not from the coordinates
    // in the mouse message, so a scripted drag that only replaced the message
    // source would rotate the camera by the movement of the REAL pointer --
    // which is parked on another monitor and not moving. The two have to be
    // the same pointer or the test measures the wrong hand.
    // The injected pointer is the RAW one, and the bias applies on top of it
    // exactly as it does to the real one -- which is what makes the scripted
    // test exercise the emulated warp rather than a path of its own.
    double cx = 0, cy = 0;
    if (g_injectMouse) {
        cx = g_injX;
        cy = g_injY;
    } else {
        if (!g_window) return;
        glfwGetCursorPos(g_window, &cx, &cy);
    }

    // The bias is zero except between a warp request and the end of that
    // drag; see the note by g_cursorBiasX.
    if (x) *x = int(cx + g_cursorBiasX);
    if (y) *y = int(cy + g_cursorBiasY);
}

// And the matching setter, which had no implementation at all.
//
// Camera::UpdateMouse RECENTRES THE POINTER EVERY FRAME while the right
// button is held:
//
//     dx = pt.x - mx;  dy = pt.y - my;
//     SetCursorPos (x0-dx, y0-dy);
//
// so that the next GetCursorPos reads only NEW movement. Win32Dlg.cpp's
// SetCursorPos forwarded to a graphics-client hook and, when no client had
// registered one -- which is every client this port has -- did nothing.
// The pointer was never warped back, so dx measured the distance from where
// the drag STARTED rather than the movement since the last frame, and the
// camera accelerated away the further the hand travelled.
//
// glfwSetCursorPos takes window coordinates, which is the same space
// orbiter_GetHostCursorPos reports and the same space the render window uses
// (its origin is 0,0, so Win32Dlg's ScreenToClient is a no-op on it). One
// coordinate system throughout, which is why no conversion appears here.
extern "C" void orbiter_SetHostCursorPos(int x, int y)
{
    // The OFFSET moves instead of the pointer:
    //     reported = raw + bias,   and the caller wants reported == (x,y)
    // so bias = (x,y) - raw. The raw pointer carries on wherever the hand
    // takes it -- and under GLFW_CURSOR_DISABLED it cannot leave the window,
    // which is the other half of what Windows arranges here. Each frame's warp
    // cancels that frame's movement, keeping the reported pointer pinned
    // exactly as a real warp would.
    double cx = 0, cy = 0;
    if (g_injectMouse) {
        cx = g_injX;
        cy = g_injY;
    } else {
        if (!g_window) return;
        glfwGetCursorPos(g_window, &cx, &cy);
    }
    g_cursorBiasX = double(x) - cx;
    g_cursorBiasY = double(y) - cy;
}

// ShowCursor and ClipCursor, from the Win32 shim. See applyCursorMode.
extern "C" void orbiter_SetCursorVisible(int visible)
{
    g_cursorVisible = (visible != 0);
    applyCursorMode();
}

extern "C" void orbiter_SetCursorConfined(int confined)
{
    g_cursorConfined = (confined != 0);
    applyCursorMode();
}

extern "C" void orbiter_SetSceneDC(void *dc)
{
    g_sceneDC = (HDC)dc;
}

extern "C" void orbiter_SetWindowSize(int w, int h)
{
    if (!initialise() || w <= 0 || h <= 0) return;

    glfwSetWindowSize(g_window, w, h);

    // Wait for the compositor to actually apply it.
    //
    // On Wayland a resize is a REQUEST, granted asynchronously -- and GLFW
    // reports the requested size back immediately, before the surface has
    // changed. Trusting that had Orbiter lay the HUD and MFDs out for
    // 1280x800 while the real surface was still the Launchpad's 600x541, so
    // the 2D output was drawn beyond the visible area entirely.
    //
    // Events must be pumped for the compositor's reply to arrive. Capped at
    // roughly half a second: a compositor that refuses the size is entitled
    // to, and the caller then gets the truth rather than a hang.
    for (int i = 0; i < 50; ++i) {
        glfwPollEvents();
        int aw = 0, ah = 0;
        glfwGetFramebufferSize(g_window, &aw, &ah);
        if (aw == w && ah == h) break;
        struct timespec ts { 0, 10 * 1000 * 1000 };
        nanosleep(&ts, nullptr);
    }

    // The swapchain must follow, or the next frame is presented at the old
    // extent and the image is stretched.
    g_swapChainRebuild = true;
}

extern "C" void orbiter_GetFramebufferSize(int *w, int *h)
{
    if (!w || !h) return;
    *w = *h = 0;
    if (!initialise()) return;
    glfwGetFramebufferSize(g_window, w, h);
}

// Pump the WINDOW EVENT QUEUE only -- no frame, no present, no ImGui.
//
// orbiter_PumpFrame renders and presents a whole frame, so a client cannot
// call it from inside a callback the pump itself dispatched; this can be
// called from anywhere, because it only lets the window manager deliver
// configure events that GLFW has been waiting for.
//
// It exists because a window manager decides a window's client area
// ASYNCHRONOUSLY, and anything that needs to see the result has to let the
// events through first. Win32 has no equivalent problem: CreateWindow with
// AdjustWindowRect yields the exact client size synchronously, before
// anything is shown.
extern "C" void orbiter_PollWindowEvents(void)
{
    if (!initialise()) return;
    glfwPollEvents();
}

// The extent vkCreateSwapchainKHR will use, taken from the surface rather
// than from GLFW's last configure event.
//
// WHAT THIS IS NOT. It was added to answer "how big will the window really
// be" before the window manager has said so, and IT DOES NOT ANSWER THAT.
// Measured on this desktop, with Orbiter.cfg asking for a 2560x1440 window on
// a 2560x1440 monitor, at the moment clbkCreateRenderWindow runs:
//
//     glfwGetFramebufferSize                     2560 x 1440
//     surface currentExtent (this function)      2560 x 1440
//     swapchain, frame 0                         2560 x 1440
//     swapchain, frame 1 onwards                 2560 x 1372
//
// 1372 is the client area the WM finally granted, 68 pixels going to the
// title bar -- and that clamping is correct and deliberate, as the "Force
// window size" note in orbiter_SetDisplayMode says. But it does not exist
// anywhere, in GLFW or in Vulkan, until the first frame is PRESENTED: that
// present returns suboptimal, the swapchain is rebuilt, and the reconfigure
// follows. There is no query that can be asked earlier, because the answer
// has not been produced yet.
//
// It is kept because it is still the right question for anything that has to
// agree with the swapchain, and because this measurement belongs somewhere a
// reader will find it before repeating the experiment.
extern "C" void orbiter_GetSurfaceExtent(int *w, int *h)
{
    if (!w || !h) return;
    *w = *h = 0;
    if (!initialise()) return;

    ImGui_ImplVulkanH_Window *wd = &g_mainWindowData;
    if (g_physicalDevice != VK_NULL_HANDLE && wd->Surface != VK_NULL_HANDLE) {
        VkSurfaceCapabilitiesKHR caps{};
        if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_physicalDevice,
                                                      wd->Surface, &caps) == VK_SUCCESS) {
            // 0xFFFFFFFF means "the surface has no preferred size, the
            // swapchain decides" -- a legitimate answer on some platforms,
            // and the only case where GLFW's number is the better one.
            if (caps.currentExtent.width  != 0xFFFFFFFFu &&
                caps.currentExtent.height != 0xFFFFFFFFu &&
                caps.currentExtent.width  != 0 &&
                caps.currentExtent.height != 0) {
                *w = (int)caps.currentExtent.width;
                *h = (int)caps.currentExtent.height;
                return;
            }
        }
    }

    glfwGetFramebufferSize(g_window, w, h);
}

// ---------------------------------------------------------------------------
// DISPLAY ENUMERATION AND MODE SELECTION FOR THE LAUNCHPAD VIDEO TAB
// ---------------------------------------------------------------------------
//
// On Windows the graphics client fills the Video tab itself: D3D9Client's
// VideoTab::Initialise calls IDirect3D9::GetAdapterCount for the "3D device"
// combo and EnumAdapterModes for "Screen resolution". Vulkan has no
// counterpart to either. vkEnumeratePhysicalDevices does give the device list
// -- the client can and does call it, since it holds the instance -- but
// nothing in Vulkan enumerates a MONITOR'S MODES. WSI reports what a surface
// can do, not what the display can be set to; modes belong to the window
// system, and the window system here is GLFW, which lives in this file. The
// client has no window, no monitor and no GLFW.
//
// So the mode list is exported instead of duplicated. The same applies to
// actually applying a choice: a fullscreen switch is glfwSetWindowMonitor on
// the window this file owns, and the swapchain that must follow it is also
// this file's.
//
// WHY THE LIST IS DE-DUPLICATED. glfwGetVideoModes returns one entry per
// (width, height, refresh, colour-depth) combination, and a modern monitor
// reports the same resolution at 24-bit and at 30-bit, sometimes at several
// refresh rates each. Left raw, "Screen resolution" lists 1920 x 1080 four
// times over and the user cannot tell the entries apart. Colour depth is not
// a choice the Video tab offers on this port -- the swapchain format is picked
// by ImGui_ImplVulkanH_SelectSurfaceFormat from what the surface supports --
// so (width, height, refresh) is the whole of what distinguishes a mode here.

namespace {

struct HostVideoMode { int w, h, hz; };
std::vector<HostVideoMode> g_videoModes;
bool                       g_videoModesBuilt = false;

void buildVideoModeList()
{
    if (g_videoModesBuilt) return;
    g_videoModesBuilt = true;          // set first: a monitor-less run must
                                       // not retry on every call

    // pickPrimaryMonitor, not glfwGetPrimaryMonitor: this list IS the Video
    // tab's "Screen resolution" combo, and enumerating the wrong monitor's
    // modes means the user's own display's resolution is not offered at all.
    // See the note at pickPrimaryMonitor.
    GLFWmonitor *mon = pickPrimaryMonitor();
    if (!mon) return;

    int count = 0;
    const GLFWvidmode *modes = glfwGetVideoModes(mon, &count);
    if (!modes) return;

    for (int i = 0; i < count; ++i) {
        const HostVideoMode m { modes[i].width, modes[i].height,
                                modes[i].refreshRate };
        bool dup = false;
        for (const HostVideoMode &e : g_videoModes) {
            if (e.w == m.w && e.h == m.h && e.hz == m.hz) { dup = true; break; }
        }
        if (!dup) g_videoModes.push_back(m);
    }

    // LARGEST FIRST, which is not the order GLFW returns.
    //
    // glfwGetVideoModes sorts ascending, so its index 0 is the SMALLEST mode
    // the monitor will accept -- 320 x 200 on the machine this was written on.
    // That is the one number that matters here, because Orbiter's default for
    // CFG_DEVPRM::Device_mode is 0 and it is stored as a plain index: any
    // config that has never had a resolution chosen in it selects index 0, and
    // ascending order turns "not configured" into "320 x 200". Descending
    // turns it into the native mode, which is the right answer to a question
    // nobody has answered.
    //
    // It also puts the modes a person is actually looking for at the top of a
    // 33-entry dropdown instead of the bottom.
    std::sort(g_videoModes.begin(), g_videoModes.end(),
              [](const HostVideoMode &a, const HostVideoMode &b) {
                  const long long aa = (long long)a.w * a.h;
                  const long long bb = (long long)b.w * b.h;
                  if (aa != bb) return aa > bb;
                  if (a.w != b.w) return a.w > b.w;
                  return a.hz > b.hz;
              });
}

} // namespace

extern "C" int orbiter_GetVideoModeCount(void)
{
    if (!initialise()) return 0;
    buildVideoModeList();
    return (int)g_videoModes.size();
}

extern "C" int orbiter_GetVideoMode(int idx, int *w, int *h, int *hz)
{
    if (!initialise()) return 0;
    buildVideoModeList();
    if (idx < 0 || (size_t)idx >= g_videoModes.size()) return 0;
    if (w)  *w  = g_videoModes[idx].w;
    if (h)  *h  = g_videoModes[idx].h;
    if (hz) *hz = g_videoModes[idx].hz;
    return 1;
}

// The primary monitor's WORK AREA -- the desktop minus whatever panels, docks
// or taskbars the compositor has reserved.
//
// This is the counterpart of SystemParametersInfo(SPI_GETWORKAREA, ...), and
// it is exported rather than answered inside Win32Dlg.cpp for the same reason
// the video-mode queries are: glfwGetMonitorWorkarea lives here, with the
// window and the monitor, and nothing else in the tree links GLFW. The
// fullscreen-window path a few hundred lines below already uses exactly this
// call and says so in its comment.
//
// Falls back to the monitor's position and current mode when the platform
// reports no work area, which is what a compositor without panels does.
extern "C" int orbiter_GetWorkArea(int *x, int *y, int *w, int *h)
{
    if (!initialise()) return 0;
    GLFWmonitor *mon = pickPrimaryMonitor();
    if (!mon) return 0;

    int mx = 0, my = 0, mw = 0, mh = 0;
    glfwGetMonitorWorkarea(mon, &mx, &my, &mw, &mh);
    if (mw <= 0 || mh <= 0) {
        const GLFWvidmode *cur = glfwGetVideoMode(mon);
        if (!cur) return 0;
        glfwGetMonitorPos(mon, &mx, &my);
        mw = cur->width; mh = cur->height;
    }
    if (x) *x = mx;
    if (y) *y = my;
    if (w) *w = mw;
    if (h) *h = mh;
    return 1;
}

// The monitor's mode as it is right now, which is what a fullscreen window
// must match and what the Video tab should preselect when the stored index is
// unusable.
extern "C" int orbiter_GetCurrentVideoMode(int *w, int *h, int *hz)
{
    if (!initialise()) return 0;
    GLFWmonitor *mon = pickPrimaryMonitor();
    if (!mon) return 0;
    const GLFWvidmode *m = glfwGetVideoMode(mon);
    if (!m) return 0;
    if (w)  *w  = m->width;
    if (h)  *h  = m->height;
    if (hz) *hz = m->refreshRate;
    return 1;
}

// VIDEODATA::outputidx, handed over by the client.
//
// Placed here rather than beside the other window exports because it has to
// invalidate g_videoModesBuilt, which lives in the anonymous namespace above:
// the mode list is THIS MONITOR's modes, built lazily on the Video tab's first
// query, so changing the monitor after it has been built would leave the combo
// showing the other display's resolutions.
//
// The client calls this from VulkanDevice::Initialize, before
// orbiter_SetDisplayMode. See pickPrimaryMonitor for what the index means and
// why 0 does not mean monitor 0.
extern "C" void orbiter_SetOutputIndex(int idx)
{
    if (idx == g_outputIndex) return;
    g_outputIndex = idx;
    g_videoModesBuilt = false;
    g_videoModes.clear();
}

// Apply a display configuration.
//
// ALL THREE STYLES ARE FULLSCREEN STYLES. That is easy to get wrong -- it was
// got wrong here first -- because the third one is called "Window with
// Taskbar". The Video tab's layout says which is which: the style combo lives
// under the "Full Screen" radio and the reference disables it entirely when
// "Window" is selected. So `fullscreen` and `style` are two separate things
// and both are needed:
//
//   fullscreen = 0            a plain window of w x h. style is irrelevant.
//   fullscreen = 1, style 0   true fullscreen: the monitor's mode is set to
//                             w x h @ hz. No alt-tab.
//   fullscreen = 1, style 1   the whole monitor, undecorated, no mode change.
//   fullscreen = 1, style 2   the monitor's WORK AREA, undecorated -- which is
//                             what leaves the taskbar (or panel, or dock)
//                             visible.
//
// spanDisplays is the reference's `vData->pageflip`, which it uses to widen
// styles 1 and 2 from SM_CXSCREEN to SM_CXVIRTUALSCREEN -- the whole desktop
// across every monitor rather than the primary one. That is why D3D9Client's
// VideoTab relabels IDC_VID_PAGEFLIP "Multiple displays".
//
// forceSize is the reference's `vData->trystencil`, relabelled there "Force
// window size". It is the SWP_NOSENDCHANGING flag on its SetWindowPos: put the
// window at exactly the requested size, past whatever would otherwise clamp
// it. Windowed only, and see the note where it is used for what "whatever
// would otherwise clamp it" is here.
extern "C" void orbiter_SetDisplayMode(int fullscreen, int style,
                                       int spanDisplays, int forceSize,
                                       int w, int h, int hz)
{
    if (!initialise()) return;

    GLFWmonitor *mon = pickPrimaryMonitor();
    if (!mon && fullscreen) {
        fprintf(stderr, "Orbiter: no monitor reported; staying windowed\n");
        fullscreen = 0;
    }

    // What this desktop actually offers, printed once, because two separate
    // fixes have now been written that could not run on it and said nothing.
    // glfwGetWindowFrameSize returns zeros where there is no client-side
    // decoration to measure, and glfwSetWindowPos does nothing at all on
    // Wayland -- so a placement or client-area correction can be entirely
    // correct and entirely inert, and the only symptom is the original bug.
    {
        int fl = 0, ft = 0, fr = 0, fb = 0;
        glfwGetWindowFrameSize(g_window, &fl, &ft, &fr, &fb);
        int wx = 0, wy = 0, ww = 0, wh = 0;
        if (platformHasWindowPos()) glfwGetWindowPos(g_window, &wx, &wy);
        glfwGetWindowSize(g_window, &ww, &wh);

        fprintf(stderr,
                "Orbiter: [display] platform=%s canSetPos=%d "
                "window=%dx%d at %d,%d frame=%d/%d/%d/%d\n",
                glfwGetPlatform() == GLFW_PLATFORM_WAYLAND ? "wayland" :
                glfwGetPlatform() == GLFW_PLATFORM_X11     ? "x11" : "other",
                platformHasWindowPos() ? 1 : 0,
                ww, wh, wx, wy, fl, ft, fr, fb);

        int count = 0;
        GLFWmonitor **all = glfwGetMonitors(&count);
        for (int i = 0; i < count; ++i) {
            int mxx = 0, myy = 0, ax = 0, ay = 0, aw = 0, ah = 0;
            glfwGetMonitorPos(all[i], &mxx, &myy);
            glfwGetMonitorWorkarea(all[i], &ax, &ay, &aw, &ah);
            const GLFWvidmode *vm = glfwGetVideoMode(all[i]);
            fprintf(stderr,
                    "Orbiter: [display]   monitor %d%s \"%s\" mode=%dx%d "
                    "at %d,%d workarea=%dx%d at %d,%d\n",
                    i, all[i] == mon ? " (primary)" : "",
                    glfwGetMonitorName(all[i]) ? glfwGetMonitorName(all[i]) : "?",
                    vm ? vm->width : 0, vm ? vm->height : 0, mxx, myy,
                    aw, ah, ax, ay);
        }
    }

    // The size before anything is asked for, and the size that will be asked
    // for. Both are read by the settle loop at the end of this function; see
    // the long note there for why "the framebuffer size stopped changing" is
    // not a usable test on its own.
    int preW = 0, preH = 0;
    glfwGetFramebufferSize(g_window, &preW, &preH);
    int wantW = 0, wantH = 0;

    // SM_CXVIRTUALSCREEN and SM_XVIRTUALSCREEN have no GLFW equivalent, so the
    // bounding box of every connected monitor is computed here. Only the
    // horizontal extent is used, exactly as the reference uses it.
    int vx = 0, vw = 0;
    if (spanDisplays) {
        int count = 0;
        GLFWmonitor **all = glfwGetMonitors(&count);
        int left = 0, right = 0;
        for (int i = 0; i < count; ++i) {
            int mxx = 0, myy = 0;
            glfwGetMonitorPos(all[i], &mxx, &myy);
            const GLFWvidmode *vm = glfwGetVideoMode(all[i]);
            if (!vm) continue;
            if (i == 0 || mxx < left)            left  = mxx;
            if (i == 0 || mxx + vm->width > right) right = mxx + vm->width;
        }
        vx = left;
        vw = right - left;
        if (vw <= 0) spanDisplays = 0;
    }

    // Capture the windowed geometry the first time we leave the desktop, so
    // there is something to come back to. Asking after the switch returns the
    // monitor's size, which would make "Window" mean "fullscreen, undecorated"
    // for the rest of the run.
    if (!g_savedWndValid) {
        if (platformHasWindowPos())
            glfwGetWindowPos(g_window, &g_savedWndX, &g_savedWndY);
        glfwGetWindowSize(g_window, &g_savedWndW, &g_savedWndH);
        g_savedWndValid = true;
    }

    if (fullscreen && style <= 1) {
        // BOTH OF THESE ARE glfwSetWindowMonitor, AND THAT IS THE POINT.
        //
        // Style 1 was first written as "undecorated window positioned over the
        // monitor", built from glfwSetWindowAttrib(DECORATED, false) plus
        // SetWindowPos and SetWindowSize. It came back 2560 x 1036 on a
        // 2560 x 1440 monitor, and 1036 is not a number this code chose: it is
        // _NET_WORKAREA's height on this desktop, i.e. the screen minus the
        // panels. A managed window does not get to be taller than the work
        // area no matter what size it asks for, and removing its decoration
        // does not change that -- only _NET_WM_STATE_FULLSCREEN does, and the
        // only thing in GLFW that sets it is attaching a monitor.
        //
        // So borderless fullscreen is glfwSetWindowMonitor AT THE MONITOR'S
        // CURRENT MODE: the window goes truly fullscreen, and because the
        // requested mode is the one already set, no mode change happens and
        // alt-tab stays instant. That is the documented GLFW idiom for it.
        // The two styles then differ in exactly one thing -- whether a mode is
        // imposed -- which is what their names say.
        const GLFWvidmode *cur = glfwGetVideoMode(mon);
        if (style == 1 || w <= 0 || h <= 0) {
            if (!cur) return;
            w  = cur->width;
            h  = cur->height;
            hz = cur->refreshRate;
        }
        if (w <= 0 || h <= 0) return;

        // "Multiple displays" on style 1. The reference widens to
        // SM_CXVIRTUALSCREEN while keeping SM_CYSCREEN, so the window spans the
        // desktop horizontally at one monitor's height. A window that wide
        // cannot be a monitor-attached fullscreen window, so it falls back to
        // an undecorated window positioned across them.
        if (spanDisplays && vw > w) {
            if (glfwGetWindowMonitor(g_window))
                glfwSetWindowMonitor(g_window, nullptr, vx, 0, vw, h,
                                     GLFW_DONT_CARE);
            glfwSetWindowAttrib(g_window, GLFW_DECORATED, GLFW_FALSE);
            if (platformHasWindowPos()) glfwSetWindowPos(g_window, vx, 0);
            glfwSetWindowSize(g_window, vw, h);
            wantW = vw; wantH = h;
        } else {
            glfwSetWindowMonitor(g_window, mon, 0, 0, w, h,
                                 hz > 0 ? hz : GLFW_DONT_CARE);
            wantW = w; wantH = h;
        }
    }
    else if (fullscreen) {
        // Style 2, "Window with Taskbar". The reference is explicit about what
        // that means -- CD3DFramework9::Initialize, case 2:
        //
        //     RECT rect;
        //     SystemParametersInfo(SPI_GETWORKAREA, 0, &rect, 0);
        //     SetWindowLongA(hWnd, GWL_STYLE, WS_CLIPCHILDREN | WS_VISIBLE);
        //     int x = GetSystemMetrics(SM_CXSCREEN);
        //     if (vData->pageflip) x = rect.right - rect.left;
        //     SetWindowPos(hWnd, 0, rect.left, rect.top, x,
        //                  rect.bottom - rect.top, SWP_SHOWWINDOW);
        //     bIsFullscreen = false;
        //
        // SPI_GETWORKAREA is glfwGetMonitorWorkarea, and it is the compositor
        // that decides that rectangle, so nothing here has to guess where the
        // panels are.
        int mx = 0, my = 0, mw = 0, mh = 0;
        glfwGetMonitorWorkarea(mon, &mx, &my, &mw, &mh);
        if (mw <= 0 || mh <= 0) {
            const GLFWvidmode *cur = glfwGetVideoMode(mon);
            if (!cur) return;
            glfwGetMonitorPos(mon, &mx, &my);
            mw = cur->width; mh = cur->height;
        }
        if (spanDisplays) { mx = vx; mw = vw; }   // the SM_CXVIRTUALSCREEN case

        if (glfwGetWindowMonitor(g_window))
            glfwSetWindowMonitor(g_window, nullptr, mx, my, mw, mh,
                                 GLFW_DONT_CARE);
        glfwSetWindowAttrib(g_window, GLFW_DECORATED, GLFW_FALSE);
        if (platformHasWindowPos()) glfwSetWindowPos(g_window, mx, my);
        glfwSetWindowSize(g_window, mw, mh);
        wantW = mw; wantH = mh;
    }
    else {
        // A PLAIN WINDOW OF w x h, AND NOTHING ELSE IS TOUCHED.
        //
        // This branch used to set GLFW_DECORATED and reposition the window
        // before resizing it, on the theory that it had to undo whatever a
        // previous fullscreen had done. On an already-windowed window that is
        // not a no-op: rewriting _MOTIF_WM_HINTS makes the window manager
        // re-apply its own geometry, and the resize that followed was
        // discarded. Measured -- the user asked for 2560 x 1440 and the render
        // target came up 600 x 541, the Launchpad's own size, unchanged.
        //
        // The reference does not do any of that either. CD3DFramework9's
        // windowed branch is a style OR and one SetWindowPos, nothing more.
        // So: only leave fullscreen if we are in it, then size.
        if (w <= 0 || h <= 0) { w = g_savedWndW; h = g_savedWndH; }
        if (w <= 0 || h <= 0) return;

        if (glfwGetWindowMonitor(g_window)) {
            glfwSetWindowMonitor(g_window, nullptr,
                                 g_savedWndX, g_savedWndY, w, h,
                                 GLFW_DONT_CARE);
            glfwSetWindowAttrib(g_window, GLFW_DECORATED, GLFW_TRUE);
        }

        // THE WINDOW IS NOT REPOSITIONED. The reference does not reposition it
        // either: CD3DFramework9's windowed branch is a style OR and one
        // SetWindowPos that carries SWP_NOMOVE's effect by passing 0,0 with a
        // size-only intent, and the window's PLACE came from
        // CreateWindow(..., CW_USEDEFAULT, CW_USEDEFAULT, ...) -- the system's
        // choice. Here that choice is the window manager's, which opens on the
        // monitor holding the pointer. Different mechanism, same contract:
        // the system places it, the client sizes it.
        //
        // A block that centred the window on its monitor stood here briefly.
        // It made test results reproducible, which was the whole of its
        // appeal, and it was still wrong -- nothing in the reference does it
        // and no Win32/Linux difference forces it. Placement stays the
        // system's; a window that comes out the wrong size is what "Force
        // window size" below is for, which is the reference's own answer.

        // winw AND winh ARE AN OUTER WINDOW SIZE. glfwSetWindowSize TAKES A
        // CONTENT SIZE. THE FRAME HAS TO COME OFF IN BETWEEN.
        //
        // This is the difference, and it is an API difference rather than a
        // behavioural one, so it is worth writing out the three places the
        // reference says it.
        //
        //   GraphicsAPI.cpp:280, GraphicsClient::clbkCreateRenderWindow --
        //       hWnd = CreateWindow (strWndClass, "",
        //           WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        //           CW_USEDEFAULT, CW_USEDEFAULT,
        //           VideoData.winw, VideoData.winh, 0, 0, hModule, this);
        //   CreateWindow's nWidth and nHeight are the WHOLE window, caption
        //   and borders included -- not the client area.
        //
        //   D3D9Frame.cpp:225, CD3DFramework9::Initialize, windowed branch --
        //       if (vData->trystencil)
        //           SetWindowPos(hWnd, 0, 0, 0, vData->winw, vData->winh,
        //                        SWP_SHOWWINDOW | SWP_NOSENDCHANGING);
        //   SetWindowPos's cx and cy are the whole window too. Same number,
        //   same meaning.
        //
        //   D3D9Frame.cpp:545, CD3DFramework9::CreateWindowedMode --
        //       GetClientRect(hWnd, &rcScreenRect);
        //       dwRenderWidth  = rcScreenRect.right  - rcScreenRect.left;
        //       dwRenderHeight = rcScreenRect.bottom - rcScreenRect.top;
        //   AND THIS IS THE ONE THAT MATTERS. The render target -- the number
        //   Orbiter is given and lays the entire 2D layer out from -- is the
        //   CLIENT area, which is winw x winh MINUS the frame. On Windows a
        //   2560x1440 window has never rendered at 2560x1440; it renders at
        //   whatever is left inside the caption and borders.
        //
        // glfwSetWindowSize's two arguments are "the desired width and height
        // of the window CONTENT AREA", which is GetClientRect's rectangle and
        // not CreateWindow's. Passing winw and winh straight through therefore
        // asked for a window one caption taller than the reference ever asks
        // for, and then reported that inflated number back as the render size
        // because it is what we requested.
        //
        // What it cost: with Orbiter.cfg at 2560x1440 the core was told the
        // viewport was 1440 tall. DefaultPanel::SetGeometry lays the generic
        // cockpit out from that -- mfdy0 = viewH - mfdh - btnh - 2*gapw,
        // bbtny = viewH - btnh - gapw -- so the bottom btnh + 2*gapw pixels,
        // the PWR/SEL/MNU row and the whole navmode block, were placed below
        // the bottom edge of a window that was never that tall. Orbiter reads
        // the viewport size exactly once, on the line after
        // clbkCreateRenderWindow returns (Orbiter.cpp:767), so nothing later
        // could correct it.
        //
        // glfwGetWindowFrameSize is AdjustWindowRect's information in the
        // other direction: it reports the extents the window manager has put
        // around this window, so subtracting them turns the reference's outer
        // size into the content size this API wants. A window manager that
        // reports no frame yet leaves all four at zero, in which case there is
        // nothing to subtract and this is a no-op.
        {
            int fl = 0, ft = 0, fr = 0, fb = 0;
            glfwGetWindowFrameSize(g_window, &fl, &ft, &fr, &fb);
            if (fl > 0 || ft > 0 || fr > 0 || fb > 0) {
                const int cw = w - fl - fr;
                const int chh = h - ft - fb;
                if (cw > 0 && chh > 0) {
                    fprintf(stderr,
                            "Orbiter: window %d x %d requested; frame is "
                            "%d/%d/%d/%d, so the client area is %d x %d "
                            "(this is what GetClientRect returns on Windows).\n",
                            w, h, fl, ft, fr, fb, cw, chh);
                    w = cw;
                    h = chh;
                }
            }
        }

        // "Force window size" -- SWP_NOSENDCHANGING, in the only spelling X11
        // has for it.
        //
        // A plain resize is a REQUEST. The window manager is free to shrink it
        // and does: on a monitor whose work area is 1036 tall, asking for
        // 2560 x 1440 comes back around 1004 high once the title bar is taken
        // off. That is the same clamping the reference bypasses with
        // SWP_NOSENDCHANGING, which suppresses the WM_WINDOWPOSCHANGING that
        // would have adjusted it.
        //
        // WM_NORMAL_HINTS is what a window manager is obliged to honour, so
        // pinning min and max to the requested size makes the size
        // non-negotiable, and clearing them afterwards leaves the window
        // resizable as before. Without this the checkbox would have nothing to
        // do -- and with it off, the clamped size is what the user gets, which
        // is also what Windows gives them.
        // CAN THE OUTER WINDOW EXIST ON THIS MONITOR AT ALL?
        //
        // Decided before anything is applied, because the answer changes what
        // is applied. If the requested size is at least the monitor's, then
        // whatever decoration the compositor adds puts the frame over the
        // edge, and asking for that exact content size produces a window whose
        // bottom is off the screen. See the maximize block below.
        const GLFWvidmode *mvm = mon ? glfwGetVideoMode(mon) : nullptr;
        const bool cannotFit   = mvm && (w >= mvm->width || h >= mvm->height);

        // "Force window size" pins the size against the window manager, which
        // is the OPPOSITE of what the maximize path needs -- pinned limits
        // would refuse the compositor's own answer. So it is skipped exactly
        // when the requested size cannot be honoured anyway.
        if (forceSize && !cannotFit) glfwSetWindowSizeLimits(g_window, w, h, w, h);

        // MAP THE WINDOW AGAIN AT THE SESSION'S SIZE, WHICH IS THE ONLY WAY TO
        // GET THE REFERENCE'S PLACEMENT ON A COMPOSITOR THAT WILL NOT TAKE ONE.
        //
        // The reference does not resize its render window into existence. It
        // CREATES one, at the session size, and lets the system place it:
        //
        //     hWnd = CreateWindow (strWndClass, "",
        //         WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        //         CW_USEDEFAULT, CW_USEDEFAULT,
        //         VideoData.winw, VideoData.winh, ...);      GraphicsAPI.cpp:280
        //
        // The Launchpad is a DIFFERENT window that Orbiter::CreateRenderWindow
        // hides, so the placement decision is made knowing the final size and
        // nothing is inherited from the dialog.
        //
        // This port has one window and reuses it, and that is where the user's
        // "it starts between two screens" comes from: launched from the
        // Launchpad, the window has been on screen as a 600x541 dialog for as
        // long as it took to click Launch, and the resize grows it IN PLACE
        // around a corner chosen for something fifteen times smaller. A
        // 2552-wide window whose corner is on a 1920-wide monitor runs onto the
        // next one. MEASURED with --scenario= the window was 600x541 and
        // unmapped, so it was placed fresh and the fault did not appear -- which
        // is exactly why the harness never saw it and the person at the desk
        // always did.
        //
        // On X11 the fit-and-move block below is the fix. WAYLAND CANNOT MOVE A
        // WINDOW AT ALL -- "Wayland does not allow toplevel windows to position
        // themselves programmatically" (SDL's README-wayland), and the protocol
        // omits it deliberately, for shell-integrity reasons Mir's own
        // documentation sets out. glfwSetWindowPos raises
        // GLFW_FEATURE_UNAVAILABLE and nothing replaces it.
        //
        // What IS available is unmapping and mapping again. Hiding a Wayland
        // window destroys its xdg_toplevel role; showing it creates a new one,
        // and the compositor places THAT the way it places any new window --
        // knowing its size, because the size was set while it was hidden. That
        // is CreateWindow's contract reproduced with the pieces this platform
        // offers, and it is the same sequence in the same order: size decided
        // first, window mapped second, system places it.
        //
        // Only when the size is actually changing, so a no-op call does not
        // flash the window, and only when it is currently mapped.
        const bool remap = (glfwGetWindowAttrib(g_window, GLFW_VISIBLE) != 0) &&
                           (preW != w || preH != h);
        if (remap) glfwHideWindow(g_window);

        glfwSetWindowSize(g_window, w, h);
        wantW = w; wantH = h;

        if (remap) {
            glfwShowWindow(g_window);
            // The pump's own hide/show rule tracks visibility with a static;
            // it is not told about this one, and does not need to be -- it only
            // acts when its idea of "anything wants the window" CHANGES, and
            // that is unchanged here.
        }

        // A WINDOW AS BIG AS THE SCREEN IS A MAXIMIZED WINDOW, AND ONLY THE
        // COMPOSITOR CAN WORK OUT WHAT THAT MEANS IN PIXELS.
        //
        // The reference asks for an OUTER window of winw x winh and takes
        // whatever client area is left inside it (GetClientRect, D3D9Frame.cpp:
        // 545). On X11 the block above reproduces that by subtracting
        // glfwGetWindowFrameSize -- 4/28/4/4 on this desktop, so 2560x1440
        // becomes a content area of 2552x1408 and the whole window fits.
        //
        // ON WAYLAND THAT MEASUREMENT DOES NOT EXIST. MEASURED:
        //
        //     [display] platform=wayland canSetPos=0 frame=0/0/0/0
        //
        // KDE draws this window's decoration SERVER-SIDE, through the
        // xdg-decoration protocol, and a Wayland client is not told the extents
        // of a decoration it does not draw. GLFW reports zeros, so nothing is
        // subtracted, so the content area really was 2560x1440 -- and the title
        // bar on top of it made the whole window taller than the 1440-tall
        // monitor. The bottom of the content went off the bottom of the screen,
        // taking the PWR/SEL/MNU row and the navmode block with it. The
        // framebuffer and the viewport agreed with each other at 2560x1440 the
        // entire time; the pixels simply were not on the display.
        //
        // Guessing a decoration height here would be inventing a number. The
        // compositor already knows it, and xdg_toplevel.set_maximized is how it
        // is asked: "the surface should be filled with the maximum size, minus
        // any panels or other shell components". It answers with a configure
        // event carrying the exact content size that fits, which is precisely
        // the number GetClientRect returns on Windows.
        //
        // AND IT IS THE FAITHFUL TRANSLATION, not a workaround. CreateWindow
        // with an outer size equal to the screen produces a window whose frame
        // fills the display and whose client area is the display minus the
        // decoration. That is the definition of maximized. So when the
        // requested OUTER size is at least as large as the monitor -- the only
        // case where the frame cannot fit -- this asks for maximized rather
        // than for a size that cannot exist.
        //
        // It also happens to settle the output question: a maximized window is
        // on exactly one monitor by construction.
        //
        // wantW/wantH are cleared because the answer is the compositor's now;
        // the settle loop falls back to waiting for the size to change and hold.
        //
        // NOT gated on forceSize. This is the counterpart of the frame
        // subtraction above -- an outer-size-to-content-size conversion, which
        // the reference performs unconditionally through GetClientRect -- and
        // not a clamp that "Force window size" should be able to overrule.
        // That distinction was got wrong first: gating it on !forceSize made
        // the whole block dead on this desktop, because Orbiter.cfg carries
        // StencilBuffer = TRUE and D3D9Client's Video tab relabels that same
        // IDC_VID_STENCIL checkbox "Force window size", so vData->trystencil
        // is set on any config with a stencil buffer.
        if (cannotFit) {
            fprintf(stderr,
                    "Orbiter: a %dx%d window plus its decoration cannot fit a "
                    "%dx%d monitor; maximizing instead so the compositor picks "
                    "the client size.\n",
                    w, h, mvm->width, mvm->height);
            glfwMaximizeWindow(g_window);
            wantW = 0; wantH = 0;
        }

        if (forceSize && !cannotFit) {
            // Let the change take effect before the limits come off, or the
            // window manager applies its own size again on the next round.
            for (int i = 0; i < 30; ++i) {
                glfwPollEvents();
                int aw = 0, ah = 0;
                glfwGetWindowSize(g_window, &aw, &ah);
                if (aw == w && ah == h) break;
                struct timespec ts { 0, 10 * 1000 * 1000 };
                nanosleep(&ts, nullptr);
            }
            glfwSetWindowSizeLimits(g_window, GLFW_DONT_CARE, GLFW_DONT_CARE,
                                    GLFW_DONT_CARE, GLFW_DONT_CARE);
        }

        // AND NOW THE WINDOW HAS TO BE PUT SOMEWHERE IT FITS, because on
        // Windows the system has already done that and here nobody has.
        //
        // The reference does not reposition its render window, and the note
        // above is right that nothing in CD3DFramework9 moves it. But that is
        // because THE WINDOW WAS PLACED WHEN IT WAS CREATED, AT ITS FINAL
        // SIZE. GraphicsAPI.cpp:280:
        //
        //     hWnd = CreateWindow (strWndClass, "",
        //         WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        //         CW_USEDEFAULT, CW_USEDEFAULT,
        //         VideoData.winw, VideoData.winh, ...);
        //
        // CW_USEDEFAULT asks the system to choose a position for a window of
        // winw x winh, and the position it chooses is on a monitor, with the
        // window on it. The render window is BRAND NEW there -- Orbiter has a
        // separate Launchpad window that it hides -- so the placement is made
        // knowing the size.
        //
        // This port has one window and reuses it. It was created and placed as
        // the 600x541 Launchpad, and the lines above grow it in place to
        // 2552x1408 around a top-left corner that was chosen for a dialog
        // fifteen times smaller. Nothing ever places a window of the session's
        // size, so the session's window lands wherever the dialog happened to
        // be: MEASURED at 660,259 on this desktop, which puts most of a
        // 2552-wide window past the right-hand edge of a 1920-wide monitor and
        // straddling the next one -- the splash comes up cut in half across
        // two screens.
        //
        // A block that CENTRED the window stood here once and was removed as
        // unfaithful, and that was the right call: centring is a style, and
        // the reference does not centre. This is a different claim. The
        // reference guarantees the window is ON a monitor, and that guarantee
        // is what reusing a window loses. So the window is moved only when it
        // does not fit where it is, and it is moved to the work-area origin of
        // the monitor it is already mostly on -- the smallest move that
        // restores what CW_USEDEFAULT promised. A window that already fits is
        // not touched, which keeps a user's own placement.
        //
        // Wayland has no way to position a window at all, hence the guard; see
        // platformHasWindowPos.
        if (platformHasWindowPos()) {
            int wx = 0, wy = 0;
            glfwGetWindowPos(g_window, &wx, &wy);

            // The monitor holding the window's top-left, else the primary.
            GLFWmonitor *home = mon;
            int count = 0;
            GLFWmonitor **all = glfwGetMonitors(&count);
            for (int i = 0; i < count; ++i) {
                int mxx = 0, myy = 0;
                glfwGetMonitorPos(all[i], &mxx, &myy);
                const GLFWvidmode *vm = glfwGetVideoMode(all[i]);
                if (!vm) continue;
                if (wx >= mxx && wx < mxx + vm->width &&
                    wy >= myy && wy < myy + vm->height) { home = all[i]; break; }
            }

            if (home) {
                int ax = 0, ay = 0, aw = 0, ah = 0;
                glfwGetMonitorWorkarea(home, &ax, &ay, &aw, &ah);
                if (aw > 0 && ah > 0) {
                    int fl = 0, ft = 0, fr = 0, fb = 0;
                    glfwGetWindowFrameSize(g_window, &fl, &ft, &fr, &fb);

                    // The frame's own rectangle, which is what has to fit.
                    const int outerW = w + fl + fr;
                    const int outerH = h + ft + fb;

                    if (wx - fl < ax || wy - ft < ay ||
                        wx - fl + outerW > ax + aw ||
                        wy - ft + outerH > ay + ah) {
                        fprintf(stderr,
                                "Orbiter: a %d x %d window does not fit at "
                                "%d,%d (work area %d x %d at %d,%d); moving it "
                                "to the corner of that monitor.\n",
                                outerW, outerH, wx, wy, aw, ah, ax, ay);
                        glfwSetWindowPos(g_window, ax + fl, ay + ft);
                    }
                }
            }
        }
    }

    g_displayStyle = fullscreen ? style : 2;

    // WAIT FOR THE RESIZE TO ACTUALLY HAPPEN, AND "STABLE" IS NOT THE TEST.
    //
    // The reference needs nothing here. SetWindowPos is synchronous: it sends
    // WM_WINDOWPOSCHANGING and WM_SIZE and returns with the window already the
    // new size, so CD3DFramework9::CreateWindowedMode's GetClientRect on the
    // next line reads the finished result. On Wayland a resize is a REQUEST,
    // answered by a configure event that arrives whenever the compositor gets
    // to it.
    //
    // This loop used to wait for the framebuffer size to stop changing between
    // polls -- three identical readings, or 600 ms. THAT TEST IS SATISFIED
    // IMMEDIATELY BY THE OLD SIZE, because a size that has not begun to change
    // yet is perfectly stable. MEASURED on this desktop's own Wayland session,
    // with Orbiter.cfg asking for 2560x1440:
    //
    //     Render target size...... : 600 x 541
    //
    // -- the Launchpad's dialog size, kept for the whole session. Orbiter then
    // laid the HUD, both MFDs and the entire generic cockpit out for a
    // 600x541 viewport. It went unnoticed because every harness in tests/
    // forces ORBITER_GLFW_PLATFORM=x11, where the resize lands fast enough to
    // beat the loop.
    //
    // So the test is now "has it become what we asked for", with the requested
    // content size carried down in wantW/wantH from whichever branch above ran.
    // Where there is no single right answer -- a fullscreen style, where the
    // monitor decides -- wantW is 0 and the fallback is the old stability test,
    // but starting from the size recorded BEFORE the request, so an unchanged
    // reading cannot end the wait.
    //
    // glfwWaitEventsTimeout rather than poll-and-sleep: the answer arrives as
    // an event, and blocking on it takes microseconds where a fixed 10 ms sleep
    // wastes most of the budget.
    {
        const double deadline = glfwGetTime() + 2.0;
        int aw = 0, ah = 0, stable = 0;
        int lastW = preW, lastH = preH;

        for (;;) {
            glfwGetFramebufferSize(g_window, &aw, &ah);

            // Exactly what was asked for ends the wait at once.
            if (wantW > 0 && aw == wantW && ah == wantH) break;

            // Anything else that has moved off the old size and then held
            // still is the compositor's counter-offer, and it is the answer.
            if (aw > 0 && (aw != preW || ah != preH)) {
                if (aw == lastW && ah == lastH) { if (++stable >= 3) break; }
                else { stable = 0; lastW = aw; lastH = ah; }
            }

            if (glfwGetTime() >= deadline) {
                fprintf(stderr,
                        "Orbiter: the window is still %dx%d after 2 s; asked "
                        "for %dx%d. Rendering at the size it granted.\n",
                        aw, ah, wantW, wantH);
                break;
            }
            glfwWaitEventsTimeout(0.01);
        }

        fprintf(stderr, "Orbiter: [display] settled at %dx%d (was %dx%d)\n",
                aw, ah, preW, preH);
    }

    g_swapChainRebuild = true;
}

// Vertical sync. Takes effect on the next frame: the swapchain is rebuilt with
// the new present mode, which is the only way to change it in Vulkan.
extern "C" void orbiter_SetVSync(int enable)
{
    if (!initialise()) return;

    const bool wantNoVSync = (enable == 0);
    if (wantNoVSync == g_noVSync) return;
    g_noVSync = wantNoVSync;

    ImGui_ImplVulkanH_Window *wd = &g_mainWindowData;
    VkPresentModeKHR vsynced[]  = { VK_PRESENT_MODE_FIFO_KHR };
    VkPresentModeKHR unsynced[] = { VK_PRESENT_MODE_MAILBOX_KHR,
                                    VK_PRESENT_MODE_IMMEDIATE_KHR,
                                    VK_PRESENT_MODE_FIFO_KHR };
    wd->PresentMode = g_noVSync
        ? ImGui_ImplVulkanH_SelectPresentMode(g_physicalDevice, wd->Surface,
                                              unsynced, IM_ARRAYSIZE(unsynced))
        : ImGui_ImplVulkanH_SelectPresentMode(g_physicalDevice, wd->Surface,
                                              vsynced, IM_ARRAYSIZE(vsynced));
    g_swapChainRebuild = true;
}

// The GPU the user chose, recorded for the NEXT run. Called by Orbiter::Create
// before the Launchpad exists; see g_preferredGpu.
extern "C" void orbiter_SetPreferredGpuIndex(int idx)
{
    g_preferredGpu = idx;
}

// The render window the simulation actually owns, published by
// Orbiter::CreateRenderWindow the moment InitRenderWnd has stamped the
// GraphicsClient* into it. Null outside a session.
static HWND g_simRenderWnd = nullptr;

extern "C" void orbiter_SetSimRenderWindow(HWND hWnd)
{
    g_simRenderWnd = hWnd;
}

extern "C" HWND orbiter_GetRenderWindow(void)
{
    if (!initialise()) return nullptr;

    // =======================================================================
    // TWO RENDER WINDOWS EXISTED, AND THE MOUSE WAS POSTED TO THE WRONG ONE.
    // =======================================================================
    //
    // GraphicsClient::clbkCreateRenderWindow (GraphicsAPI.cpp:342) calls
    // CreateWindow(strWndClass, ...) and Orbiter::CreateRenderWindow passes
    // the result through InitRenderWnd, which does the one thing that makes a
    // render window work:
    //
    //     SetWindowLongPtr (hWnd, GWLP_USERDATA, (LONG_PTR)this);
    //
    // ::WndProc (GraphicsAPI.cpp:963) reads exactly that back, and forwards to
    // RenderWndProc only `if (gc)`. A window of the right CLASS but without
    // that pointer falls into DefWindowProc and drops every message, silently.
    //
    // This function used to create a SECOND window -- right class, no
    // GraphicsClient* -- and postMouseMessages posted to it. So every mouse
    // message was posted, queued, and dispatched (Win32Dlg's "dispatch
    // msg=0x020A" lines are all there), then thrown away one call short of
    // Orbiter::MsgProc. Measured: 32 mouse messages dispatched, 0 reaching
    // MsgProc. The wheel did not zoom, right-drag did not swing the camera,
    // and no cockpit control could be clicked -- while the keyboard worked
    // perfectly, because the keyboard is POLLED through DirectInput and never
    // goes near a window procedure.
    //
    // It could not have worked even by accident: RenderWndProc's own first
    // act is `if (hRenderWnd != hWnd) { LogErr("Invalid Window !!"); return 0; }`
    // (D3D9Client.cpp:1714), so a second window is refused by the reference
    // client too. There is one render window on Windows and there is one here.
    if (g_simRenderWnd) return g_simRenderWnd;

    // No session: fall back to a window of this host's own, which is what the
    // splash and the Launchpad size themselves against.
    static HWND renderWnd = nullptr;
    if (!renderWnd) {
        int w = 0, h = 0;
        glfwGetFramebufferSize(g_window, &w, &h);

        // ===================================================================
        // THE CLASS NAME IS LOAD-BEARING, AND IT WAS WRONG.
        // ===================================================================
        //
        // This used to create the window under the invented class name
        // "Orbiter_Render", which nothing registers. CreateWindowExA looks
        // the class up to find its window procedure and leaves wndProc NULL
        // when it misses, so dispatchToProc fell through to DefWindowProcA
        // and **the render window had no window procedure at all**.
        //
        // Nothing said so. The window worked for everything the dialog layer
        // does by itself -- GetClientRect, SetWindowText, sizing -- because
        // none of that goes through the procedure. What silently did not work
        // was every message Orbiter sends the render window, and the one that
        // matters is the shutdown chain:
        //
        //     SessionLimitReached -> PostMessage(hRenderWnd, WM_CLOSE)
        //       -> RenderWndProc WM_CLOSE   -> PreCloseSession, DestroyWindow
        //       -> RenderWndProc WM_DESTROY -> CloseSession
        //       -> clbkCloseSession, clbkDestroyRenderWindow
        //
        // WM_CLOSE was posted, dispatched into DefWindowProcA, and the whole
        // chain ended there. **A session started with --maxsystime never
        // terminated and no graphics-client cleanup ever ran**, in any
        // session this port has ever recorded. It looked like the test
        // harness's kill -9 arriving first; it was not.
        //
        // strWndClass is the class GraphicsClient::clbkInitialise registers,
        // with ::WndProc, which reads the GraphicsClient* out of GWLP_USERDATA
        // (InitRenderWnd puts it there) and forwards to
        // GraphicsClient::RenderWndProc and then Orbiter::MsgProc. Creating
        // the window under that class is the whole fix, and it is what
        // GraphicsClient::clbkCreateRenderWindow does on Windows.
        extern const char *strWndClass;   // GraphicsAPI.cpp

        // CHECKED, NOT ASSUMED: a missing registration would put this back
        // exactly where it was, silently, and the failure would once again
        // look like something else entirely.
        WNDCLASSA wc;
        if (!GetClassInfoA(nullptr, strWndClass, &wc) || !wc.lpfnWndProc) {
            fprintf(stderr, "orbiter_GetRenderWindow: the window class \"%s\" "
                    "has no procedure registered. The render window will not "
                    "receive WM_CLOSE, so the session cannot shut down.\n",
                    strWndClass);
        }

        // The caption is empty because the reference's is: both branches of
        // GraphicsClient::clbkCreateRenderWindow (GraphicsAPI.cpp:347 and
        // :351) pass "" and leave the naming to the client, which does it on
        // the next line of its own clbkCreateRenderWindow -- D3D9Client.cpp:430
        // is SetWindowText(hRenderWnd, "[D3D9Client]"). "Orbiter" stood here
        // and would have overridden a name the client never got to set,
        // because SetWindowTextA had nowhere to put it; it does now.
        renderWnd = CreateWindowExA(0, strWndClass, "",
                                    WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                                    0, 0, w, h,
                                    nullptr, nullptr, nullptr, nullptr);
    }
    return renderWnd;
}

// The host window's caption, which the Win32 shim's SetWindowTextA calls when
// the render window is renamed.
//
// TWO WINDOWS ON WINDOWS, ONE HERE. The Launchpad is a dialog with its own
// caption and the render window is a separate top-level window with its own,
// so on Windows the two never collide. This port reuses the single GLFW window
// for both -- see orbiter_GetRenderWindow above -- so the render window's
// caption has to be applied to it, or a running session sits under the title
// "OpenOrbiter Launchpad" for as long as it lasts.
//
// glfwSetWindowTitle works on both platforms this build supports: X11 sets
// _NET_WM_NAME, Wayland sends xdg_toplevel.set_title. Neither is decoration
// the client draws, so neither is affected by whether the compositor puts the
// title bar on the client's side or its own.
extern "C" void orbiter_SetHostWindowTitle(const char *title)
{
    if (!g_window) return;
    glfwSetWindowTitle(g_window, title ? title : "");
}

// Scene frame boundaries.
//
// Orbiter::Render3DEnvironment calls clbkRenderScene, then has ImGui draw the
// dialogs, then clbkDisplayFrame. Both ends map onto the single frame the
// UIHost pump already builds, so the scene and the dialogs land in one
// swapchain image and are presented together -- rendering them in separate
// frames would tear the dialogs away from the scene beneath them.
//
// The scene itself is not drawn yet: this stage establishes the frame path.
extern "C" void orbiter_PumpFrame(void);   // defined below

extern "C" void orbiter_BeginSceneFrame(void)
{
    if (!initialise()) return;
    g_inSceneFrame  = true;
    g_sessionActive = true;   // stays set until the session ends
}

extern "C" void orbiter_EndSceneFrame(void)
{
    if (!initialise()) return;
    orbiter_PumpFrame();
    g_inSceneFrame = false;   // per-frame only; g_sessionActive persists
}

// HOW MANY FRAMES THIS HOST KEEPS IN FLIGHT.
//
// NO WINDOWS COUNTERPART, AND THAT IS THE POINT. The D3D9 runtime owned every
// constant register and every texture binding a draw referred to, and kept
// them alive for as long as its own queued frames needed them; an application
// could overwrite a constant the instant after a DrawPrimitive and the
// runtime dealt with it. The graphics client is written in that style,
// because it was that program.
//
// Vulkan gives no such guarantee: a draw only RECORDS a reference to a
// descriptor set and to the buffer bytes behind it, and both must stay
// untouched until the command buffer naming them has finished executing. So
// the client has to recycle its per-frame pools and arenas on a lag, and the
// lag it needs is exactly this number -- renderFrame reuses slot n % count
// and waits on THAT SLOT'S fence, so anything a frame used is certainly idle
// count frames later, and not before.
//
// Publishing the number rather than letting the client guess it: a guess too
// small is a use-after-free the driver answers with VK_ERROR_DEVICE_LOST, and
// a guess too large is pools and megabytes held for nothing.
extern "C" unsigned orbiter_GetFramesInFlight(void)
{
    // Before the swapchain exists there is nothing in flight; 2 is the
    // smallest honest answer and is never too small.
    const unsigned n = (unsigned)g_mainWindowData.ImageCount;
    return n < 2u ? 2u : n;
}

// The scripted UI driver's pointer. See the note by g_injectMouse: this sets
// the raw state postMouseMessages reads, and nothing else about the path
// changes. `buttons` is a bitmask -- 1 left, 2 right, 4 middle -- and holds
// across frames so a drag can be expressed as down, several moves, up.
extern "C" void orbiter_InjectMouse(int x, int y, int buttons, float wheel)
{
    g_injectMouse = true;
    g_injX        = (float)x;
    g_injY        = (float)y;
    g_injButtons  = buttons;
    g_injWheel    = wheel;
}

extern "C" void orbiter_ClearSplash(void);   // defined below

// LOADING IS OVER: the client's bRunning, on this side of the boundary.
//
// The reference's counterpart is one line in D3D9Client::clbkPostCreation --
//     bRunning = true;
// -- and everything that reads it changes meaning at once: OutputLoadStatus
// starts declining messages, and clbkDisplayFrame stops re-blitting the splash
// over the back buffer. Nothing is destroyed and nothing is hidden; the flag
// alone is the transition, and the scene is drawable from the same instant.
//
// It exists here because the host owns two things the client does not: the
// splash pixels, and the decision whether the window has any reason to be on
// screen. Without this call the second of those went false for the whole gap
// between the scenario finishing loading and the first scene frame -- see the
// long note in drawSplash for what that cost.
//
// g_sessionActive was already being raised lazily by orbiter_BeginSceneFrame,
// which is a frame too late: by then the window has already been unmapped and
// remapped. That assignment is kept, because a client that never calls this is
// better served by a late flag than by none.
extern "C" void orbiter_BeginSession(void)
{
    if (!initialise()) return;
    g_sessionActive = true;
}

// WHO OWNS THE FRAME RIGHT NOW: the message loop, or the graphics client.
//
// There are two frame sources in this process and only one may run at a time.
// Win32Dlg.cpp's PeekMessageA pumps a frame whenever the queue drains, which
// is what drives the Launchpad and every dialog when no session exists. Once a
// session starts, the CLIENT drives: clbkDisplayFrame -> PresentScene ->
// orbiter_EndSceneFrame -> orbiter_PumpFrame, once per simulation frame.
//
// With both running, each pump acquires a swapchain image, and with a
// two-image swapchain the second acquire has nothing left to give:
//
//     VUID-vkAcquireNextImageKHR-surface-07783
//     Application has already previously acquired 1 image from swapchain
//
// after which presentFrame presents an image index it does not own and the
// driver faults. That is exactly the Windows arrangement seen from the other
// side: there, Present is called once per frame from clbkDisplayFrame and the
// message loop only dispatches messages -- it never draws.
extern "C" int orbiter_SessionOwnsFrames(void)
{
    return g_sessionActive ? 1 : 0;
}

// ---------------------------------------------------------------------------
// The device lock, reached by the graphics client.
//
// See the long note beside g_deviceMutex at the top of this file: this is the
// half of D3DCREATE_MULTITHREADED that the client needs, because its one-shot
// uploads run on Orbiter's tile-loader threads and submit on the core's queue.
// VulkanDevice::BeginOneShot takes it and EndOneShot releases it, so the whole
// allocate/record/submit/wait/free span is one critical section -- a command
// pool may not be recorded into from two threads at once either.
//
// Exported as plain functions rather than a lock object because the client
// reaches this file only through the orbiter_* C interface.
// ---------------------------------------------------------------------------
extern "C" void orbiter_LockDevice(void)
{
    g_deviceMutex.lock();
}

extern "C" void orbiter_UnlockDevice(void)
{
    g_deviceMutex.unlock();
}

// ---------------------------------------------------------------------------
// Reset every frame's command buffer, so none of them still refers to a
// descriptor set the client is about to free.
//
// renderFrame resets a frame's pool at the START of recording that frame
// (see the vkResetCommandPool above), which is correct for the steady state
// and leaves the LAST frame of a session recorded and never reset. Its
// command buffer still names the client's set 0, its images and its buffers.
//
// vkDeviceWaitIdle is not enough on its own. It guarantees the commands have
// COMPLETED, which is what the spec's wording asks for -- but a descriptor
// set stays bound to a command buffer, and the validation layer keeps
// reporting it as in use, until that buffer is reset or freed. MEASURED
// before this existed, on the KSC scenario with the layers on:
//
//     465 x VUID-vkFreeDescriptorSets-pDescriptorSets-00309
//       1 x VUID-vkDestroyImageView-imageView-01026
//
// all of them at the client's teardown, and none of them in a run where the
// session ended from a different place.
//
// The pools are reset rather than the buffers individually because that is
// what renderFrame does and because a pool reset also returns the buffers'
// memory. The fences are waited on first for the same reason renderFrame
// waits on them: a pool may not be reset while a buffer allocated from it is
// still executing, and the caller's vkDeviceWaitIdle is belt to this braces
// only if it actually happened -- this function does not assume it did.
extern "C" void orbiter_ResetFrameCommands(void)
{
    if (!g_initialised || !g_device) return;

    // Frames is an ImVector, not a raw array -- its own Size is the count, and
    // it is what renderFrame indexes with wd->FrameIndex.
    ImGui_ImplVulkanH_Window *wd = &g_mainWindowData;

    for (int i = 0; i < wd->Frames.Size; i++) {
        ImGui_ImplVulkanH_Frame *fd = &wd->Frames[i];
        if (fd->Fence != VK_NULL_HANDLE)
            vkWaitForFences(g_device, 1, &fd->Fence, VK_TRUE, UINT64_MAX);
        if (fd->CommandPool != VK_NULL_HANDLE)
            vkResetCommandPool(g_device, fd->CommandPool, 0);
    }
}

// ---------------------------------------------------------------------------
// The other half of the back-buffer readback. See recordFrameCapture.
//
// Counterpart of ONE LINE of the reference --
//
//     pDevice->GetRenderTarget(0, &pBack)
//
// -- which is all D3D9Client needed, because the swap chain was its own. Here
// the client cannot name the image at all, so the whole operation is done on
// its behalf: arm the capture, pump one frame (which renders the scene through
// the client's own callback exactly as any other frame does, and presents it),
// wait for that frame, and hand back its pixels.
//
// The extra frame is not a departure either. Orbiter::PreCloseSession already
// calls Render3DEnvironment(true) immediately before clbkSaveSurfaceToImage,
// for the same reason: to get one frame drawn without the dialogs on it. This
// pumps that frame rather than the message loop doing it a moment later.
//
// Returns 0 on failure and leaves *w and *h alone. The pixels are BGRA, four
// bytes per pixel, width*4 to the row -- the format
// VulkanDevice::ReadTexture would have produced, so the caller's unpacking is
// unchanged.
// ---------------------------------------------------------------------------
extern "C" int orbiter_CaptureBackBuffer(void *dst, unsigned bytes, int *w, int *h)
{
    if (!g_initialised || !dst) return 0;

    ImGui_ImplVulkanH_Window *wd = &g_mainWindowData;

    g_captureDone  = false;
    g_captureArmed = true;

    orbiter_PumpFrame();

    if (!g_captureDone || g_captureMem == VK_NULL_HANDLE) {
        g_captureArmed = false;
        return 0;
    }

    // The frame that recorded the copy is the one still named by FrameIndex --
    // presentFrame advances SemaphoreIndex, not this. Waiting on its fence is
    // what makes the staging buffer readable; it is NOT reset, because
    // renderFrame waits and resets it itself when it comes round again.
    VkFence fence = wd->Frames[wd->FrameIndex].Fence;
    if (fence != VK_NULL_HANDLE)
        vkWaitForFences(g_device, 1, &fence, VK_TRUE, UINT64_MAX);

    const size_t need = (size_t)g_captureW * g_captureH * 4;
    if (bytes < need) return 0;

    void *src = nullptr;
    if (vkMapMemory(g_device, g_captureMem, 0, VK_WHOLE_SIZE, 0, &src) != VK_SUCCESS || !src)
        return 0;

    // AFTER THE MAP, NOT BEFORE. vkInvalidateMappedMemoryRanges requires the
    // memory to be host mapped at the time of the call --
    //     VUID-VkMappedMemoryRange-memory-00684
    //     Attempting to use memory that is not currently host mapped
    // -- and the invalidate is what makes a non-coherent allocation's contents
    // visible to this read.
    VkMappedMemoryRange range{};
    range.sType  = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    range.memory = g_captureMem;
    range.size   = VK_WHOLE_SIZE;
    vkInvalidateMappedMemoryRanges(g_device, 1, &range);

    memcpy(dst, src, need);
    vkUnmapMemory(g_device, g_captureMem);

    // THE SWAPCHAIN FORMAT IS NOT ALWAYS BGRA. setupSurface asks for
    // B8G8R8A8_UNORM first but accepts R8G8B8A8_UNORM, and on that format the
    // bytes come out R,G,B,A -- which would save a picture with red and blue
    // swapped, the classic wrong-endian screenshot. The caller is promised
    // BGRA, so the swap happens here where the format is known.
    if (wd->SurfaceFormat.format == VK_FORMAT_R8G8B8A8_UNORM ||
        wd->SurfaceFormat.format == VK_FORMAT_R8G8B8A8_SRGB) {
        unsigned char *p = (unsigned char *)dst;
        for (size_t i = 0; i < need; i += 4) {
            const unsigned char t = p[i];
            p[i] = p[i + 2];
            p[i + 2] = t;
        }
    }

    if (w) *w = (int)g_captureW;
    if (h) *h = (int)g_captureH;
    return 1;
}

// Called when the session ends and the Launchpad comes back, so the window
// once again follows dialog visibility.
extern "C" void orbiter_EndSession(void)
{
    g_sessionActive = false;
    g_inSceneFrame  = false;
    orbiter_ClearSplash();

    // And the caption goes back to the Launchpad's. On Windows this is free:
    // the render window is destroyed and the Launchpad dialog underneath it
    // has had its own caption all along. Here the two share the one GLFW
    // window, so whatever the client named it -- "[VulkanClient]", the way
    // D3D9Client names its own -- would otherwise stay over the Launchpad for
    // the rest of the run. The string is the one initialise() creates the
    // window with.
    if (g_window) glfwSetWindowTitle(g_window, "OpenOrbiter Launchpad");
}

// ---------------------------------------------------------------------------
// The splash screen. See the note by SplashState for the division of labour.
//
// Every coordinate and every string comes from the client, computed with the
// reference's own expressions; nothing here decides what the splash looks
// like.
// ---------------------------------------------------------------------------

extern "C" void orbiter_SetSplashImage(const unsigned char *rgba,
                                       int w, int h, int x, int y)
{
    if (!initialise()) return;

    // Replacing an image destroys the old one first. clbkSetSplashScreen can
    // change the picture between sessions, and the window is the only thing
    // that would otherwise notice -- as a steadily growing resident set.
    if (g_splash.set != VK_NULL_HANDLE || g_splash.image != VK_NULL_HANDLE) {
        // Under the device lock: vkDeviceWaitIdle is vkQueueWaitIdle on every
        // queue, and this queue is shared with the client's loader threads.
        // See the note beside g_deviceMutex.
        { std::lock_guard<std::recursive_mutex> lk(g_deviceMutex);
          vkDeviceWaitIdle(g_device); }
        if (g_splash.set)    ImGui_ImplVulkan_RemoveTexture(g_splash.set);
        if (g_splash.view)   vkDestroyImageView(g_device, g_splash.view, g_allocator);
        if (g_splash.image)  vkDestroyImage(g_device, g_splash.image, g_allocator);
        if (g_splash.memory) vkFreeMemory(g_device, g_splash.memory, g_allocator);
        g_splash.set = VK_NULL_HANDLE; g_splash.view = VK_NULL_HANDLE;
        g_splash.image = VK_NULL_HANDLE; g_splash.memory = VK_NULL_HANDLE;
    }

    g_splash.imgW = g_splash.imgH = 0;
    g_splash.active = true;

    if (!rgba || w <= 0 || h <= 0) return;

    g_splash.set = uploadTextureEx(rgba, w, h, &g_splash.image,
                                   &g_splash.memory, &g_splash.view);
    if (g_splash.set == VK_NULL_HANDLE) {
        fprintf(stderr, "Orbiter: splash image %dx%d could not be uploaded\n",
                w, h);
        return;
    }
    g_splash.imgW = w;
    g_splash.imgH = h;
    g_splash.imgX = x;
    g_splash.imgY = y;
}

extern "C" void orbiter_SetSplashInfo(const char *l0, const char *l1,
                                      const char *l2, int x, int y,
                                      int spacing, int size, unsigned colour)
{
    g_splash.info[0] = l0 ? l0 : "";
    g_splash.info[1] = l1 ? l1 : "";
    g_splash.info[2] = l2 ? l2 : "";
    g_splash.infoX       = x;
    g_splash.infoY       = y;
    g_splash.infoSpacing = spacing;
    g_splash.infoSize    = size;
    // COLORREF is 0x00BBGGRR; the same conversion Gdi.cpp's toImGui does.
    g_splash.colour = IM_COL32(colour & 0xFF, (colour >> 8) & 0xFF,
                               (colour >> 16) & 0xFF, 255);
    g_splash.active = true;
}

extern "C" void orbiter_SetSplashStatus(const char *label, const char *item,
                                        int x, int y, int w,
                                        int labelSize, int itemSize)
{
    g_splash.label     = label ? label : "";
    g_splash.item      = item  ? item  : "";
    g_splash.statusX   = x;
    g_splash.statusY   = y;
    g_splash.statusW   = w;
    g_splash.labelSize = labelSize;
    g_splash.itemSize  = itemSize;
    g_splash.hasStatus = true;
    g_splash.active    = true;
}

extern "C" void orbiter_ClearSplash(void)
{
    if (g_initialised &&
        (g_splash.set != VK_NULL_HANDLE || g_splash.image != VK_NULL_HANDLE)) {
        // The frame that last drew the splash may still be executing, and its
        // command buffer refers to this descriptor set and image view.
        // Under the device lock -- vkDeviceWaitIdle is host access to every
        // queue, and this one is shared with the client's loader threads.
        { std::lock_guard<std::recursive_mutex> lk(g_deviceMutex);
          vkDeviceWaitIdle(g_device); }
        if (g_splash.set)    ImGui_ImplVulkan_RemoveTexture(g_splash.set);
        if (g_splash.view)   vkDestroyImageView(g_device, g_splash.view, g_allocator);
        if (g_splash.image)  vkDestroyImage(g_device, g_splash.image, g_allocator);
        if (g_splash.memory) vkFreeMemory(g_device, g_splash.memory, g_allocator);
    }
    g_splash = SplashState();
}

// ---------------------------------------------------------------------------
// PER-PHASE FRAME ACCOUNTING
//
// ORBITER_TRACE_PACING says a typical frame is 5.8 ms and that 7-11 times a
// second one takes 70-130 ms. It cannot say WHERE, and there are four
// candidates that are not even in the same binary: Orbiter's own physics and
// message work between pumps, the dialog/widget pass here, the graphics
// client's scene recording (reached through the callback inside renderFrame),
// and the present itself.
//
// So each is timed and the breakdown is printed ONLY for a frame that actually
// hitched. A per-frame line at 170 fps would cost more than the thing being
// measured and would bury the ten frames that matter under a thousand that do
// not.
//
// `between` is the one that is easy to overlook and the reason the mark is
// taken at the END of the previous pump: it is everything Orbiter did while
// this file was not running -- BeginTimeStep, UpdateWorld, UserInput,
// clbkRenderScene -- and if the hitch lives there, nothing inside this
// function will ever show it.
struct FramePhases {
    std::chrono::steady_clock::time_point t0, tPoll, tDlg, tRender, tEnd, tPrev;
    bool havePrev = false;
};
FramePhases g_ph;
bool g_tracePacing = (getenv("ORBITER_TRACE_PACING") != nullptr);

inline double msBetween(std::chrono::steady_clock::time_point a,
                        std::chrono::steady_clock::time_point b)
{
    return std::chrono::duration<double, std::milli>(b - a).count();
}

// IS THIS THREAD ALREADY BUILDING A FRAME.
//
// Anything the pump calls out to can call back in -- drawDialog runs a
// dialog's window procedure, orbiter_DrawImGuiDialogs runs an ImGuiDialog's
// OnDraw, and either may be a plugin's code that calls a shim entry point of
// its own. A second pump from inside the first is not merely wasteful: ImGui
// asserts if NewFrame is called with a frame already open, and the swapchain
// image the outer frame acquired has not been presented yet.
//
// Set through an RAII guard rather than by hand because the pump has four
// early returns -- window closing, zero-sized framebuffer, nothing visible,
// and initialise() failing -- and a flag left set by any one of them would
// stop every later frame.
//
// MessageBoxA is the caller that needs the answer; see the note there.
namespace { bool g_inPumpFrame = false; }

extern "C" int orbiter_InFramePump(void) { return g_inPumpFrame ? 1 : 0; }

extern "C" void orbiter_PumpFrame(void)
{
    if (!initialise()) return;

    struct PumpGuard {
        PumpGuard()  { g_inPumpFrame = true;  }
        ~PumpGuard() { g_inPumpFrame = false; }
    } pumpGuard;

    if (g_tracePacing) g_ph.t0 = std::chrono::steady_clock::now();

    if (glfwWindowShouldClose(g_window)) {
        // Closing the window must go through WM_CLOSE, not straight to
        // PostQuitMessage.
        //
        // LaunchpadDialog::DlgProc handles WM_CLOSE by calling UpdateConfig(),
        // which is what asks every tab for its current control state and
        // records the window geometry -- and only then destroys the window,
        // whose WM_DESTROY posts the quit. Short-circuiting to PostQuitMessage
        // skips UpdateConfig entirely, so every setting the user changed in
        // the Launchpad is silently discarded on exit.
        //
        // The flag is cleared first: the handler may decline to close (demo
        // mode blocks exit), and leaving it set would re-send WM_CLOSE every
        // frame.
        glfwSetWindowShouldClose(g_window, GLFW_FALSE);

        HWND active = orbiter_ActiveDialog();
        if (active) PostMessageA(active, WM_CLOSE, 0, 0);
        else        PostQuitMessage(0);
        return;
    }

    glfwPollEvents();
    if (g_tracePacing) g_ph.tPoll = std::chrono::steady_clock::now();

    // A MOUSE BUTTON THAT NOBODY IS HOLDING.
    //
    // ImGui's button state is edge-driven: it is told about a press and about
    // a release, and it believes the last thing it heard. If a release is
    // never delivered the button stays down for the rest of the session --
    // io.WantCaptureMouse stays true, a window keeps following the pointer,
    // and clicks go to the wrong place.
    //
    // Losing a release is not hypothetical here. A41 in the register is
    // exactly that ("the button-up lost in rotation mode"), and a core taken
    // from a session the user reported as frozen -- after dragging a dialog --
    // had `GImGui->IO.MouseDown[0] == true` with nobody touching the mouse.
    // A compositor that takes an interactive move/resize grab mid-drag
    // swallows the release, and Wayland does exactly that.
    //
    // GLFW's own button state is LEVEL, not edge: glfwGetMouseButton reports
    // what the button is doing now. So it can correct a state ImGui only
    // reached through a lost event. Only the stuck-down direction is
    // corrected -- a press ImGui has not seen yet is left alone, because the
    // event carrying it may simply not have been dispatched this frame, and
    // forcing it down here would deliver a click at the wrong position.
    {
        ImGuiIO &mio = ImGui::GetIO();
        for (int b = 0; b < 3; ++b) {
            if (mio.MouseDown[b] &&
                glfwGetMouseButton(g_window, b) == GLFW_RELEASE) {
                mio.MouseDown[b] = false;
                if (b == 0)
                    oapiWriteLog((char *)"UIHost: mouse button 1 was stuck "
                                         "down (release lost); cleared from "
                                         "the GLFW button state.");
            }
        }

        // WHAT GLFW ITSELF SEES, as opposed to what ImGui believes.
        //
        // The counterpart of DInput.cpp's ORBITER_TRACE_INPUT line, added for
        // the same reason and after the same confusion. postMouseMessages'
        // [mouse] trace reports io.MouseDown, so "the button never went down"
        // there has three quite different causes -- the event never reached
        // the process, it reached GLFW but not ImGui's queue, or ImGui had it
        // and something cleared it -- and that trace cannot tell them apart.
        //
        // It cost a whole investigation: an injected drag produced no [mouse]
        // trace at all, and the first suspect was the stuck-button correction
        // immediately above, which would have been a serious regression. It
        // was not that (the correction logs every firing and had not fired),
        // but proving so took a second harness. This line answers it directly:
        // glfw= is the level state from the platform, imgui= is what the
        // widget layer thinks, and a press that is in the first and not the
        // second is a lost event rather than a dead device.
        //
        // Printed on CHANGE of either view, so a run costs a handful of lines.
        if (getenv("ORBITER_TRACE_MOUSE")) {
            int g = 0, m = 0;
            for (int b = 0; b < 3; ++b) {
                if (glfwGetMouseButton(g_window, b) == GLFW_PRESS) g |= 1 << b;
                if (mio.MouseDown[b])                              m |= 1 << b;
            }
            static int pg = -1, pm = -1;
            if (g != pg || m != pm) {
                pg = g; pm = m;
                fprintf(stderr, "[btn] glfw=%d imgui=%d focused=%d\n", g, m,
                        glfwGetWindowAttrib(g_window, GLFW_FOCUSED));
            }
        }
    }

    // Keyboard state, read by GetKeyState. Dialog keyboard handling needs to
    // know whether Shift is held to reverse the tab direction, and ImGui's
    // GLFW backend does not expose that through a Win32-shaped call.
    orbiter_SetShiftState(
        glfwGetKey(g_window, GLFW_KEY_LEFT_SHIFT)  == GLFW_PRESS ||
        glfwGetKey(g_window, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS);
    // Ctrl for the same reason: OVP/VulkanClient's RenderWndProc tests
    // Shift+Ctrl through GetAsyncKeyState for its picking and debug
    // shortcuts, and that call answers from this sample.
    orbiter_SetCtrlState(
        glfwGetKey(g_window, GLFW_KEY_LEFT_CONTROL)  == GLFW_PRESS ||
        glfwGetKey(g_window, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS);

    // A minimised window has a zero-sized framebuffer; presenting to it is
    // invalid, so the frame is skipped rather than submitted.
    int fbw = 0, fbh = 0;
    glfwGetFramebufferSize(g_window, &fbw, &fbh);
    if (fbw == 0 || fbh == 0) {
        // A BARE `return` HERE WAS INDISTINGUISHABLE FROM A LOCKUP -- see the
        // note on noteFrameSkipped at the top of this file for why, and for
        // the core that made the point.
        //
        // The CONDITION is left alone: presenting to a zero-sized surface is
        // invalid, so skipping is correct. What was wrong was skipping
        // silently and at maximum speed. The sleep is what the acquire would
        // otherwise have done -- a window with no framebuffer has nothing to
        // draw to and nothing to wait on, so without it this burns a core for
        // as long as the condition lasts.
        //
        // MEASURED, and it is not what it looks like: MINIMISING THE WINDOW
        // DOES NOT REACH THIS BRANCH. Not on X11, where an iconified window
        // keeps its geometry because XGetWindowAttributes answers with the
        // stored width and height whether the window is mapped or not; and
        // not on Wayland either -- Tests/input/wl_minimise_probe.sh drives
        // KWin's own scripting interface to minimise and restore the window
        // on the Wayland backend and this branch reports nothing at all.
        // So the state is rarer than "the user minimised it", and what
        // actually produces it here is not yet known.
        noteFrameSkipped("the framebuffer is 0x0 (the window is minimised, "
                         "unmapped or not yet configured)");
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return;
    }

    // Window resize.
    //
    // Two things have to happen and neither did. The swapchain must be
    // recreated at the new size -- OUT_OF_DATE is not guaranteed on every
    // platform, and under Wayland a resize often does not raise it, so the
    // old-size image is simply scaled up by the compositor and every glyph
    // stretches with the window.
    //
    // And the dialog must be told, because Win32 lays out on WM_SIZE:
    //
    //     case WM_SIZE:
    //         return Resize (hWnd, LOWORD(lParam), HIWORD(lParam), wParam);
    //
    // LaunchpadDialog::Resize moves the Launch/Help/Exit buttons to the new
    // edge, restretches IDC_BLACKBOX and IDC_SHADOW, resizes
    // IDC_MNU_PAGECONTAINER and calls TabAreaResized on every tab. Without the
    // message the controls keep their startup geometry no matter what size the
    // window is.
    {
        static int lastW = 0, lastH = 0;
        if (fbw != lastW || fbh != lastH) {
            lastW = fbw;
            lastH = fbh;
            g_swapChainRebuild = true;

            if (HWND top = orbiter_ActiveDialog()) {
                // The client rectangle first, so GetClientRect agrees with the
                // size reported in the message.
                SetWindowPos(top, nullptr, 0, 0, fbw, fbh,
                             SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
                PostMessageA(top, WM_SIZE, SIZE_RESTORED,
                             MAKELPARAM(fbw, fbh));
            }
        }
    }

    if (g_swapChainRebuild) {
        // BOTH OF THESE TOUCH THE SHARED QUEUE, so both are inside the
        // section. SetMinImageCount calls vkDeviceWaitIdle, and
        // CreateOrResizeWindow calls it again and submits a command buffer of
        // its own -- and vkDeviceWaitIdle is defined as vkQueueWaitIdle on
        // every queue, which is host access to a queue the client's loader
        // threads may be submitting on. Same reason as the
        // RenderDrawData section in renderFrame; see the note there.
        std::lock_guard<std::recursive_mutex> lk(g_deviceMutex);

        ImGui_ImplVulkan_SetMinImageCount(g_minImageCount);

        // Unhook our render pass so the helper does not destroy it. It is the
        // handle the graphics client holds; see the note by g_scenePass.
        g_mainWindowData.RenderPass = VK_NULL_HANDLE;

        // Same usage flags as the first creation -- TRANSFER_SRC included, so
        // a screenshot still works after a resize. See swapchainUsage().
        ImGui_ImplVulkanH_CreateOrResizeWindow(
            g_instance, g_physicalDevice, g_device, &g_mainWindowData,
            g_queueFamily, g_allocator, fbw, fbh, (uint32_t)g_minImageCount,
            swapchainUsage());

        // The helper rebuilt its own colour-only render pass and framebuffers,
        // so the depth attachment has to be put back. Skipping this is a
        // resize that silently drops depth -- the window keeps working and
        // every depth-testing pipeline starts failing validation.
        attachDepth(&g_mainWindowData, (uint32_t)fbw, (uint32_t)fbh);

        // EVERY INDEX INTO THE REBUILT ARRAYS GOES BACK TO ZERO.
        //
        // CreateWindowSwapChain destroys and recreates both Frames and
        // FrameSemaphores and re-derives SemaphoreCount from the new
        // ImageCount -- but it resets neither index. FrameIndex was already
        // being cleared here; SemaphoreIndex was not, and it is the more
        // dangerous of the two: it is only ever reduced modulo the OLD
        // SemaphoreCount, so a rebuild that returns fewer images (3 -> 2, so
        // SemaphoreCount 4 -> 3) can leave it indexing past the end of the
        // vector. That reads a garbage VkSemaphore handle out of freed memory.
        //
        // g_framePending is cleared for a plainer reason: whatever was in
        // flight has been waited out by CreateWindowSwapChain's
        // vkDeviceWaitIdle, and its semaphores no longer exist.
        g_mainWindowData.FrameIndex     = 0;
        g_mainWindowData.SemaphoreIndex = 0;
        g_framePending     = false;
        g_swapChainRebuild = false;
    }

    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    // Walk the dialog tree Win32Dlg.cpp maintains and draw whatever is
    // currently visible. Interaction inside drawDialog posts WM_COMMAND, which
    // the caller will find in the queue as soon as this returns.
    HWND dialogs[64];
    const int n = orbiter_EnumTopLevelDialogs(dialogs, 64);

    // Hide the host window when nothing wants to be drawn in it.
    //
    // Launching a scenario with no graphics client leaves this window with no
    // content at all: Orbiter::CreateRenderWindow calls m_pLaunchpad->Hide()
    // and then, having no gclient, sets hRenderWnd = NULL and creates a
    // ConsoleNG instead. Windows has nothing on screen at that point -- the
    // Launchpad is gone and the session is driven from the console -- but here
    // the GLFW window stayed up as an empty black rectangle still captioned
    // "OpenOrbiter Launchpad", and closing it terminated the session.
    //
    // The window is not destroyed, only hidden: the Launchpad is shown again
    // when the session ends, and recreating the surface and swapchain would be
    // both slower and a good deal more to go wrong.
    {
        // A session keeps the window: the scene is the content, and it
        // routinely has no dialogs open at all. Without this the
        // hide-when-empty rule below would blank the render window the moment
        // a scenario started with a graphics client attached.
        //
        // AND SO DOES A SPLASH, which is the window's only content for the
        // whole of scenario loading: Orbiter::CreateRenderWindow hides the
        // Launchpad before it calls clbkCreateRenderWindow, so between that
        // call and clbkPostCreation there is not one visible dialog. Without
        // the splash counted here the window is hidden for the entire load and
        // the splash is drawn into nothing.
        int anyVisible = (g_sessionActive || g_splash.active) ? 1 : 0;
        for (int i = 0; i < n; ++i) {
            const char *cls = nullptr, *txt = nullptr;
            int id = 0, x = 0, y = 0, cx = 0, cy = 0, vis = 0, en = 0;
            unsigned st = 0;
            orbiter_GetControlInfo(dialogs[i], &cls, &txt, &id, &x, &y,
                                   &cx, &cy, &st, &vis, &en);
            if (vis) { anyVisible = 1; break; }
        }

        static int shown = 1;
        if (anyVisible != shown) {
            shown = anyVisible;
            if (anyVisible) glfwShowWindow(g_window);
            else            glfwHideWindow(g_window);
        }
        // Nothing visible: no frame to build, and no reason to spin the GPU.
        //
        // EndFrame, not a bare return: ImGui asserts if NewFrame is called
        // again without the current frame having been ended, and NewFrame has
        // already run above.
        if (!anyVisible) {
            ImGui::EndFrame();
            // Silent until now, and it is one of the five unpaced exits -- see
            // noteFrameSkipped. This one is REACHED IN NORMAL USE: between the
            // Launchpad closing and clbkPostCreation raising g_sessionActive
            // there is legitimately nothing to draw. So it reports like the
            // others, and the recovery line gives the duration, which is what
            // separates the ordinary sub-second gap from a state that stuck.
            noteFrameSkipped("nothing is visible -- no session, no splash and "
                             "no visible dialog");
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            return;
        }
    }

    // The splash goes FIRST and into the background draw list, which is the
    // counterpart of the reference blitting pSplashScreen to the back buffer
    // before anything else is composited over it.
    drawSplash();

    for (int i = 0; i < n; ++i)
        drawDialog(dialogs[i]);

    // AND THE CORE'S OWN ImGui DIALOGS, which are a completely separate list
    // from the one above and were being drawn nowhere.
    //
    // Two dialog systems live side by side in this program and it is easy to
    // assume one of them is all of them. drawDialog() walks
    // orbiter_EnumTopLevelDialogs -- the HWND tree Win32Dlg.cpp maintains for
    // everything built from an .rc template. But Orbiter's own Select and
    // InputBox are not Win32 dialogs at all: Select.h declares
    // `class Select : public ImGuiDialog`, upstream and unmodified, and they
    // live in DialogManager::DlgImGuiList, which nothing here ever touched.
    //
    // That is why the Orbit MFD's TGT button did nothing visible. It reaches
    // Instrument::OpenSelect_Tgt, which calls g_select->Open() and raises the
    // dialog's active flag correctly; the menu was then built by
    // Select::OnDraw inside DialogManager::ImGuiNewFrame's own ImGui frame,
    // which was closed with EndFrame and never rendered. Every module dialog
    // and every InputBox had the same fate.
    //
    // Here it is inside the frame that is actually submitted and presented,
    // after the Win32-shim dialogs so a Select popup sits above them, and
    // before drawMessageBox so a modal message box still sits above everything.
    // See the note in DialogManager::ImGuiNewFrame for the Windows arrangement
    // this replaces and why the client cannot reproduce it.
    //
    // ONLY WHILE THE SESSION IS RUNNING, and that gate is not caution -- it is
    // the reference's own condition. There, DrawImGuiDialogs is reachable from
    // exactly one place, DialogManager::ImGuiNewFrame, called from exactly one
    // place, Orbiter::Render3DEnvironment -- which does not run during
    // scenario loading. UIHost's pump does: it is what draws the splash, and
    // OutputLoadStatus pumps a frame for every load message.
    //
    // Drawing them then is a null dereference, MEASURED:
    //
    //   #2  Body::Name (this=0x0)              Body.h:33
    //   #3  InfoTarget::Display                MenuInfoBar.cpp:775
    //   #4  DialogManager::DrawImGuiDialogs    DlgMgr.cpp:739
    //   #5  orbiter_DrawImGuiDialogs           DlgMgr.cpp:762
    //   #6  orbiter_PumpFrame                  UIHost.cpp:5294
    //   #7  VulkanClient::OutputLoadStatus     VulkanClient.cpp:1242   <-- loading
    //   ...
    //   #15 Orbiter::InitializeWorld           Orbiter.cpp:260
    //
    // The info bar's InfoTarget reads the focus vessel, and during
    // PlanetarySystem::Read there is no focus vessel yet. Every other dialog
    // in DlgImGuiList has the same exposure: they are written on the
    // assumption that a world exists, because on Windows one always does by
    // the time they are drawn.
    //
    // g_sessionActive is the flag clbkPostCreation raises through
    // orbiter_BeginSession, which is the client's bRunning -- "loading is
    // over" -- so it is precisely the reference's condition and not an
    // approximation of it.
    if (g_sessionActive)
        orbiter_DrawImGuiDialogs();

    // Drawn last so it is above the dialog tree, as a modal is.
    drawMessageBox();

    // Turn this frame's key events into Win32 messages. Done after the widget
    // pass so ImGui's WantTextInput reflects whether a field took focus.
    postKeyboardMessages();

    // And the mouse, to the RENDER window rather than to a dialog. Same
    // reason for the placement: the widget pass above is what sets
    // WantCaptureMouse for this frame, and that flag decides whether the
    // pointer belongs to a dialog or to the scene. See postMouseMessages.
    postMouseMessages();

    // The scripted UI driver, immediately after the real keys so a scripted
    // one lands in the same queue position. Inert unless ORBITER_UI_SCRIPT
    // names a file; see Linux/UiDriver.cpp for why it exists.
    orbiter_UiDriverStep();

    if (g_tracePacing) g_ph.tDlg = std::chrono::steady_clock::now();

    ImGui::Render();
    ImDrawData *drawData = ImGui::GetDrawData();

    const bool minimised = drawData->DisplaySize.x <= 0.0f ||
                           drawData->DisplaySize.y <= 0.0f;
    if (minimised) {
        // The third unpaced exit, and NOT the same test as the framebuffer
        // check above. DisplaySize comes from ImGui_ImplGlfw_NewFrame, which
        // reads glfwGetWindowSize -- the WINDOW, in screen coordinates. The
        // check above reads glfwGetFramebufferSize -- the SURFACE, in pixels.
        // On a scaled display the two differ by the content scale, and either
        // can be zero without the other. See noteFrameSkipped.
        noteFrameSkipped("ImGui's DisplaySize is 0 (the window has no client "
                         "area)");
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    else {
        // The Launchpad's backdrop; a session clears to black instead,
        // because that is what space looks like before anything is drawn in
        // it and it makes the transition visibly correct.
        //
        // g_sessionActive, NOT g_inSceneFrame -- THE SAME BUG THE SCENE HOOK
        // HAD, in the same file, left behind when that one was fixed.
        //
        // There are two pump paths: orbiter_EndSceneFrame sets g_inSceneFrame
        // and pumps, and the Win32Dlg.cpp message loop pumps independently
        // with the flag false. Both build and present a frame. Keyed on the
        // per-frame flag, a session therefore cleared BLACK on frames from one
        // path and DARK GREY on frames from the other, alternating at frame
        // rate -- a visible background flicker, and a screenshot caught
        // whichever landed last.
        //
        // Found by pixel-comparing two runs that should have been identical:
        // the geometry matched exactly and 61% of the pixels still differed,
        // all of them background. Worth recording as a method note -- the
        // difference was invisible in a single screenshot and obvious the
        // moment two were subtracted.
        //
        // The splash counts as a session for this too: SplashScreen() opens
        // with Clear(..., 0x0, ...) -- black -- and the image is letterboxed
        // into it, so anything else would put a grey border round it.
        const bool scene = g_sessionActive || g_splash.active;
        g_mainWindowData.ClearValue.color.float32[0] = scene ? 0.0f : 0.10f;
        g_mainWindowData.ClearValue.color.float32[1] = scene ? 0.0f : 0.10f;
        g_mainWindowData.ClearValue.color.float32[2] = scene ? 0.0f : 0.12f;
        g_mainWindowData.ClearValue.color.float32[3] = 1.00f;
        renderFrame(drawData);
        if (g_tracePacing) g_ph.tRender = std::chrono::steady_clock::now();
        presentFrame();
    }

    if (g_tracePacing) {
        g_ph.tEnd = std::chrono::steady_clock::now();
        const double between = g_ph.havePrev ? msBetween(g_ph.tPrev, g_ph.t0) : 0.0;
        const double total   = g_ph.havePrev ? msBetween(g_ph.tPrev, g_ph.tEnd)
                                             : msBetween(g_ph.t0, g_ph.tEnd);
        // 25 ms: above the 5.8 ms median by enough that a merely-slow frame is
        // not reported, below the 70-130 ms band the hitches live in.
        if (total > 25.0) {
            fprintf(stderr,
                    "frame %6.1f ms | between(Orbiter) %6.1f  poll %5.1f  "
                    "dialogs %5.1f  record+scene %6.1f  present %5.1f\n",
                    total, between,
                    msBetween(g_ph.t0,      g_ph.tPoll),
                    msBetween(g_ph.tPoll,   g_ph.tDlg),
                    msBetween(g_ph.tDlg,    g_ph.tRender),
                    msBetween(g_ph.tRender, g_ph.tEnd));
        }
        g_ph.tPrev    = g_ph.tEnd;
        g_ph.havePrev = true;
    }
}

// ===========================================================================
// Texture upload
//
// Gdi.cpp calls this the first time a bitmap is blitted. It cannot happen at
// LoadBitmap time: uploading needs the Vulkan device, and bitmaps are loaded
// during dialog construction, before the renderer exists.
//
// The returned value is the VkDescriptorSet ImGui binds when drawing, which is
// what ImTextureID is for this backend.
// ===========================================================================

namespace {

uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags want)
{
    VkPhysicalDeviceMemoryProperties props;
    vkGetPhysicalDeviceMemoryProperties(g_physicalDevice, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) &&
            (props.memoryTypes[i].propertyFlags & want) == want)
            return i;
    }
    return (uint32_t)-1;
}

// One sampler shared by every uploaded bitmap. These are UI images drawn at
// their native size, so nearest filtering keeps them crisp rather than
// blurring a 16x16 icon.
VkSampler g_uiSampler = VK_NULL_HANDLE;

VkSampler uiSampler()
{
    if (g_uiSampler != VK_NULL_HANDLE) return g_uiSampler;

    VkSamplerCreateInfo ci{};
    ci.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    ci.magFilter    = VK_FILTER_NEAREST;
    ci.minFilter    = VK_FILTER_NEAREST;
    ci.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ci.minLod       = -1000;
    ci.maxLod       =  1000;
    ci.maxAnisotropy = 1.0f;

    vkCreateSampler(g_device, &ci, g_allocator, &g_uiSampler);
    return g_uiSampler;
}

// The body of orbiter_UploadTexture, with the objects it creates handed back
// so a caller that intends to destroy them can. Declared alongside drawSplash;
// see the note there for why the splash is the one caller that needs them.
VkDescriptorSet uploadTextureEx(const unsigned char *rgba,
                                int width, int height,
                                VkImage *outImage, VkDeviceMemory *outMemory,
                                VkImageView *outView)
{
    if (outImage)  *outImage  = VK_NULL_HANDLE;
    if (outMemory) *outMemory = VK_NULL_HANDLE;
    if (outView)   *outView   = VK_NULL_HANDLE;

    if (!g_initialised || !rgba || width <= 0 || height <= 0)
        return VK_NULL_HANDLE;

    const VkDeviceSize bytes = (VkDeviceSize)width * height * 4;

    // --- image -------------------------------------------------------------
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory imageMem = VK_NULL_HANDLE;

    VkImageCreateInfo ii{};
    ii.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType     = VK_IMAGE_TYPE_2D;
    ii.format        = VK_FORMAT_R8G8B8A8_UNORM;
    ii.extent.width  = (uint32_t)width;
    ii.extent.height = (uint32_t)height;
    ii.extent.depth  = 1;
    ii.mipLevels     = 1;
    ii.arrayLayers   = 1;
    ii.samples       = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling        = VK_IMAGE_TILING_OPTIMAL;
    ii.usage         = VK_IMAGE_USAGE_SAMPLED_BIT |
                       VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ii.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(g_device, &ii, g_allocator, &image) != VK_SUCCESS)
        return VK_NULL_HANDLE;

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(g_device, image, &req);

    VkMemoryAllocateInfo ai{};
    ai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize  = req.size;
    ai.memoryTypeIndex = findMemoryType(req.memoryTypeBits,
                                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ai.memoryTypeIndex == (uint32_t)-1 ||
        vkAllocateMemory(g_device, &ai, g_allocator, &imageMem) != VK_SUCCESS) {
        vkDestroyImage(g_device, image, g_allocator);
        return VK_NULL_HANDLE;
    }
    vkBindImageMemory(g_device, image, imageMem, 0);

    // --- view --------------------------------------------------------------
    VkImageView view = VK_NULL_HANDLE;
    VkImageViewCreateInfo vi{};
    vi.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image    = image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format   = VK_FORMAT_R8G8B8A8_UNORM;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.layerCount = 1;
    vkCreateImageView(g_device, &vi, g_allocator, &view);

    // --- staging buffer ----------------------------------------------------
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory stagingMem = VK_NULL_HANDLE;

    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size  = bytes;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vkCreateBuffer(g_device, &bi, g_allocator, &staging);

    vkGetBufferMemoryRequirements(g_device, staging, &req);
    ai.allocationSize  = req.size;
    ai.memoryTypeIndex = findMemoryType(req.memoryTypeBits,
                                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    vkAllocateMemory(g_device, &ai, g_allocator, &stagingMem);
    vkBindBufferMemory(g_device, staging, stagingMem, 0);

    void *mapped = nullptr;
    vkMapMemory(g_device, stagingMem, 0, bytes, 0, &mapped);
    memcpy(mapped, rgba, (size_t)bytes);

    // The memory is not guaranteed coherent, so the write is flushed before
    // the GPU reads it.
    VkMappedMemoryRange range{};
    range.sType  = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    range.memory = stagingMem;
    range.size   = VK_WHOLE_SIZE;
    vkFlushMappedMemoryRanges(g_device, 1, &range);
    vkUnmapMemory(g_device, stagingMem);

    // --- copy, on a one-shot command buffer --------------------------------
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo pci{};
    pci.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.queueFamilyIndex = g_queueFamily;
    pci.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    vkCreateCommandPool(g_device, &pci, g_allocator, &pool);

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cbi{};
    cbi.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbi.commandPool        = pool;
    cbi.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbi.commandBufferCount = 1;
    vkAllocateCommandBuffers(g_device, &cbi, &cmd);

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);

    VkImageMemoryBarrier toDst{};
    toDst.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toDst.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
    toDst.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDst.image               = image;
    toDst.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
    toDst.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    toDst.subresourceRange.levelCount = 1;
    toDst.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &toDst);

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent.width  = (uint32_t)width;
    region.imageExtent.height = (uint32_t)height;
    region.imageExtent.depth  = 1;
    vkCmdCopyBufferToImage(cmd, staging, image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    VkImageMemoryBarrier toRead = toDst;
    toRead.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toRead.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &toRead);

    vkEndCommandBuffer(cmd);

    VkSubmitInfo submit{};
    submit.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers    = &cmd;
    // Shared queue; see g_deviceMutex at the top of this file. The submit and
    // the wait are one critical section: another thread's submit landing
    // between them would be waited on here as well, which is harmless, but a
    // submit racing this one is not.
    {
        std::lock_guard<std::recursive_mutex> lk(g_deviceMutex);
        vkQueueSubmit(g_queue, 1, &submit, VK_NULL_HANDLE);

        // Uploads happen once per bitmap during the first frame that draws it,
        // so waiting here is simpler than tracking a fence and costs nothing
        // after startup.
        vkQueueWaitIdle(g_queue);
    }

    vkDestroyCommandPool(g_device, pool, g_allocator);
    vkDestroyBuffer(g_device, staging, g_allocator);
    vkFreeMemory(g_device, stagingMem, g_allocator);

    VkDescriptorSet set = ImGui_ImplVulkan_AddTexture(
        uiSampler(), view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // The image, its memory and its view are handed back to a caller that
    // asked for them, and otherwise deliberately leaked: those are the
    // dialog's bitmaps, live for the process lifetime, and the descriptor set
    // references them.
    if (outImage)  *outImage  = image;
    if (outMemory) *outMemory = imageMem;
    if (outView)   *outView   = view;
    return set;
}

} // namespace

extern "C" unsigned long long orbiter_UploadTexture(const unsigned char *rgba,
                                                    int width, int height)
{
    return (unsigned long long)uploadTextureEx(rgba, width, height,
                                               nullptr, nullptr, nullptr);
}

// ---------------------------------------------------------------------------
// AN ImTextureID FOR A SURFACE THE GRAPHICS CLIENT ALREADY OWNS.
// ---------------------------------------------------------------------------
//
// This is the Vulkan answer to one line of the reference:
//
//     uint64_t D3D9Client::clbkImGuiSurfaceTexture(SURFHANDLE surf)
//     {
//         ImTextures.push_back(surf);
//         clbkIncrSurfaceRef(surf);
//         LPDIRECT3DTEXTURE9 pTxt = SURFACE(surf)->GetTexture();
//         return (uint64_t)pTxt;
//     }
//
// D3D9 can return the texture pointer itself, because ImGui's DX9 backend
// binds an IDirect3DTexture9 directly. ImGui's VULKAN backend binds a
// DESCRIPTOR SET, which has to be allocated from ImGui's own pool by
// ImGui_ImplVulkan_AddTexture -- and only this file has that pool, the
// sampler and the backend. The client cannot reach any of them without
// linking a second ImGui Vulkan backend, which is exactly what VulkanClient.h
// exists to avoid. Hence an export.
//
// WHAT IT COST WHILE IT RETURNED 0. VulkanClient::clbkImGuiSurfaceTexture was
// a stub returning 0 and ImGui::GetImTextureID handed that straight to
// ImGui::Image, so every icon in the F4 menu bar drew with descriptor set 0 --
// whatever happened to be bound -- and came out as noise. Eighteen buttons of
// static. Nothing logged, because nothing was wrong from Vulkan's point of
// view: a valid draw with the wrong descriptor.
//
// CACHED BY VIEW, and it has to be. ImGui_ImplVulkan_AddTexture ALLOCATES a
// descriptor set per call; the menu bar asks once per icon per frame, so
// calling through would exhaust ImGui's pool in seconds. The reference has no
// equivalent problem -- returning a texture pointer allocates nothing -- which
// is why it needs no cache and this does.
//
// The reference's per-frame ref-count hold (ImTextures + clbkIncrSurfaceRef,
// released in clbkImGuiRenderDrawData) is NOT reproduced, and does not need to
// be: it exists so a surface released mid-frame outlives the draw that
// references it. Here the client calls orbiter_ImGuiForgetTexture from
// clbkReleaseSurface, so the entry goes when the view does.
namespace {
std::map<VkImageView, VkDescriptorSet> g_imguiTexCache;
}

extern "C" unsigned long long orbiter_ImGuiTextureFromView(void *imageView)
{
    if (!g_initialised || !imageView) return 0;

    VkImageView view = (VkImageView)imageView;

    auto it = g_imguiTexCache.find(view);
    if (it != g_imguiTexCache.end())
        return (unsigned long long)it->second;

    VkDescriptorSet set = ImGui_ImplVulkan_AddTexture(
        uiSampler(), view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (set == VK_NULL_HANDLE) return 0;

    g_imguiTexCache[view] = set;
    return (unsigned long long)set;
}

// Called from VulkanClient::clbkReleaseSurface, before the view is destroyed.
//
// Deferred rather than immediate would be safer if a descriptor set could
// still be in flight, but ImGui_ImplVulkan_RemoveTexture frees it back to
// ImGui's pool and ImGui's own backend does the same on shutdown while frames
// may be pending; matching its behaviour is the right call rather than
// inventing a second lifetime rule.
extern "C" void orbiter_ImGuiForgetTexture(void *imageView)
{
    if (!g_initialised || !imageView) return;

    auto it = g_imguiTexCache.find((VkImageView)imageView);
    if (it == g_imguiTexCache.end()) return;

    ImGui_ImplVulkan_RemoveTexture(it->second);
    g_imguiTexCache.erase(it);
}

// The image a custom swap chain presents into a child control.
//
// Called from gcCore::FlipSwap with the swap's back-buffer view, and with a
// NULL view from RegisterSwap (when it re-creates a swap at a new size) and
// from ReleaseSwap. See the note beside g_controlImages for why "present into
// that window" is spelled this way on a platform with one window.
//
// The VIEW is stored rather than the descriptor set, so the lookup goes
// through orbiter_ImGuiTextureFromView on the frame that draws it. That keeps
// the ImGui call inside the frame and inside this file, and it means a view
// dropped by orbiter_ImGuiForgetTexture cannot leave a stale descriptor
// behind here -- the next lookup simply re-adds it.
extern "C" void orbiter_SetControlImage(HWND h, void *imageView, int w, int hgt)
{
    if (!h) return;

    if (!imageView) {
        g_controlImages.erase(h);
        return;
    }

    ControlImage &ci = g_controlImages[h];
    ci.view = imageView;
    ci.w    = w;
    ci.h    = hgt;
}

extern "C" void orbiter_ShutdownUI(void)
{
    if (!g_initialised) return;

    vkDeviceWaitIdle(g_device);

    // The back-buffer readback staging buffer, if a screenshot was ever taken.
    if (g_captureBuf) { vkDestroyBuffer(g_device, g_captureBuf, g_allocator); g_captureBuf = VK_NULL_HANDLE; }
    if (g_captureMem) { vkFreeMemory(g_device, g_captureMem, g_allocator);    g_captureMem = VK_NULL_HANDLE; }
    g_captureSize = 0;
    g_captureDone = g_captureArmed = false;

    // Likewise ours: the splash image, its memory, its view and its descriptor
    // set. Normally already gone -- clbkPostCreation clears it and
    // orbiter_EndSession clears it again -- but a session that never reached
    // clbkPostCreation would otherwise leave a VkImage alive past the device.
    orbiter_ClearSplash();
    // Before the device goes. The depth image, its memory and its view are
    // ours, not ImGui's, so ImGui_ImplVulkanH_DestroyWindow will not free them.
    destroyDepthResources();

    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    // DestroyWindow destroys whatever is in wd->RenderPass, which is our
    // g_scenePass -- so the global is cleared rather than destroyed again.
    // Freeing it here as well would be a double free of a live handle.
    ImGui_ImplVulkanH_DestroyWindow(g_instance, g_device,
                                    &g_mainWindowData, g_allocator);
    g_scenePass       = VK_NULL_HANDLE;
    g_scenePassColour = VK_FORMAT_UNDEFINED;
    g_scenePassDepth  = VK_FORMAT_UNDEFINED;
    g_framePending    = false;

    vkDestroyDescriptorPool(g_device, g_descriptorPool, g_allocator);
    vkDestroyDevice(g_device, g_allocator);
    vkDestroyInstance(g_instance, g_allocator);

    glfwDestroyWindow(g_window);
    glfwTerminate();

    g_initialised = false;
}


// ===========================================================================
// Monitor geometry
//
// Declared in windows.h for OVP/VulkanClient's FixOutOfScreenPositions, which
// pulls an off-screen popup back into view and needs to know which monitor it
// landed on and how big that monitor is. They live here rather than in
// Win32Dlg.cpp because GLFW is the thing that knows, and GLFW is included
// here.
//
// HMONITOR is opaque on Windows and opaque here: it carries the GLFW monitor
// index plus one, so that index 0 is not the same value as NULL.
//
// dwFlags is accepted and its three documented values honoured, but the
// window's position is what actually decides: a window is "on" the monitor
// whose work area contains its top-left corner, and the nearest monitor
// otherwise -- which is what MONITOR_DEFAULTTONEAREST asks for and the only
// value the client passes.
// ===========================================================================

extern "C" HMONITOR MonitorFromWindow(HWND h, DWORD dwFlags)
{
    int count = 0;
    GLFWmonitor **mons = glfwGetMonitors(&count);
    if (!mons || count <= 0) return nullptr;

    // Where is the window? orbiter_* windows are the shim's own, not GLFW's,
    // so the position comes from the shim; a window it does not know about
    // falls back to the GLFW window's own position, which is the render
    // window and the case that matters.
    RECT r = { 0, 0, 0, 0 };
    if (!h || !GetWindowRect(h, &r)) {
        int wx = 0, wy = 0;
        if (g_window) glfwGetWindowPos(g_window, &wx, &wy);
        r.left = wx; r.top = wy; r.right = wx; r.bottom = wy;
    }

    int best = -1;
    long bestDist = 0;

    for (int i = 0; i < count; i++) {
        int mx = 0, my = 0, mw = 0, mh = 0;
        glfwGetMonitorWorkarea(mons[i], &mx, &my, &mw, &mh);

        if (r.left >= mx && r.left < mx + mw && r.top >= my && r.top < my + mh)
            return (HMONITOR)(intptr_t)(i + 1);      // contained: done

        // Squared distance from the window corner to the monitor rectangle,
        // for MONITOR_DEFAULTTONEAREST.
        long dx = 0, dy = 0;
        if (r.left < mx)            dx = mx - r.left;
        else if (r.left >= mx + mw) dx = r.left - (mx + mw - 1);
        if (r.top < my)             dy = my - r.top;
        else if (r.top >= my + mh)  dy = r.top - (my + mh - 1);
        const long dist = dx * dx + dy * dy;

        if (best < 0 || dist < bestDist) { best = i; bestDist = dist; }
    }

    if (dwFlags == MONITOR_DEFAULTTONULL)    return nullptr;
    if (dwFlags == MONITOR_DEFAULTTOPRIMARY) return (HMONITOR)(intptr_t)1;
    return (HMONITOR)(intptr_t)(best + 1);
}

extern "C" BOOL GetMonitorInfoA(HMONITOR hMon, LPMONITORINFO info)
{
    if (!info) return FALSE;
    // Windows requires the caller to set cbSize and rejects anything else;
    // the same check is made here so a caller that forgets is told.
    if (info->cbSize != sizeof(MONITORINFO)) return FALSE;

    int count = 0;
    GLFWmonitor **mons = glfwGetMonitors(&count);
    const int idx = (int)(intptr_t)hMon - 1;
    if (!mons || idx < 0 || idx >= count) return FALSE;

    // rcMonitor is the whole display, rcWork excludes taskbars and docks.
    // GLFW gives both: the video mode is the display, the work area is what
    // is left. The client reads rcMonitor.
    int wx = 0, wy = 0, ww = 0, wh = 0;
    glfwGetMonitorWorkarea(mons[idx], &wx, &wy, &ww, &wh);

    int px = 0, py = 0;
    glfwGetMonitorPos(mons[idx], &px, &py);
    const GLFWvidmode *vm = glfwGetVideoMode(mons[idx]);

    info->rcMonitor.left   = px;
    info->rcMonitor.top    = py;
    info->rcMonitor.right  = px + (vm ? vm->width  : ww);
    info->rcMonitor.bottom = py + (vm ? vm->height : wh);

    info->rcWork.left   = wx;
    info->rcWork.top    = wy;
    info->rcWork.right  = wx + ww;
    info->rcWork.bottom = wy + wh;

    info->dwFlags = (idx == 0) ? MONITORINFOF_PRIMARY : 0;
    return TRUE;
}
