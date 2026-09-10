#!/bin/bash
# ===========================================================================
# drive.sh -- run Orbiter and drive its keyboard, mouse and wheel for real.
#
# THE SPLIT, AND WHY IT IS THIS WAY.  On this desk (KDE/Wayland, Orbiter on
# XWayland) the two halves of "synthetic input" have opposite failure modes,
# measured, twice:
#
#   * xdotool POSITION works.  It moves the X pointer the GLFW window reads,
#     and the client reports exactly frame-relative coordinates back.
#   * xdotool KEYS and BUTTONS never arrive -- ORBITER_TRACE_INPUT shows
#     focused=1 with glfwPressed=0 throughout an `xdotool key F8`.
#   * uinject (uinput/evdev) KEYS, BUTTONS and WHEEL arrive.
#   * uinject POSITION is unreliable: it is a relative device, so libinput's
#     pointer acceleration rescales the deltas.
#
# So: position with xdotool, act with uinject.  Neither alone is enough.
#
# ONE LONG-LIVED INJECTOR, NOT ONE PER COMMAND.  Creating a uinput device
# WARPS THE CURSOR -- measured: the pointer jumped from the (300,250) it had
# just been moved to, to (1154,610), on every `uinject` invocation, and the
# click or wheel then landed there instead.  That is why the injector is
# opened once, before any positioning, and held on a FIFO for the whole run.
# A per-command injector silently aims every click somewhere else.
# ===========================================================================
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
BUILD=/home/racerx/orbiter-native/build
SHOTS=/home/racerx/orbiter-native/_shots
UINJECT="$HERE/uinject"

# DISPLAY/XAUTHORITY/WAYLAND_DISPLAY resolved from the RUNNING session.
# The cookie filename changes at every login, so a literal here survives
# exactly until the next reboot and then reports "no window".
. "$(dirname "$0")/session.sh"

SCN="${1:-Delta-glider/Virtual cockpit}"
LOG="${2:-/tmp/drive.txt}"
FIFO=/tmp/uinject.fifo

start_orbiter() {
    pgrep -x Orbiter | xargs -r kill; sleep 3
    cd "$BUILD" || exit 1
    setsid nohup env \
        ORBITER_TRACE_MSG=1 \
        ./Orbiter "--scenariox=$SCN" --maxframes=1000000 --keeplog \
        >"$LOG" 2>&1 </dev/null & disown
    ORBPID=$!
    # Kill THIS run on any exit, including an interrupt -- by PID, so a
    # session the user started themselves is never touched. An orphaned
    # run was once found alive SIXTEEN HOURS later, sharing the GPU with
    # the user's own Orbiter when that one took a SIGSEGV in the driver --
    # which made the crash a contaminated sample. See
    # the porting notes.
    trap '[ "${KEEP:-0}" = 1 ] || kill "$ORBPID" 2>/dev/null' EXIT INT TERM
    sleep 38
    WID=$(xdotool search --name "OpenOrbiter" | tail -1)
    [ -z "$WID" ] && { echo "FAIL: no window"; exit 1; }
    xdotool windowactivate --sync "$WID"; sleep 1
    eval "$(xdotool getwindowgeometry --shell "$WID")"
    OFF_X=$X; OFF_Y=$Y; CW=$WIDTH; CH=$HEIGHT
    echo "window frame=($OFF_X,$OFF_Y) size=${CW}x${CH} id=$WID"
    start_injector
    calibrate
}

start_injector() {
    rm -f "$FIFO"; mkfifo "$FIFO"
    "$UINJECT" - < "$FIFO" >/dev/null 2>&1 &
    UIPID=$!
    exec 3>"$FIFO"          # holds the FIFO open for the whole run
    sleep 2                 # device created; its cursor warp happens NOW,
}                           # before anything is positioned

stop_injector() { exec 3>&-; wait "$UIPID" 2>/dev/null; rm -f "$FIFO"; }

inj() { echo "$*" >&3; }

# The client-area origin is VERIFIED, not assumed. A window manager frame
# would offset every cockpit click, and a wrong offset looks exactly like
# "the click did nothing" rather than "the click missed".
calibrate() {
    local want_x=300 want_y=250 c cx cy
    xdotool mousemove $((OFF_X + want_x)) $((OFF_Y + want_y)); sleep 1.0
    c=$(awk '/\[mouse\]/{l=$0} END{print l}' "$LOG" |
        sed -n 's/.*pos=(\(-\?[0-9]*\),\(-\?[0-9]*\)).*/\1 \2/p')
    cx=${c% *}; cy=${c#* }
    [ -z "$cx" ] && { echo "FAIL: no [mouse] trace (ORBITER_TRACE_MSG set? session running?)"; exit 1; }
    if [ "$cx" != "$want_x" ] || [ "$cy" != "$want_y" ]; then
        echo "FAIL: frame+($want_x,$want_y) reported as client ($cx,$cy) -- frame inset"
        exit 1
    fi
    echo "client origin verified: frame+($want_x,$want_y) -> client ($cx,$cy)"
}

# Everything below takes CLIENT coordinates -- the same ones Orbiter's trace
# prints, and the same ones a panel's click zones are defined in.
mv()    { xdotool mousemove $((OFF_X + $1)) $((OFF_Y + $2)); sleep 1.0; }
key()   { inj "key $1 ${2:-150}"; sleep 1.6; }
click() { inj "click ${1:-l}";    sleep 1.2; }
wheel() { inj "wheel $1";         sleep 1.2; }
shot()  { import -window "$WID" "$SHOTS/$1.png" 2>/dev/null; echo "  shot $1"; }

# A drag: the button is held on the persistent injector while xdotool moves.
drag() { # drag <l|r|m> <x0> <y0> <x1> <y1>
    local b=$1 x0=$2 y0=$3 x1=$4 y1=$5 i
    mv "$x0" "$y0"
    inj "btn $b down"; sleep 0.5
    for i in 1 2 3 4 5 6 7 8; do
        xdotool mousemove $((OFF_X + x0 + (x1-x0)*i/8)) $((OFF_Y + y0 + (y1-y0)*i/8))
        sleep 0.15
    done
    inj "btn $b up"; sleep 1.0
}
