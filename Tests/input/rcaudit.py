#!/usr/bin/env python3
# ===========================================================================
# rcaudit.py -- what the dialog templates actually ask for.
#
# WHY THIS EXISTS.  The dialog LAYOUTS are converted: rc2cpp.py turns every
# .rc into ResourceTemplates.h, and a childdump shows the controls at exactly
# the ids, positions and styles the .rc declares.  What is NOT in the Windows
# source is the DRAWING of the stock controls -- msctls_trackbar32, Button,
# Edit, ComboBox and the rest are comctl32.dll, a Microsoft binary that this
# tree has no source for.  UIHost::drawControl is a hand-written stand-in for
# it.
#
# So the renderer has to be checked against something, and the templates ARE
# that something: the set of (class, style) combinations the tree can ever
# ask to draw is finite and written down.  Finding the gaps one screenshot at
# a time -- which is how TBS_VERT was found, after twenty-odd sliders had been
# rendering as empty boxes -- is the wrong method.  This enumerates them.
#
# PARSES BOTH FORMS, and the second one is the reason a first pass by hand
# missed TBS_VERT:
#
#     CONTROL "", IDC_X, "msctls_trackbar32", TBS_VERT | ..., 1,2,3,4
#     CONTROL "", IDC_X, TRACKBAR_CLASS,      TBS_VERT | ..., 1,2,3,4
#
# The class is a quoted string in one and an UNQUOTED MACRO in the other.
# Splitting on '"' silently drops every line of the second kind -- and the
# atmospheric dialog, the one with the broken sliders, is entirely of the
# second kind.
# ===========================================================================
import os, re, sys
from collections import defaultdict

ROOT = sys.argv[1] if len(sys.argv) > 1 else '.'

# The keyword statements are shorthand for a class plus implied styles.
KEYWORD_CLASS = {
    'PUSHBUTTON': 'Button', 'DEFPUSHBUTTON': 'Button',
    'CHECKBOX': 'Button', 'AUTOCHECKBOX': 'Button',
    'RADIOBUTTON': 'Button', 'AUTORADIOBUTTON': 'Button',
    'AUTO3STATE': 'Button', 'STATE3': 'Button', 'GROUPBOX': 'Button',
    'LTEXT': 'Static', 'RTEXT': 'Static', 'CTEXT': 'Static',
    'ICON': 'Static',
    'EDITTEXT': 'Edit',
    'COMBOBOX': 'ComboBox',
    'LISTBOX': 'ListBox',
    'SCROLLBAR': 'ScrollBar',
}

# Class macros -> the class they expand to (commctrl.h).
CLASS_MACRO = {
    'TRACKBAR_CLASS': 'msctls_trackbar32',
    'TRACKBAR_CLASSA': 'msctls_trackbar32',
    'UPDOWN_CLASS': 'msctls_updown32',
    'UPDOWN_CLASSA': 'msctls_updown32',
    'PROGRESS_CLASS': 'msctls_progress32',
    'PROGRESS_CLASSA': 'msctls_progress32',
    'WC_TREEVIEW': 'SysTreeView32',
    'WC_TABCONTROL': 'SysTabControl32',
    'WC_LISTVIEW': 'SysListView32',
    'STATUSCLASSNAME': 'msctls_statusbar32',
    'TOOLBARCLASSNAME': 'ToolbarWindow32',
}

def split_top(s):
    """Split on commas that are not inside quotes."""
    out, cur, q = [], '', False
    for ch in s:
        if ch == '"':
            q = not q
            cur += ch
        elif ch == ',' and not q:
            out.append(cur.strip()); cur = ''
        else:
            cur += ch
    out.append(cur.strip())
    return out

styles = defaultdict(set)   # class -> set of style tokens (LIVE dialogs only)
counts  = defaultdict(int)  # class -> occurrences        (LIVE dialogs only)
where   = defaultdict(set)  # class -> files
dead_styles = defaultdict(set)
dead_counts = defaultdict(int)

# (class, token) -> {dialog, ...}.  THE POINT OF THE WHOLE TOOL.  A token's
# COUNT says nothing about whether there is work to do: BS_ICON has a count
# because four dead camera/recorder templates declare it.  What decides is
# WHICH DIALOGS carry it, so record that and print it.
tok_live = defaultdict(set)
tok_dead = defaultdict(set)

records = []                # (class, style, path, dialog)
all_dialogs = set()

def note(cls, style, path, dlg, live):
    tgt_c, tgt_s = (counts, styles) if live else (dead_counts, dead_styles)
    tgt_d = tok_live if live else tok_dead
    tgt_c[cls] += 1
    if live:
        where[cls].add(os.path.relpath(path, ROOT))
    for tok in re.split(r'[|]', style):
        tok = tok.strip()
        if not tok or tok.startswith('0x') or tok.isdigit():
            continue
        if tok.startswith('NOT '):
            tok = tok[4:].strip()
        tgt_s[cls].add(tok)
        tgt_d[(cls, tok)].add(dlg)

def is_build_dir(dirpath):
    """CPack stages a full copy of the tree under build/ and out/.  Counting
    those duplicates every template and every control."""
    parts = os.path.normpath(dirpath).split(os.sep)
    return 'build' in parts or 'out' in parts or '_CPack_Packages' in parts

def strip_comments(t):
    """A dialog named only in a COMMENT is not built.  IDD_MAP was reported
    live off two comment lines in UIHost.cpp, one of which says in so many
    words that IDD_MAP is dead -- and BS_ICON went on the work list because
    of it."""
    t = re.sub(r'/\*.*?\*/', ' ', t, flags=re.S)
    t = re.sub(r'//[^\n]*', ' ', t)
    return t

for dirpath, _, files in os.walk(ROOT):
    if is_build_dir(dirpath):
        continue
    for fn in files:
        if not fn.endswith('.rc'):
            continue
        path = os.path.join(dirpath, fn)
        try:
            text = open(path, encoding='utf-8', errors='replace').read()
        except OSError:
            continue
        # Join continuation lines so a wrapped CONTROL is one record.
        text = re.sub(r',\s*\n\s*', ', ', text)
        cur_dlg = '(none)'
        for line in text.splitlines():
            line = line.strip()
            d = re.match(r'^(IDD_[A-Za-z0-9_]+)\s+DIALOG', line)
            if d:
                # KEYED BY MODULE, NOT BY NAME.  There are three IDD_MAIN --
                # the Launchpad's, and one in each TerrainToolKit -- and with
                # a bare-name key a reference to any one marks all three live.
                cur_dlg = (os.path.relpath(dirpath, ROOT), d.group(1))
                all_dialogs.add(cur_dlg)
                continue
            m = re.match(r'^CONTROL\s+(.*)$', line)
            if m:
                f = split_top(m.group(1))
                if len(f) < 4:
                    continue
                cls = f[2].strip().strip('"')
                cls = CLASS_MACRO.get(cls, cls)
                records.append((cls, f[3], path, cur_dlg))
                continue
            m = re.match(r'^([A-Z0-9]+)\s+(.*)$', line)
            if m and m.group(1) in KEYWORD_CLASS:
                kw = m.group(1)
                f = split_top(m.group(2))
                style = ''
                for fld in f[2:]:
                    if re.search(r'[A-Z]{2,}_', fld):
                        style = fld
                        break
                records.append((KEYWORD_CLASS[kw], kw + ' | ' + style, path, cur_dlg))

# ---------------------------------------------------------------------------
# LIVE vs DEAD TEMPLATE, and this is the half that was missing.
#
# A .rc declares templates; it does not say which are still BUILT.  Orbiter
# has been moving dialogs to ImGui (DlgCamera, DlgRecorder, DlgMap ... are all
# `class X : public ImGuiDialog`), and when one moves, its template stays in
# the .rc as dead weight.  Auditing declarations alone therefore invents work:
# BS_ICON, BS_BITMAP and SS_SUNKEN are used ONLY by IDD_CAM_PG_CONTROL,
# IDD_CAM_PG_PRESET, IDD_RECPLAY and IDD_GRAPHICS -- none of which any .cpp
# names any more.  Fixing the renderer for those would have been effort spent
# on controls that can never appear.
#
# The test is deliberately crude and deliberately generous: a dialog counts as
# live if its IDD_ name appears anywhere in a .cpp or .h.  That over-reports
# rather than under-reports, which is the safe direction -- a dialog wrongly
# called live only costs a look.
# ---------------------------------------------------------------------------
dlg_refs = defaultdict(set)     # dialog -> the .cpp files that name it

# Every .cpp once, comments removed, remembered by directory.
sources = []                    # (reldir, relpath, stripped text)
for dirpath, _, files in os.walk(ROOT):
    if is_build_dir(dirpath):
        continue
    for fn in files:
        # .cpp ONLY. Every IDD_ name appears in its own resource.h by
        # definition, so including headers marks all templates live and the
        # test says nothing. Code that BUILDS a dialog names it in a .cpp.
        if not fn.endswith('.cpp'):
            continue
        p = os.path.join(dirpath, fn)
        try:
            t = open(p, encoding='utf-8', errors='replace').read()
        except OSError:
            continue
        sources.append((os.path.relpath(dirpath, ROOT),
                        os.path.relpath(p, ROOT), strip_comments(t)))

for (rcdir, name) in all_dialogs:
    for srcdir, srcpath, t in sources:
        # Scope: only code in the module that OWNS the .rc counts. Without
        # this, ToolKit.cpp's IDD_MAIN vouches for the Launchpad's.
        if not (srcdir == rcdir or srcdir.startswith(rcdir + os.sep)):
            continue
        if re.search(r'\b' + re.escape(name) + r'\b', t):
            dlg_refs[(rcdir, name)].add(srcpath)

live_dlg = {d for d in all_dialogs if dlg_refs[d]}
dead_dlg = all_dialogs - live_dlg

for cls, style, path, dlg in records:
    note(cls, style, path, dlg, dlg in live_dlg or dlg == '(none)')

print("dialog templates: %d total, %d live, %d DEAD (no .cpp/.h reference)"
      % (len(all_dialogs), len(live_dlg), len(dead_dlg)))
print()
print("%-26s %6s  %s" % ("CLASS (live dialogs)", "COUNT", "STYLE TOKENS USED"))
print("-" * 110)
for cls in sorted(counts, key=lambda c: -counts[c]):
    toks = sorted(t for t in styles[cls] if not t.startswith('WS_EX_'))
    print("%-26s %6d  %s" % (cls, counts[cls], ' '.join(toks)))

print()
print("STYLE TOKENS THAT APPEAR ONLY IN DEAD TEMPLATES (nothing to implement):")
for cls in sorted(dead_styles):
    only = sorted(t for t in dead_styles[cls]
                  if t not in styles[cls] and not t.startswith('WS_EX_'))
    if only:
        print("   %-24s %s" % (cls, ' '.join(only)))

# ---------------------------------------------------------------------------
# PER-TOKEN PROVENANCE.  The table above still only proves a token exists in
# SOME live dialog; it does not say where, and "where" is the whole question
# when deciding whether to write renderer code.  `rcaudit.py ROOT BS_ICON`
# answers it: the live dialogs that carry the bit, then the dead ones.  A
# token whose live list is empty is not work.
# ---------------------------------------------------------------------------
queries = [a for a in sys.argv[2:] if not a.startswith('-')]
if queries:
    for q in queries:
        print()
        print("=== %s ===" % q)
        hits = sorted((c, t) for (c, t) in tok_live if t == q)
        if not hits:
            print("   LIVE : none")
        for c, t in hits:
            ds = sorted(d for d in tok_live[(c, t)] if d in live_dlg)
            if not ds:
                continue
            print("   LIVE : %s" % c)
            # ...and by whom.  A dialog named only by a sample or an unbuilt
            # module is not the same kind of "live" as one named by the core.
            for rcdir, name in ds:
                print("          %-20s in %s" % (name, rcdir))
                for r in sorted(dlg_refs[(rcdir, name)]):
                    print("          %-20s <- %s" % ("", r))
        for c, t in sorted((c, t) for (c, t) in tok_dead if t == q):
            print("   dead : %-24s %s" % (
                c, ' '.join(n for _, n in sorted(tok_dead[(c, t)]))))
