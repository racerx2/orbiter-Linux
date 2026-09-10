#!/usr/bin/env python3
"""Convert a plugin's .rc STRINGTABLE into the module strings Orbiter reads.

On Windows, ModuleTab::RefreshLists identifies a plugin by loading it as a
data file and reading two strings out of its resource section:

    LoadString(hMod, 1000, buf, 1024)   -> the description shown in the pane
    LoadString(hMod, 1001, buf, 1024)   -> the category the tree groups under

Those come from a STRINGTABLE in the plugin's .rc. Meshdebug's, for example:

    STRINGTABLE
    BEGIN
        IDS_INFO  "MESH DEBUG:\\r\\n\\r\\nA small utility for developers..."
        IDS_TYPE  "Developer resources and samples"
    END

ELF has no resource section, so the Linux LoadString resolves those two ids to
exported symbols instead. This emits them. A plugin with no STRINGTABLE emits
nothing, and ModuleTab's own default of "Miscellaneous" applies -- which is
what several plugins in this tree genuinely have, having no .rc at all.
"""

import os
import re
import subprocess
import sys

# The two ids ModuleTab reads, and the symbol each becomes.
WANTED = {
    1000: "orbiterModuleDescription",
    1001: "orbiterModuleCategory",
}


def preprocess(rc_path, includes):
    """Run the .rc through the C preprocessor, as rc.exe does.

    Ids arrive as names from the plugin's resource.h; after this they are the
    integers ModuleTab asks for. RC_INVOKED is what the SDK headers test to
    skip their C declarations.
    """
    cmd = ["cpp", "-P", "-DRC_INVOKED"]
    for inc in includes:
        cmd += ["-I", inc]
    cmd.append(rc_path)

    proc = subprocess.run(cmd, capture_output=True)
    # cp1252, not UTF-8: these files carry Windows-encoded punctuation, and
    # decoding them as UTF-8 raises on the first such byte.
    return proc.stdout.decode("cp1252", errors="replace")


def parse_stringtable(text):
    """Collect id -> string from every STRINGTABLE block.

    RC accepts either BEGIN/END or braces to delimit a block, and both spellings
    appear in this tree: Meshdebug.rc uses BEGIN/END, Orbits.rc uses { }.
    """
    out = {}
    in_table = False
    for line in text.splitlines():
        s = line.strip()
        if not s:
            continue
        if s.upper().startswith("STRINGTABLE"):
            in_table = True
            continue
        if not in_table:
            continue
        if s.upper() == "BEGIN" or s == "{":
            continue
        if s.upper() == "END" or s == "}":
            in_table = False
            continue

        # An entry is: <id expression> "text"
        m = re.match(r'^(.+?)\s+"(.*)"\s*$', s)
        if not m:
            continue
        expr, value = m.group(1).strip(), m.group(2)
        try:
            ident = int(eval(expr, {"__builtins__": {}}, {}))
        except Exception:
            continue
        out[ident] = value
    return out


def c_escape(s):
    """Re-encode an .rc string literal as a C string literal.

    The two languages escape differently, and both differences appear in this
    tree:

      * RC doubles a quote to embed one -- ScnEditor.rc has
        ""Scenario editor"" -- where C uses \\".
      * A backslash that does not begin a known escape is literal in RC.
        TransX.rc has Doc\\TransX, and leaving that alone would make \\T a C
        escape sequence.

    So the RC text is decoded to real characters first, then re-encoded.
    """
    # -- decode RC --
    known = {'r': '\r', 'n': '\n', 't': '\t', 'a': '\a', '\\': '\\', '"': '"'}
    out = []
    i = 0
    while i < len(s):
        c = s[i]
        if c == '"' and i + 1 < len(s) and s[i + 1] == '"':
            out.append('"')
            i += 2
            continue
        if c == '\\' and i + 1 < len(s):
            nxt = s[i + 1]
            if nxt in known:
                out.append(known[nxt])
                i += 2
                continue
            # Not an escape RC recognises: the backslash is literal.
            out.append('\\')
            i += 1
            continue
        out.append(c)
        i += 1
    text = ''.join(out)

    # -- encode C --
    enc = {'\\': '\\\\', '"': '\\"', '\r': '\\r', '\n': '\\n', '\t': '\\t'}
    return ''.join(enc.get(ch, ch) for ch in text)


def main():
    if len(sys.argv) < 3:
        sys.stderr.write("usage: rcstrings.py <in.rc> <out.cpp> [-I dir ...]\n")
        return 2

    rc_path, out_path = sys.argv[1], sys.argv[2]
    includes = []
    args = sys.argv[3:]
    for i, a in enumerate(args):
        if a == "-I" and i + 1 < len(args):
            includes.append(args[i + 1])
    includes.append(os.path.dirname(os.path.abspath(rc_path)))

    strings = {}
    if os.path.exists(rc_path):
        strings = parse_stringtable(preprocess(rc_path, includes))

    lines = [
        "// Generated from %s -- do not edit." % os.path.basename(rc_path),
        "//",
        "// The module description and category, which ModuleTab::RefreshLists",
        "// reads with LoadString(hMod, 1000) and LoadString(hMod, 1001). On",
        "// Windows these live in the resource section; here they are exported",
        "// symbols that the Linux LoadString resolves by name.",
        "",
    ]

    found = []
    for ident, symbol in sorted(WANTED.items()):
        if ident not in strings:
            continue
        found.append(symbol)
        lines.append('extern "C" const char %s[] = "%s";'
                     % (symbol, c_escape(strings[ident])))

    if not found:
        lines.append("// No STRINGTABLE entry for 1000 or 1001 in this module;")
        lines.append('// ModuleTab\'s default category of "Miscellaneous" applies.')

    lines.append("")
    with open(out_path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))

    sys.stderr.write("rcstrings: %s -> %s (%s)\n"
                     % (os.path.basename(rc_path), os.path.basename(out_path),
                        ", ".join(found) if found else "none"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
