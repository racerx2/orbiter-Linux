#!/usr/bin/env python3
"""Convert Orbiter.rc dialog templates into a C++ table.

On Windows, rc.exe compiles Orbiter.rc into the executable's resource section.
ELF has no equivalent, so this performs the same job at build time and emits a
C++ source file that is compiled into the binary. Orbiter.rc is read but never
modified; it remains the single source of truth for both platforms.

The .rc is first run through the C preprocessor, which is what makes this
tractable: every IDC_*/IDD_* id, LAUNCHPAD_WIN_WIDTH and the arithmetic built
on it are ordinary #defines in resource.h, and cpp resolves them exactly as the
resource compiler would. What reaches the parser below is therefore almost
entirely numeric.

Usage:
    rc2cpp.py <Orbiter.rc> <output.cpp> [-I dir]...
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile


# ---------------------------------------------------------------------------
# Preprocessing
# ---------------------------------------------------------------------------

def as_bytes_8bit(rc_path):
    """Return the .rc as 8-bit bytes, transcoding a UTF-16 script if needed.

    rc.exe reads a Unicode .rc natively and the dialog editor writes one
    whenever the script contains a character outside the code page -- so this
    is not exotic. XRSound's Resource.rc is UTF-16LE, and cpp reading it raw
    sees a NUL after every character:

        Resource.rc:5:4: error: invalid preprocessing directive #i

    -- the truncated remains of "#include". Every line failed and the module
    converted nothing.

    Returns None when the file is already 8-bit, so the common case does no
    work and the original path is handed to cpp unchanged.
    """
    with open(rc_path, "rb") as f:
        head = f.read(4)
    if head[:2] == b"\xff\xfe" and head[2:4] != b"\x00\x00":
        enc = "utf-16-le"
    elif head[:2] == b"\xfe\xff":
        enc = "utf-16-be"
    else:
        return None

    with open(rc_path, "rb") as f:
        text = f.read().decode(enc)
    if text and text[0] == "﻿":
        text = text[1:]
    # cp1252 out, because that is what the rest of this script assumes a .rc
    # is: preprocess() decodes cpp's output the same way. Characters outside
    # the code page become '?' rather than aborting the build -- the same
    # substitution rc.exe makes for a #pragma code_page it cannot represent.
    return text.encode("cp1252", errors="replace")


_INCLUDE_RE = re.compile(r'^\s*#\s*include\s+"([^"]+)"', re.MULTILINE)


def stage_unicode_sources(rc_path):
    """Transcode a UTF-16 .rc and its UTF-16 includes into a temp directory.

    Returns the directory, or None when nothing needed transcoding -- so the
    common case does no work at all and cpp is handed the original path.

    The walk follows quoted includes only. An angle-bracket include names a
    system or shim header, which is 8-bit by construction; a quoted one names
    a file beside the script, which is where a Unicode resource.h lives.
    """
    if as_bytes_8bit(rc_path) is None:
        return None

    rc_dir = os.path.dirname(os.path.abspath(rc_path))
    tmpdir = tempfile.mkdtemp(prefix="rc2cpp_")

    pending = [(rc_path, os.path.basename(rc_path))]
    seen = set()
    while pending:
        src, rel = pending.pop()
        if rel in seen:
            continue
        seen.add(rel)

        data = as_bytes_8bit(src)
        if data is None:
            continue                      # already 8-bit: leave it where it is

        dst = os.path.join(tmpdir, rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, "wb") as f:
            f.write(data)

        for inc in _INCLUDE_RE.findall(data.decode("cp1252", errors="replace")):
            cand = os.path.join(rc_dir, inc.replace("\\", "/"))
            if os.path.isfile(cand):
                pending.append((cand, inc.replace("\\", "/")))

    return tmpdir


def preprocess(rc_path, include_dirs):
    """Run the .rc through cpp with RC_INVOKED defined, as rc.exe does.

    -P suppresses line markers, which would otherwise have to be filtered out
    of the token stream. Failures are fatal rather than silently partial: a
    half-expanded template would produce a dialog with wrong coordinates, which
    is far harder to diagnose than a build error.
    """
    cmd = ["cpp", "-P", "-DRC_INVOKED", "-D_WIN32_LINUX_SHIM"]
    for d in include_dirs:
        cmd += ["-I", d]

    # A UTF-16 script is staged into a temporary directory as 8-bit text, ALONG
    # WITH ANY UTF-16 HEADERS IT INCLUDES -- the .rc is rarely Unicode on its
    # own. XRSound's Resource.rc and its resource.h are both UTF-16LE, and
    # transcoding only the .rc moved the identical error one file along:
    #
    #     resource.h:11:4: error: invalid preprocessing directive #d
    #
    # The staging directory goes FIRST on the include path, so a transcoded
    # header wins; every 8-bit header still resolves out of the .rc's own
    # directory, which the caller already passes with -I.
    tmpdir = stage_unicode_sources(rc_path)
    if tmpdir:
        cmd += ["-I", tmpdir]
        cmd.append(os.path.join(tmpdir, os.path.basename(rc_path)))
    else:
        cmd.append(rc_path)

    # Orbiter.rc declares #pragma code_page(1252) and contains cp1252 bytes in
    # its caption strings (degree signs and the like), so it is decoded as
    # cp1252 rather than UTF-8. Reading it as UTF-8 fails outright on those
    # bytes; reading it as latin-1 would silently mangle them.
    proc = subprocess.run(cmd, capture_output=True)
    if tmpdir:
        shutil.rmtree(tmpdir, ignore_errors=True)
    if proc.returncode != 0:
        sys.stderr.write("rc2cpp: preprocessing failed:\n")
        sys.stderr.write(proc.stderr.decode("cp1252", errors="replace"))
        sys.exit(1)
    return proc.stdout.decode("cp1252", errors="replace")


# ---------------------------------------------------------------------------
# Expression evaluation
# ---------------------------------------------------------------------------

# Style constants the preprocessor could not resolve are evaluated here. After
# cpp runs against the shim's windows.h nearly everything is already numeric;
# this table covers the handful of common-control styles that live in headers
# the .rc does not include.
EXTRA_CONSTANTS = {
    # Tree view
    "TVS_HASBUTTONS": 0x0001, "TVS_HASLINES": 0x0002,
    "TVS_LINESATROOT": 0x0004, "TVS_EDITLABELS": 0x0008,
    "TVS_DISABLEDRAGDROP": 0x0010, "TVS_SHOWSELALWAYS": 0x0020,
    "TVS_CHECKBOXES": 0x0100, "TVS_TRACKSELECT": 0x0200,
    "TVS_NOTOOLTIPS": 0x0080, "TVS_FULLROWSELECT": 0x1000,
    # Trackbar
    "TBS_AUTOTICKS": 0x0001, "TBS_VERT": 0x0002, "TBS_HORZ": 0x0000,
    "TBS_TOP": 0x0004, "TBS_BOTTOM": 0x0000, "TBS_LEFT": 0x0004,
    "TBS_RIGHT": 0x0000, "TBS_BOTH": 0x0008, "TBS_NOTICKS": 0x0010,
    "TBS_ENABLESELRANGE": 0x0020, "TBS_NOTHUMB": 0x0080,
    # Tab control
    "TCS_RAGGEDRIGHT": 0x0800, "TCS_BUTTONS": 0x0100,
    "TCS_MULTILINE": 0x0200, "TCS_FIXEDWIDTH": 0x0400,
    # `NOT x` in a style list clears a bit from the class default. These
    # styles are rebuilt from zero rather than masked, so the operand is
    # dropped and this entry only exists to keep the token resolvable.
    "NOT": 0,
}

# Numbers must be matched before identifiers. An identifier pattern alone
# matches "x0040L" inside "0x0040L" -- the leading 0 is a digit, so the match
# starts at the x -- and the hex literal is then mistaken for a symbol.
_TOKEN = re.compile(
    r"(?P<num>0[xX][0-9a-fA-F]+[uUlL]*|\d+[uUlL]*)"
    r"|(?P<id>[A-Za-z_][A-Za-z0-9_]*)")


class UnresolvedSymbol(Exception):
    """Raised when an identifier survived preprocessing unexpanded."""


def _eval_term(term):
    """Evaluate one OR-term: a constant, a number, or simple arithmetic."""
    def resolve(match):
        num = match.group("num")
        if num is not None:
            # Strip the C integer suffix; Python has no use for it.
            return num.rstrip("uUlL")

        name = match.group("id")
        if name in EXTRA_CONSTANTS:
            return str(EXTRA_CONSTANTS[name])
        raise UnresolvedSymbol(name)

    expr = _TOKEN.sub(resolve, term).strip()
    if not expr:
        return 0
    try:
        return int(eval(expr, {"__builtins__": {}}, {}))
    except UnresolvedSymbol:
        raise
    except Exception:
        sys.stderr.write(f"rc2cpp: cannot evaluate term '{term}'\n")
        sys.exit(1)


def split_or_terms(expr):
    """Split a style expression on '|', but only OUTSIDE parentheses.

    A bare `expr.split("|")` tears a parenthesised constant in half. That is
    not hypothetical: DS_SHELLFONT is defined as a PAIR, exactly as the SDK
    spells it --

        #define DS_SHELLFONT (DS_SETFONT | DS_FIXEDSYS)

    -- so after preprocessing a STYLE line reads `(0x0040L | 0x0008L) |
    WS_POPUP | ...` and the naive split produced the terms "(0x0040L" and
    "0x0008L)", neither of which evaluates. ToolKit.rc opens that way and
    converted nothing.
    """
    terms, cur, depth = [], "", 0
    for c in expr:
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
        if c == "|" and depth == 0:
            terms.append(cur)
            cur = ""
        else:
            cur += c
    terms.append(cur)
    return terms


def evaluate(expr):
    """Evaluate a style or coordinate expression from the .rc.

    The expression is split on '|' before anything else. That matters for the
    `NOT x` form which appears in style lists: it means "clear this bit from
    the class default", and dropping the whole term is correct here because
    these styles are rebuilt from zero rather than masked against a default.
    Removing `NOT x` by substitution instead would leave a dangling separator.

    Individual terms may still contain arithmetic -- LAUNCHPAD_WIN_WIDTH-106
    expands to 400-106 -- so each term is evaluated rather than just looked up.
    """
    expr = expr.strip()
    if not expr:
        return 0

    total = 0
    for term in split_or_terms(expr):
        term = term.strip()
        if not term:
            continue
        if re.match(r"^NOT\b", term, re.IGNORECASE):
            continue
        total |= _eval_term(term)
    return total & 0xFFFFFFFF


# ---------------------------------------------------------------------------
# Argument splitting
# ---------------------------------------------------------------------------

def split_args(text):
    """Split a control statement's arguments on commas.

    Quoted strings may contain commas, so the split is quote-aware. Escaped
    quotes inside a string ("" in .rc syntax) are handled by tracking the
    quote state rather than by a regex.
    """
    args, cur, in_str, i = [], "", False, 0
    while i < len(text):
        c = text[i]
        if c == '"':
            if in_str and i + 1 < len(text) and text[i + 1] == '"':
                cur += '""'          # doubled quote inside a string
                i += 2
                continue
            in_str = not in_str
            cur += c
        elif c == "," and not in_str:
            args.append(cur.strip())
            cur = ""
        else:
            cur += c
        i += 1
    if cur.strip():
        args.append(cur.strip())
    return args


def unquote(s):
    """Turn an .rc string literal into a C string body."""
    s = s.strip()
    if len(s) >= 2 and s[0] == '"' and s[-1] == '"':
        s = s[1:-1]
    return s.replace('""', '\\"')


def is_string(s):
    return s.strip().startswith('"')


# BEGIN/END AND { } ARE THE SAME THING, and rc.exe takes either.
#
# Visual Studio's editor writes BEGIN/END; ResEdit -- which generated
# ToolKit.rc, GenericCamera.rc, Orbits.rc and ExtMFD.rc -- writes braces. The
# parser knew only the first, so on a ResEdit script the header scan ran off
# the end of the file looking for a BEGIN that never came: TerrainToolKit
# reported "1 dialogs, 0 controls" for a file with three dialogs and eighteen
# controls, and GenericCamera's STRINGTABLE -- which carries id 1001, the
# module category the Launchpad reads -- yielded nothing at all.

def is_block_open(line):
    s = line.strip()
    return s == "{" or s.upper() == "BEGIN"


def is_block_close(line):
    s = line.strip()
    return s == "}" or s.upper() == "END"


# ---------------------------------------------------------------------------
# Control statement parsing
#
# Statement forms differ in argument order, which is why each is handled
# explicitly rather than by one generic rule. CONTROL in particular puts the
# window class and style *before* the coordinates, unlike every other form.
# ---------------------------------------------------------------------------

# statement -> (implicit class, implicit style, has leading text argument)
#
# THE BUTTON FORMS ARE NOT OPTIONAL EXTRAS. Every .rc statement rc.exe knows
# has to be here, because one it does not is silently dropped -- the control
# vanishes from the dialog and nothing says so. ToolKit.rc's Export page is
# AUTOCHECKBOX for two of its seven controls; AConfigurator.rc and
# DGConfigurator.rc are built almost entirely from CHECKBOX and
# AUTORADIOBUTTON. Those pages came up missing exactly those controls.
#
# The implicit styles are the SDK's: WS_CHILD|WS_VISIBLE = 0x50000000, plus
# WS_TABSTOP = 0x00010000 where the statement implies one, plus the class's
# own low bits (BS_*, SS_*).
SIMPLE_CONTROLS = {
    "PUSHBUTTON":    ("Button", 0x50010000, True),   # WS_CHILD|WS_VISIBLE|WS_TABSTOP
    "DEFPUSHBUTTON": ("Button", 0x50010001, True),   # ... | BS_DEFPUSHBUTTON
    "LTEXT":         ("Static", 0x50000000, True),   # SS_LEFT
    "CTEXT":         ("Static", 0x50000001, True),   # SS_CENTER
    "RTEXT":         ("Static", 0x50000002, True),   # SS_RIGHT
    "GROUPBOX":      ("Button", 0x50000007, True),   # BS_GROUPBOX
    "EDITTEXT":      ("Edit",   0x50810000, False),  # WS_BORDER|WS_TABSTOP
    "COMBOBOX":      ("ComboBox", 0x50010000, False),
    "LISTBOX":       ("ListBox", 0x50810000, False),
    # BS_CHECKBOX 0x02, BS_AUTOCHECKBOX 0x03, BS_RADIOBUTTON 0x04,
    # BS_3STATE 0x05, BS_AUTO3STATE 0x06, BS_AUTORADIOBUTTON 0x09.
    "CHECKBOX":         ("Button", 0x50010002, True),
    "AUTOCHECKBOX":     ("Button", 0x50010003, True),
    "RADIOBUTTON":      ("Button", 0x50010004, True),
    "STATE3":           ("Button", 0x50010005, True),
    "AUTO3STATE":       ("Button", 0x50010006, True),
    "AUTORADIOBUTTON":  ("Button", 0x50010009, True),
    # SS_ICON 0x03. The "text" slot names an icon resource rather than a
    # caption, which parse_control below already handles for CONTROL.
    "ICON":          ("Static", 0x50000003, True),
    "SCROLLBAR":     ("ScrollBar", 0x50000000, False),
}


def parse_control(keyword, argtext):
    """Parse one control statement into a dict, or None if unrecognised."""
    args = split_args(argtext)

    if keyword == "CONTROL":
        # CONTROL text, id, class, style, x, y, cx, cy [, exstyle]
        if len(args) < 8:
            return None
        text_arg = args[0]
        ctrl = {
            "class":   unquote(args[2]),
            "id":      evaluate(args[1]),
            "style":   evaluate(args[3]),
            "x":       evaluate(args[4]),
            "y":       evaluate(args[5]),
            "cx":      evaluate(args[6]),
            "cy":      evaluate(args[7]),
            "exstyle": evaluate(args[8]) if len(args) > 8 else 0,
        }
        # The text slot holds a bitmap id rather than a caption for
        # SS_BITMAP/BS_BITMAP controls -- e.g. CONTROL IDB_BANNER,IDC_LOGO,...
        if is_string(text_arg):
            ctrl["text"] = unquote(text_arg)
            ctrl["bitmap"] = 0
        else:
            ctrl["text"] = ""
            ctrl["bitmap"] = evaluate(text_arg)
        return ctrl

    if keyword in SIMPLE_CONTROLS:
        cls, base_style, has_text = SIMPLE_CONTROLS[keyword]
        idx = 0
        text = ""
        bitmap = 0
        if has_text:
            if not args:
                return None
            # ICON names a RESOURCE in the slot every other statement uses for
            # a caption -- `ICON IDI_WARN, IDC_STATIC, 7, 7, 20, 20` -- so the
            # same test CONTROL uses decides which it is.
            if is_string(args[0]):
                text = unquote(args[0])
            else:
                bitmap = evaluate(args[0])
            idx = 1
        # id, x, y, cx, cy [, style [, exstyle]]
        if len(args) < idx + 5:
            return None
        style = base_style
        if len(args) > idx + 5:
            style |= evaluate(args[idx + 5])
        return {
            "class":   cls,
            "text":    text,
            "id":      evaluate(args[idx]),
            "x":       evaluate(args[idx + 1]),
            "y":       evaluate(args[idx + 2]),
            "cx":      evaluate(args[idx + 3]),
            "cy":      evaluate(args[idx + 4]),
            "style":   style,
            "exstyle": evaluate(args[idx + 6]) if len(args) > idx + 6 else 0,
            "bitmap":  bitmap,
        }

    return None


# ---------------------------------------------------------------------------
# Dialog parsing
# ---------------------------------------------------------------------------

DIALOG_RE = re.compile(
    r"^\s*(\S+)\s+(DIALOGEX|DIALOG)\s+(.+)$", re.IGNORECASE)

# The MEMORY ATTRIBUTES an .rc may carry between the resource type and its
# arguments:
#
#     IDD_DGCONFIG DIALOG DISCARDABLE  0, 0, 227, 208
#
# They are 16-bit-Windows load options -- PRELOAD/LOADONCALL said when the
# segment was brought in, MOVEABLE/FIXED/DISCARDABLE how the memory manager
# could treat it -- and 32-bit rc.exe accepts and IGNORES them. Older resource
# scripts are full of them: AConfigurator.rc and DGConfigurator.rc both open
# this way, and the parser read DISCARDABLE as the first coordinate and raised
# UnresolvedSymbol on it, so neither Launchpad configuration page converted.
_MEMFLAGS_RE = re.compile(
    r"^\s*(?:PRELOAD|LOADONCALL|FIXED|MOVEABLE|DISCARDABLE|PURE|IMPURE)\b",
    re.IGNORECASE)


def strip_memflags(rest):
    """Drop any leading memory-attribute keywords, as rc.exe does."""
    while True:
        m = _MEMFLAGS_RE.match(rest)
        if not m:
            return rest.strip()
        rest = rest[m.end():]

# A file-backed resource declaration:  IDENT  TYPE  "path"
#
# BITMAP is the common case, but the same syntax carries any type. Orbiter also
# declares
#     IDT_DISCLAIMER  TEXT   "Disclaimer.txt"
#     IDR_IMAGE1      IMAGE  "bitmaps\\Splash.jpg"
# and TabAbout.cpp reads the first with
#     FindResource(NULL, MAKEINTRESOURCE(IDT_DISCLAIMER), "TEXT")
# so restricting this to BITMAP leaves the disclaimer dialog blank.
#
# ICON is here for the same reason. Orbiter declares
#     IDI_MAIN_ICON   ICON   "Orbiter.ico"
# and on Windows the window class picks it up through
#     wndClass.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_MAIN_ICON));
# in Orbiter::Create. Leaving ICON out left LoadIcon with nothing to find, so
# the title bar showed the window manager's placeholder instead of Orbiter's
# icon. The bytes are the whole .ico file, which is what rc.exe embeds too.
RESOURCE_RE = re.compile(
    r'^\s*(\S+)\s+(BITMAP|TEXT|IMAGE|RCDATA|ICON)\s+'
    r'(?:DISCARDABLE\s+)?"(.+?)"\s*$', re.IGNORECASE)


def resolve_nocase(base, relpath):
    """Resolve a Windows-style relative path case-insensitively.

    Returns the real path, or None when no component matches. Walks one
    component at a time and matches against the real directory listing, so a
    path like "bitmaps\\finger.ico" finds "Bitmaps/finger.ico".
    """
    cur = base
    parts = [p for p in relpath.replace("\\\\", "/").replace("\\", "/").split("/")
             if p not in ("", ".")]
    for part in parts:
        if not os.path.isdir(cur):
            return None
        hit = None
        for entry in os.listdir(cur):
            if entry.lower() == part.lower():
                hit = entry
                break
        if hit is None:
            return None
        cur = os.path.join(cur, hit)
    return cur if os.path.isfile(cur) else None


def parse_bitmaps(text, rc_dir):
    """Collect file-backed resources and read their contents.

    On Windows rc.exe embeds the bytes in the resource section and
    FindResource/LoadResource/LockResource hand them back. The same is done
    here: the bytes are emitted as a byte array and the shim serves a pointer
    into it, so the caller sees exactly what it sees on Windows.

    Files are read verbatim rather than converted. Several bitmaps are 4- and
    8-bit palettised, and re-encoding them would risk changing colours;
    decoding is the runtime's job, where the palette is applied once.

    A TEXT resource is stored with a trailing NUL, because callers use it
    directly as a C string -- AboutProc passes the LockResource result
    straight to SetWindowText.
    """
    bitmaps = []
    for line in text.splitlines():
        m = RESOURCE_RE.match(line)
        if not m:
            continue

        ident, restype, relpath = m.group(1), m.group(2).upper(), m.group(3)

        try:
            res_id = evaluate(ident)
        except UnresolvedSymbol as e:
            sys.stderr.write(
                f"rc2cpp: skipping bitmap '{e.args[0]}' -- id not defined\n")
            continue

        # .rc paths use backslashes; the file lives relative to the .rc.
        path = os.path.join(rc_dir, relpath.replace("\\\\", "/").replace("\\", "/"))
        if not os.path.isfile(path):
            # A .rc is written for a case-INSENSITIVE filesystem, and Orbiter's
            # is inconsistent about it: the same directory is spelled
            # "Bitmaps\\left.ico" on one line and "bitmaps\\finger.ico" on the
            # next, and only one of those exists on Linux. Resolving the path a
            # component at a time, case-insensitively, is what rc.exe
            # effectively does.
            path = resolve_nocase(rc_dir, relpath)
        if not path:
            sys.stderr.write(
                f"rc2cpp: resource file not found: {relpath} (under {rc_dir})\n")
            continue

        with open(path, "rb") as f:
            data = f.read()

        # Text resources are consumed as C strings, so they are NUL-terminated
        # here rather than relying on the caller to bound them.
        if restype == "TEXT" and not data.endswith(b"\x00"):
            data += b"\x00"

        bitmaps.append((res_id, os.path.basename(path), restype, data))

    return bitmaps


def parse_strings(text):
    """Collect every STRINGTABLE entry as (id, text).

    A STRINGTABLE is

        STRINGTABLE
        BEGIN
            IDS_ERR1   "Selected dock is already in use."
            ...
        END

    and a module can have several. On Windows rc.exe puts these in the
    module's resource section and LoadString(hInst, id, ...) reads them from
    THAT module -- which is why they are emitted as an exported symbol rather
    than registered in the shared registry the dialogs use. Ids here are small
    (ScnEditor's run 1..8) and would collide across modules immediately.

    WHAT THEIR ABSENCE COST. Only ids 1000 and 1001 were ever carried across,
    as the two named symbols the Modules tab reads. Everything else returned
    an empty string, so EditorTab_Date::TabProc's

        LoadString (ed->InstHandle(), IDS_PROP1+i, cbuf, 128);
        SendDlgItemMessage (hDlg, IDC_PROP_ORBITAL, CB_ADDSTRING, 0, cbuf);

    added four empty items and the Scenario Editor's two "Vessel state
    propagation" combo boxes came up blank. The Docking tab's four error
    messages (IDS_ERR1..4) were blank the same way.
    """
    out = []
    lines = text.splitlines()
    i = 0
    while i < len(lines):
        if lines[i].strip().upper().split(" ")[0] != "STRINGTABLE":
            i += 1
            continue
        # Skip the header lines (a STRINGTABLE may carry DISCARDABLE etc.)
        while i < len(lines) and not is_block_open(lines[i]):
            i += 1
        i += 1
        pending = ""
        while i < len(lines):
            line = lines[i].strip()
            if is_block_close(line):
                break
            i += 1
            if not line or line.startswith("//"):
                continue
            pending = (pending + " " + line).strip() if pending else line
            # An entry is `<id> "text"`. The text may carry escaped quotes
            # ("" in .rc), so only try once the quotes balance.
            if pending.count('"') % 2:
                continue
            parts = pending.split(None, 1)
            if len(parts) == 2 and is_string(parts[1].strip()):
                try:
                    out.append((evaluate(parts[0]), unquote(parts[1].strip())))
                except UnresolvedSymbol as e:
                    sys.stderr.write(
                        f"rc2cpp: skipping string '{e.args[0]}' -- id not "
                        "defined in resource.h\n")
            pending = ""
        i += 1
    return out


def parse(text):
    """Walk the preprocessed .rc and collect every dialog template."""
    lines = text.splitlines()
    dialogs = []
    i = 0

    while i < len(lines):
        m = DIALOG_RE.match(lines[i])
        if not m:
            i += 1
            continue

        ident, _kind, rest = m.group(1), m.group(2), m.group(3)

        # A dialog id that survived preprocessing is not defined in
        # resource.h, which means nothing can ever ask for this dialog: an id
        # that does not exist cannot be passed to CreateDialogParam. Orbiter.rc
        # still carries a few such templates left over from removed features.
        # Skipping them is safe, and categorically different from an
        # unresolved *style* bit, which would silently produce a control that
        # looks right and behaves wrongly -- that still fails the build.
        try:
            dlg_id = evaluate(ident)
        except UnresolvedSymbol as e:
            sys.stderr.write(
                f"rc2cpp: skipping dialog '{e.args[0]}' -- id not defined in "
                "resource.h and referenced by no source\n")
            i += 1
            continue

        coords = [evaluate(a) for a in split_args(strip_memflags(rest))]
        if len(coords) < 4:
            i += 1
            continue

        dlg = {
            "id": dlg_id,
            "x": coords[0], "y": coords[1],
            "cx": coords[2], "cy": coords[3],
            "caption": "", "style": 0, "exstyle": 0,
            "font": "MS Shell Dlg", "fontsize": 8,
            "controls": [],
        }

        # Header lines between the DIALOG line and the block opener.
        i += 1
        while i < len(lines) and not is_block_open(lines[i]):
            line = lines[i].strip()
            up = line.upper()
            if up.startswith("STYLE"):
                dlg["style"] = evaluate(line[5:])
            elif up.startswith("EXSTYLE"):
                dlg["exstyle"] = evaluate(line[7:])
            elif up.startswith("CAPTION"):
                dlg["caption"] = unquote(line[7:])
            elif up.startswith("FONT"):
                fargs = split_args(line[4:])
                if fargs:
                    try:
                        dlg["fontsize"] = int(fargs[0])
                    except ValueError:
                        pass
                if len(fargs) > 1:
                    dlg["font"] = unquote(fargs[1])
            i += 1
        i += 1   # step past BEGIN

        # Control statements, up to the matching END. Statements can wrap onto
        # a following line, so a line is only parsed once it has a keyword and
        # the accumulated text looks complete.
        pending = ""
        while i < len(lines):
            line = lines[i].strip()
            if is_block_close(line):
                break
            i += 1
            if not line or line.startswith("//"):
                continue

            pending = (pending + " " + line).strip() if pending else line

            parts = pending.split(None, 1)
            keyword = parts[0].upper()
            if keyword not in SIMPLE_CONTROLS and keyword != "CONTROL":
                pending = ""
                continue
            if len(parts) < 2:
                continue

            ctrl = parse_control(keyword, parts[1])
            if ctrl:
                dlg["controls"].append(ctrl)
                pending = ""
            # else: leave pending, the statement continues on the next line
        i += 1   # step past END

        dialogs.append(dlg)

    return dialogs


# ---------------------------------------------------------------------------
# Emission
# ---------------------------------------------------------------------------

def emit(dialogs, bitmaps, out_path, rc_path, module=None, strings=None):
    # module=None  -> the EXECUTABLE's table. Defines g_dialogTemplates and
    #                 friends, which ResourceRegistry.cpp's lookups search
    #                 first.
    # module="Name" -> a MODULE's table. Defines nothing global; registers
    #                 itself with the core at load time instead. This is the
    #                 ELF stand-in for rc.exe giving every DLL its own resource
    #                 section, which is what CreateDialogParam(hInst, ...)
    #                 searches on Windows.
    w = [
        "// Generated by Src/Orbiter/Linux/rc2cpp.py -- do not edit.",
        "//",
        f"// Source: {rc_path}",
        "//",
        "// This is the ELF equivalent of the resource section rc.exe produces",
        "// on Windows. Regenerated by the build whenever the .rc changes.",
        "",
        '#include "ResourceTemplates.h"',
        "",
        "#include <strings.h>   // strcasecmp, for the resource type match",
        "",
        "namespace orbiter_res {",
        "namespace {",
        "",
    ]

    for d in dialogs:
        if not d["controls"]:
            continue
        w.append(f"const ControlTemplate kControls_{d['id']}[] = {{")
        for c in d["controls"]:
            w.append(
                '    {{ "{cls}", "{text}", {id}, {x}, {y}, {cx}, {cy}, '
                "{style}u, {ex}u, {bmp} }},".format(
                    cls=c["class"], text=c["text"], id=c["id"],
                    x=c["x"], y=c["y"], cx=c["cx"], cy=c["cy"],
                    style=c["style"], ex=c["exstyle"], bmp=c["bitmap"]))
        w.append("};")
        w.append("")

    # Guarded for the same reason kBitmaps is below: `const DialogTemplate
    # kDialogs[] = {};` is a zero-length array, which C++ rejects. A module
    # whose .rc declares no dialog at all is ordinary once every module is
    # converted rather than only the ones that open windows -- TransX,
    # AscentMFD, MFDTemplate, DrawOrbits and GenericCamera are MFDs, and what
    # they carry is a STRINGTABLE and an icon.
    if dialogs:
        w.append("const DialogTemplate kDialogs[] = {")
    for d in d_sorted(dialogs):
        ctrls = f"kControls_{d['id']}" if d["controls"] else "nullptr"
        w.append(
            '    {{ {id}, "{cap}", {x}, {y}, {cx}, {cy}, {style}u, {ex}u, '
            '"{font}", {fsize}, {ctrls}, {n} }},'.format(
                id=d["id"], cap=d["caption"], x=d["x"], y=d["y"],
                cx=d["cx"], cy=d["cy"], style=d["style"], ex=d["exstyle"],
                font=d["font"], fsize=d["fontsize"],
                ctrls=ctrls, n=len(d["controls"])))
    if dialogs:
        w.append("};")
        w.append("")
    # Bitmap payloads. Emitted as byte arrays so the .bmp bytes travel
    # inside the executable exactly as rc.exe embeds them on Windows.
    # The array name carries the INDEX, not the resource id.
    #
    # Ids are not unique: resource.h defines IDI_FINGER2 and IDR_IMAGE1 as the
    # same number, 292. That is legal on Windows because a resource is keyed by
    # (type, id) -- FindResource(292, "ICON") and FindResource(292, "IMAGE")
    # are different objects. Naming the payload after the id therefore emitted
    # two arrays with the same name the moment ICON resources were included.
    for idx, (res_id, name, restype, data) in enumerate(bitmaps):
        w.append(f"// {name} ({restype}, id {res_id}, {len(data)} bytes)")
        w.append(f"const unsigned char kBitmapData_{idx}[] = {{")
        for off in range(0, len(data), 16):
            chunk = data[off:off + 16]
            w.append("    " + ",".join(f"0x{b:02x}" for b in chunk) + ",")
        w.append("};")
        w.append("")

    # Only when there is something to put in it: `const BitmapResource
    # kBitmaps[] = {};` is a zero-length array, which C++ rejects. Orbiter.rc
    # always has bitmaps so the executable never hit this; a module .rc that
    # declares only dialogs does.
    if bitmaps:
        w.append("const BitmapResource kBitmaps[] = {")
        for idx, (res_id, name, restype, data) in enumerate(bitmaps):
            w.append(f'    {{ {res_id}, "{name}", "{restype}", '
                     f'kBitmapData_{idx}, {len(data)} }},')
        w.append("};")
        w.append("")

    nbmp = "(int)(sizeof(kBitmaps) / sizeof(kBitmaps[0]))" if bitmaps else "0"
    bmp  = "kBitmaps" if bitmaps else "nullptr"
    ndlg = "(int)(sizeof(kDialogs) / sizeof(kDialogs[0]))" if dialogs else "0"
    dlg  = "kDialogs" if dialogs else "nullptr"

    if module:
        # A MODULE'S OWN RESOURCES, registered with the core at load time.
        #
        # On Windows rc.exe puts these in the DLL's resource section and
        # CreateDialogParam(hInst, IDD_x, ...) finds them there, per module.
        # ELF has no resource section and this shim's lookup is one table, so
        # the module hands its table over instead and the core searches it
        # after its own. Registration happens in a static constructor, i.e. at
        # dlopen, which is before InitModule and therefore before anything can
        # ask for a template.
        w.append("")
        w.append("// The core's registry. extern \"C\" so it resolves out of")
        w.append("// the executable's dynamic symbol table the same way every")
        w.append("// other orbiter_* entry point a module calls does.")
        w.append("extern \"C\" void orbiter_RegisterModuleResources(")
        w.append("    const char *name,")
        w.append("    const orbiter_res::DialogTemplate *dialogs, int dialogCount,")
        w.append("    const orbiter_res::BitmapResource *bitmaps, int bitmapCount);")
        w.append("")
        w.append("struct ModuleResourceRegistrar {")
        w.append("    ModuleResourceRegistrar() {")
        w.append(f'        orbiter_RegisterModuleResources("{module}",')
        w.append(f"            {dlg}, {ndlg},")
        w.append(f"            {bmp}, {nbmp});")
        w.append("    }")
        w.append("};")
        w.append("const ModuleResourceRegistrar kRegistrar;")
        w.append("")
        w.append("} // namespace")
        w.append("")
        w.append("} // namespace orbiter_res")
        w.append("")

        # THE STRING TABLE, as an exported symbol rather than a registration.
        #
        # LoadString takes the module's HINSTANCE and reads that module's
        # resources, so the lookup has to be keyed on the module -- and on
        # this platform the HINSTANCE is a dlopen handle, which makes
        # GetProcAddress the exact equivalent. A shared registry keyed on id
        # could not work: these ids start at 1.
        #
        # Outside namespace orbiter_res and outside the anonymous namespace:
        # it has to be an exported, unmangled symbol for GetProcAddress to
        # find it by name.
        if strings:
            w.append("// This module's STRINGTABLE. LoadString(hInst, id, ...)")
            w.append("// finds it with GetProcAddress(hInst,")
            w.append("// \"orbiterModuleStringBlob\"); see Linux/Win32Dlg.cpp")
            w.append("// and the note in Linux/ResourceTemplates.h for why it")
            w.append("// is a FLAT BLOB and not an array of structs -- a module")
            w.append("// opened LOAD_LIBRARY_AS_DATAFILE is mapped but never")
            w.append("// relocated, so any pointer stored inside it is a")
            w.append("// link-time address and crashes on use.")
            w.append("extern \"C\" const char orbiterModuleStringBlob[] =")
            # unquote() already returns a C STRING BODY -- it turns the .rc's
            # doubled "" into \" and leaves \r\n as the two-character escapes
            # the C++ compiler will interpret, which is exactly what rc.exe
            # does with them. So it is emitted raw, the same way every control
            # caption above is. Escaping it again produced literal backslashes
            # in the string: "...\\r\\n\\r\\nCreate and delete..."
            for sid, stext in strings:
                w.append(f'    "{sid}\\0" "{stext}\\0"')
            # The literal's own trailing NUL is the empty id that ends it.
            w.append('    "";')
            w.append("")
    else:
        # THE EXECUTABLE'S TABLE. The lookups themselves are NOT emitted here
        # any more -- they live in Linux/ResourceRegistry.cpp, because they now
        # have to search the modules' tables as well and a generated file is
        # the wrong place for logic that has nothing to do with this .rc.
        w.append("} // namespace")
        w.append("")
        w.append(f"const DialogTemplate *const g_dialogTemplates = {dlg};")
        w.append(f"const int g_dialogTemplateCount = {ndlg};")
        w.append("")
        w.append(f"const BitmapResource *const g_bitmaps = {bmp};")
        w.append(f"const int g_bitmapCount = {nbmp};")
        w.append("")
        w.append("} // namespace orbiter_res")
        w.append("")

    with open(out_path, "w") as f:
        f.write("\n".join(w))


def d_sorted(dialogs):
    return sorted(dialogs, key=lambda d: d["id"])


# ---------------------------------------------------------------------------

def main():
    if len(sys.argv) < 3:
        sys.stderr.write(__doc__)
        sys.exit(2)

    rc_path, out_path = sys.argv[1], sys.argv[2]
    include_dirs = []
    module = None
    args = sys.argv[3:]
    i = 0
    while i < len(args):
        if args[i] == "-I" and i + 1 < len(args):
            include_dirs.append(args[i + 1])
            i += 2
        elif args[i] == "--module" and i + 1 < len(args):
            # Emit a MODULE table that registers itself, rather than the
            # executable's global one. See emit().
            module = args[i + 1]
            i += 2
        else:
            i += 1

    text = preprocess(rc_path, include_dirs)
    dialogs = parse(text)
    # Bitmap paths in the .rc are relative to the .rc itself.
    bitmaps = parse_bitmaps(text, os.path.dirname(os.path.abspath(rc_path)))

    strings = parse_strings(text)

    # AN EMPTY CONVERSION IS A BUG FOR THE EXECUTABLE AND NORMAL FOR A MODULE.
    #
    # Orbiter.rc has 40-odd dialogs; producing none from it means the parse
    # broke, and failing the build is the only way that gets noticed. A
    # module's .rc is a different thing: TransX, AscentMFD, MFDTemplate,
    # DrawOrbits and GenericCamera are MFDs with no dialog at all, and what
    # they carry is a STRINGTABLE -- the category the Launchpad's Modules tab
    # reads -- and an icon. Refusing those would fail the build for every
    # module that simply has no window, which is why this ran for one file for
    # so long.
    #
    # Nothing at all is still worth saying, since a module that declares
    # resources and yields none of them has a broken script.
    if not dialogs and not module:
        sys.stderr.write("rc2cpp: no dialog templates found -- refusing to "
                         "emit an empty table\n")
        sys.exit(1)
    if not dialogs and not bitmaps and not strings:
        sys.stderr.write(f"rc2cpp: {rc_path} yielded no resources at all\n")

    emit(dialogs, bitmaps, out_path, rc_path, module, strings)
    total = sum(len(d["controls"]) for d in dialogs)
    bmbytes = sum(len(b[3]) for b in bitmaps)
    sys.stderr.write(
        f"rc2cpp: {'module ' + module + ': ' if module else ''}"
        f"{len(dialogs)} dialogs, {total} controls, "
        f"{len(bitmaps)} bitmaps ({bmbytes} bytes), "
        f"{len(strings)} strings -> {out_path}\n")


if __name__ == "__main__":
    main()
