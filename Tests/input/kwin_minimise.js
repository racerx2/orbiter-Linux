// KWin script: minimise or restore Orbiter's window, by CAPTION.
//
// Loaded by wl_minimise_probe.sh. KWin scripts take NO ARGUMENTS, so this is a
// template: the probe substitutes @WANT@ with true or false and loads the
// resulting concrete file under its own plugin name.
//
// WHY A KWIN SCRIPT AND NOT xdotool
//
// The window under test runs on the WAYLAND backend, where there is no X
// window for xdotool to address at all. KWin's own scripting interface is the
// only route to minimise/restore that works there, and it targets the window
// by caption rather than by focus, so it still works once the window has been
// minimised and is no longer active.
//
// WHY MINIMISE AT ALL: on Wayland a minimised window's surface is unmapped and
// the compositor configures it to 0x0, which is the ONE condition that reaches
// orbiter_PumpFrame's zero-framebuffer branch without a code change. On X11 an
// iconified window keeps its geometry -- XGetWindowAttributes answers with the
// old width and height -- so the same test under XWayland reaches nothing,
// which is exactly what the first attempt measured.

var want = @WANT@;

function windows() {
    if (typeof workspace.windowList === 'function') return workspace.windowList();
    if (typeof workspace.clientList === 'function') return workspace.clientList();
    return [];
}

var list = windows();
var hits = 0;
for (var i = 0; i < list.length; i++) {
    var w = list[i];
    var cap = w.caption ? String(w.caption) : '';
    var cls = w.resourceClass ? String(w.resourceClass) : '';
    if (cap.indexOf('rbiter') === -1 && cls.indexOf('rbiter') === -1) continue;
    w.minimized = want;
    hits++;
    print('orbiterprobe: "' + cap + '" class="' + cls + '" minimized=' + w.minimized);
}
print('orbiterprobe: want=' + want + ' windows=' + list.length + ' matched=' + hits);
