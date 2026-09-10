#!/usr/bin/env python3
# ===========================================================================
# fix_cleanup_trap.py -- give every harness that LAUNCHES ORBITER a cleanup
# trap, so an interrupted run does not leave a session alive.
#
# Run once. Files that already have a trap are skipped.
#
# WHY
#
# The harnesses pkill at the START of a run and not at the end, so a run that
# is interrupted -- which is most of them, while a test is being developed --
# leaves Orbiter running indefinitely. One was found SIXTEEN HOURS later, and
# it was alive when the user's own ./Orbiter took a SIGSEGV inside the GPU
# driver: two instances on one GPU, which makes that crash a contaminated
# sample. See the porting notes.
#
# By PID rather than `pkill -x Orbiter`, so this never kills a session the
# user started themselves alongside a test.
#
# WHAT IT MATCHES
#
# The launch block ends in `& disown` or `&` + a `disown` line. Only blocks
# that actually run ./Orbiter are trapped -- gdbrun.sh also backgrounds a
# `sleep infinity` to hold a fifo open, and trapping that PID would kill the
# fifo instead of the session. A launch inside a loop is fine: the trap body
# is single-quoted, so $ORBPID is read when the trap FIRES, and re-installing
# the same trap each iteration is harmless.
# ===========================================================================
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))

TRAP = (
    "{i}ORBPID=$!\n"
    "{i}# Kill THIS run on any exit, including an interrupt -- by PID, so a\n"
    "{i}# session the user started themselves is never touched. An orphaned\n"
    "{i}# run was once found alive SIXTEEN HOURS later, sharing the GPU with\n"
    "{i}# the user's own Orbiter when that one took a SIGSEGV in the driver --\n"
    "{i}# which made the crash a contaminated sample. See\n"
    "{i}# the porting notes.\n"
    "{i}trap 'kill \"$ORBPID\" 2>/dev/null' EXIT INT TERM\n"
)

# A launch block: from `setsid nohup` up to the line ending it, which is
# either `... & disown` or `... &` followed by a bare `disown` line.
LAUNCH = re.compile(
    r"^([ \t]*)setsid nohup\b.*?(?:&[ \t]*disown[ \t]*\n|&[ \t]*\n[ \t]*disown[ \t]*\n)",
    re.M | re.S)


def convert(path):
    text = open(path).read()
    if re.search(r"^\s*trap .*(EXIT|INT|TERM)", text, re.M):
        return "has a trap already"
    if "setsid nohup" not in text:
        return "does not launch"

    out, last, n = [], 0, 0
    for m in LAUNCH.finditer(text):
        block = m.group(0)
        out.append(text[last:m.end()])
        last = m.end()
        if "./Orbiter" not in block:
            continue                       # gdbrun.sh's fifo holder
        out.append(TRAP.format(i=m.group(1)))
        n += 1
    out.append(text[last:])
    if not n:
        return "no ./Orbiter launch matched"
    open(path, "w").write("".join(out))
    return "trapped %d launch(es)" % n


for name in sorted(n for n in os.listdir(HERE) if n.endswith(".sh")):
    if name == "session.sh":
        continue
    print("%-22s %s" % (name, convert(os.path.join(HERE, name))))
