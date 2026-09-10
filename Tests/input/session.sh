# ===========================================================================
# session.sh -- resolve the desktop session, at run time, for every harness.
#
# SOURCE this, do not execute it:   . "$(dirname "$0")/session.sh"
#
# WHY THIS FILE EXISTS
#
# Every harness in this directory used to carry
#
#     export XAUTHORITY=/run/user/1000/xauth_swQDJU
#
# as a literal. That filename is generated FRESH BY THE DISPLAY MANAGER ON
# EVERY LOGIN. The user rebooted on 2026-09-09 and the cookie became
# xauth_UvdTHA, so all thirty-odd harnesses began reporting
#
#     no window -- process never got that far
#
# which reads exactly like an application that failed to start. It is not:
# it is the harness failing to reach the X server. That is the false-negative
# shape section A-L of the register warns about -- "a harness line that can
# only fail for the wrong reason is worse than one that cannot fail, because
# it looks like a check". A stale cookie would have been read as a regression
# in Orbiter.
#
# WHAT IT SETS
#
#   DISPLAY, XAUTHORITY, WAYLAND_DISPLAY, XDG_RUNTIME_DIR, XDG_SESSION_TYPE
#
# resolved from the RUNNING session rather than assumed. The desk is Wayland
# with XWayland; Orbiter is launched with ORBITER_GLFW_PLATFORM=x11, and
# xdotool -- used for pointer position and window management, never for keys
# or buttons (see section F of the register) -- needs the X half.
#
# It refuses to guess: if no cookie can be found it says so on stderr and
# returns non-zero, so a harness can stop rather than run blind.
# ===========================================================================

orbiter_session_env() {
    local uid rt

    uid=$(id -u)
    rt="${XDG_RUNTIME_DIR:-/run/user/$uid}"

    # Preferred source: the environment of a process that IS the session.
    # This is authoritative -- it is what the compositor itself was given --
    # and it costs one read. Anything else here is a fallback.
    local p env_display env_auth env_wl env_type
    for p in $(pidof plasmashell kwin_wayland gnome-shell xfwm4 2>/dev/null); do
        [ -r "/proc/$p/environ" ] || continue
        env_display=$(tr '\0' '\n' < "/proc/$p/environ" | awk -F= '$1=="DISPLAY"      {print $2; exit}')
        env_auth=$(   tr '\0' '\n' < "/proc/$p/environ" | awk -F= '$1=="XAUTHORITY"   {print $2; exit}')
        env_wl=$(     tr '\0' '\n' < "/proc/$p/environ" | awk -F= '$1=="WAYLAND_DISPLAY" {print $2; exit}')
        env_type=$(   tr '\0' '\n' < "/proc/$p/environ" | awk -F= '$1=="XDG_SESSION_TYPE" {print $2; exit}')
        [ -n "$env_display" ] && [ -n "$env_auth" ] && break
    done

    # Fallback: the cookie the display manager left in the runtime directory.
    # One file, randomly named, hence the glob -- which is the whole reason
    # this function exists.
    if [ -z "${env_auth:-}" ]; then
        local c
        for c in "$rt"/xauth_*; do
            [ -f "$c" ] && env_auth="$c" && break
        done
    fi
    [ -z "${env_auth:-}" ] && [ -f "$HOME/.Xauthority" ] && env_auth="$HOME/.Xauthority"

    if [ -z "${env_auth:-}" ]; then
        echo "session.sh: no X authority cookie found under $rt or \$HOME." >&2
        echo "session.sh: is anyone logged in on this machine?" >&2
        return 1
    fi

    export XDG_RUNTIME_DIR="$rt"
    export DISPLAY="${env_display:-${DISPLAY:-:0}}"
    export XAUTHORITY="$env_auth"
    export WAYLAND_DISPLAY="${env_wl:-wayland-0}"
    export XDG_SESSION_TYPE="${env_type:-wayland}"

    # Orbiter is driven through XWayland by default: xdotool needs X, and
    # every harness that positions the pointer or reads window geometry uses
    # it. PLATFORM=wayland in the environment overrides that.
    #
    # THAT DEFAULT IS ALSO A BLIND SPOT, and it is worth stating here rather
    # than in one harness. `initialise()` in UIHost.cpp only honours
    # ORBITER_GLFW_PLATFORM if it is SET; with it unset GLFW prefers Wayland
    # whenever a compositor is present. So the user's own ./Orbiter runs the
    # WAYLAND backend and every harness in this directory runs XWayland --
    # they are not the same program. Conditions that exist only on one of
    # them (a compositor configure of 0x0 during an interactive move, a
    # pointer release swallowed by a move grab) cannot be reproduced from a
    # harness that forces the other. See the porting notes.
    export ORBITER_GLFW_PLATFORM="${PLATFORM:-x11}"
    return 0
}

# ---------------------------------------------------------------------------
# THE CLEANUP TRAP, AND THE ONE WAY OUT OF IT.
#
# Every harness that launches Orbiter now ends its own session on exit:
#
#     ORBPID=$!
#     trap '[ "${KEEP:-0}" = 1 ] || kill "$ORBPID" 2>/dev/null' EXIT INT TERM
#
# By PID, never `pkill -x Orbiter`, so a session the user started themselves is
# never touched. It exists because the harnesses pkill at the START of a run
# and not at the end, so an interrupted run -- which is most of them while a
# test is being developed -- left Orbiter alive indefinitely. One was found
# SIXTEEN HOURS later, sharing the GPU with the user's own Orbiter at the
# moment that one took a SIGSEGV inside the driver, which made the crash a
# contaminated sample. See the porting notes.
#
# `KEEP=1 ./somesweep.sh` leaves the session up, for the case where the point
# of the run is to have something on screen to look at afterwards.
#
# ONE HARNESS IS DELIBERATELY EXEMPT: gdbrun.sh exists to leave gdb and its
# inferior alive so a later shell can drive them through a fifo. An EXIT trap
# there would kill the session the moment setup finished. The reason is
# written at the site.
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# KEEP THE BINARY THAT PRODUCED THE CORE.
#
# A core dump is only readable against the EXACT executable that produced it.
# gdb matches by build id, and an incremental relink changes it -- so a rebuild
# turns every earlier core into
#
#     #0  0x00005b74c2c8e2d0 in ?? ()
#     Cannot access memory at address 0x73bc88
#
# with no symbols and no globals.
#
# That is not hypothetical. The core from the freeze the user reported on
# 2026-09-09 was read once -- fps, framecount, bVisible, IO.MouseDown -- and
# then Orbiter was rebuilt to add instrumentation. When the reading needed to
# be extended (ImGui's FrameCount against Orbiter's, which would have said
# which of the five unpaced exits the pump was sitting in) the core was already
# unreadable, and the evidence for the whole diagnosis was gone.
# the porting notes record what that cost.
#
# So every harness run snapshots the current binary first, keyed by build id so
# an unchanged binary costs one `readelf` and no copy. Five are kept.
# ---------------------------------------------------------------------------
orbiter_keep_binary() {
    local bin=/home/racerx/orbiter-native/build/Orbiter
    local hist=/home/racerx/orbiter-native/build/binhist
    [ -x "$bin" ] || return 0

    local id
    id=$(readelf -n "$bin" 2>/dev/null \
         | awk '/Build ID:/ {print $3; exit}')
    [ -z "$id" ] && id=$(stat -c %Y "$bin" 2>/dev/null) || true
    [ -z "$id" ] && return 0

    mkdir -p "$hist" || return 0
    [ -f "$hist/Orbiter.$id" ] && return 0
    cp -p "$bin" "$hist/Orbiter.$id" 2>/dev/null || return 0

    # Keep the five most recent; a build is 55 MB.
    ls -1t "$hist"/Orbiter.* 2>/dev/null | tail -n +6 | while read -r old; do
        rm -f "$old"
    done
    return 0
}

# The whole point is that a harness sourcing this gets the variables without
# having to remember to call anything.
orbiter_session_env || return 1 2>/dev/null || exit 1
orbiter_keep_binary
