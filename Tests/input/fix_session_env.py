#!/usr/bin/env python3
# ===========================================================================
# fix_session_env.py -- replace the hardcoded X authority cookie in every
# harness with a run-time lookup (session.sh).
#
# Run once. It is idempotent: a file already converted is reported "clean".
#
# The literal /run/user/1000/xauth_swQDJU was minted by the display manager
# at ONE login. The user rebooted on 2026-09-09 and it became xauth_UvdTHA,
# so every harness that carried it stopped being able to reach the X server
# and reported "no window -- process never got that far" -- which reads as an
# Orbiter failure, not a harness failure. See session.sh.
#
# Two shapes exist in the tree and both are handled:
#
#   1.  export DISPLAY=:0
#       export XAUTHORITY=/run/user/1000/xauth_swQDJU
#
#   2.  setsid nohup env XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0 \
#           DISPLAY=:0 XDG_SESSION_TYPE=wayland XAUTHORITY=/run/user/1000/xauth_swQDJU \
#           ORBITER_GLFW_PLATFORM=x11 <harness-specific vars> \
#
# Shape 2's whole prefix is exactly what session.sh exports, so it collapses
# to `setsid nohup \` plus whatever was harness-specific on the third line.
# ===========================================================================
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
STALE = "/run/user/1000/xauth_swQDJU"

SOURCE_BLOCK = (
    "# DISPLAY/XAUTHORITY/WAYLAND_DISPLAY resolved from the RUNNING session.\n"
    "# The cookie filename changes at every login, so a literal here survives\n"
    "# exactly until the next reboot and then reports \"no window\".\n"
    '. "$(dirname "$0")/session.sh"\n'
)

# Shape 2, as one block. The three env lines are consumed whole; group 1 is
# the third line's indentation and group 2 is whatever was harness-specific
# on it after ORBITER_GLFW_PLATFORM=x11 (an ORBITER_UI_SCRIPT=..., usually).
SHAPE2 = re.compile(
    r"setsid nohup env XDG_RUNTIME_DIR=\S+ WAYLAND_DISPLAY=\S+ \\\n"
    r"[ \t]*DISPLAY=\S+ XDG_SESSION_TYPE=\S+ XAUTHORITY=\S+ \\\n"
    r"([ \t]*)ORBITER_GLFW_PLATFORM=x11[ \t]*(.*?)[ \t]*\\\n"
)

# Shape 1: the export pair, or the XAUTHORITY export alone.
SHAPE1_PAIR = re.compile(r"^export DISPLAY=\S+\nexport XAUTHORITY=\S+\n", re.M)
SHAPE1_ONE = re.compile(r"^export XAUTHORITY=\S+\n", re.M)


def convert(path):
    with open(path, "r") as f:
        text = f.read()
    if STALE not in text:
        return "clean"
    original = text
    notes = []

    def shape2_sub(m):
        indent, rest = m.group(1), m.group(2)
        # `env` survives only if something harness-specific is still being
        # set; with nothing left it would be a bare `env` and is dropped.
        if rest:
            return "setsid nohup env \\\n%s%s \\\n" % (indent, rest)
        return "setsid nohup \\\n"

    new, n2 = SHAPE2.subn(shape2_sub, text)
    if n2:
        notes.append("shape2 x%d" % n2)
        text = new

    new, n1 = SHAPE1_PAIR.subn(SOURCE_BLOCK, text)
    if n1:
        notes.append("shape1-pair x%d" % n1)
        text = new
    new, n1b = SHAPE1_ONE.subn(SOURCE_BLOCK, text)
    if n1b:
        notes.append("shape1-solo x%d" % n1b)
        text = new

    if STALE in text:
        return "STILL STALE -- unrecognised shape, left alone"
    if text == original:
        return "no change"

    # A harness that only had shape 2 never sourced session.sh; it must, or
    # xdotool in the same script has no cookie. Insert after `set -u` if the
    # source line is not already present.
    if '. "$(dirname "$0")/session.sh"' not in text:
        text, k = re.subn(r"^(set -u\n)", r"\1\n" + SOURCE_BLOCK, text, count=1,
                          flags=re.M)
        notes.append("sourced after set -u" if k else "NO set -u -- SOURCE NOT ADDED")

    with open(path, "w") as f:
        f.write(text)
    return ", ".join(notes)


def main():
    names = sorted(n for n in os.listdir(HERE) if n.endswith(".sh"))
    for n in names:
        if n == "session.sh":
            continue
        print("%-22s %s" % (n, convert(os.path.join(HERE, n))))


if __name__ == "__main__":
    main()
