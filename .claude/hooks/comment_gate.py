#!/usr/bin/env python3
import ast
import json
import os
import re
import sys

import _hookpath

MAX_LINES = 3
C_EXTS = (".cpp", ".c", ".h", ".hpp", ".cc", ".cxx")
PY_EXTS = (".py",)
PROJECT_DIRS = ("cerf/", "ce_apps/", "launcher/", "tools/", "docs/", ".claude/")

CITATION_RE = re.compile(
    r"\bARM ARM\b|§"
    r"|\bTable\s+[A-Z]?\d|\bFig(?:ure)?\.?\s+[A-Z]?\d"
    r"|(?i:\bch\.\s*\d|\bchapter\s+\d|\bpage\s+\d|\bpg\.?\s*\d|\bp\.?\s*\d{3,})"
    r"|(?i:\b(?:ddi|ihi|den|prd|arm)\s*0*\d{3,}|\bjesd\s*\d|\brfc\s*\d)"
    r"|\b[A-Z]\d+\.\d+(?:\.\d+)+\b"
    r"|(?i:\bSDM\b|\bTRM\b|\bPRM\b|\bdatasheet\b|\buser\s+manual\b"
    r"|\breference\s+manual\b|\bvol(?:ume)?\.?\s*\d)"
    r"|(?i:\b(?:qemu|linux|netbsd|freebsd|openbsd|u-boot|coreboot)\b)"
    r"|\b[\w.-]+/[\w./-]*\.(?:c|h|cc|cpp|cxx|s|S|py)\b"
    r"|\b\w+\.(?:exe|dll)\b"
    r"|\b(?:sub|loc|off|unk|byte|word|dword|qword|stru|jpt)_[0-9A-Fa-f]{3,}\b"
    r"|0x[0-9A-Fa-f]{4,}"
)


def cited(text):
    for m in CITATION_RE.finditer(text):
        token = m.group(0)
        if "/" in token and token.lower().startswith(PROJECT_DIRS):
            continue
        return True
    return False


def c_block_comments(lines):
    blocks = []
    in_block = False
    start = None
    for idx, line in enumerate(lines, start=1):
        i, n = 0, len(line)
        in_str = None
        while i < n:
            c = line[i]
            if in_block:
                j = line.find("*/", i)
                if j < 0:
                    i = n
                    continue
                blocks.append((start[0], start[1], idx, j + 2))
                in_block = False
                i = j + 2
                continue
            if in_str:
                if c == "\\":
                    i += 2
                    continue
                if c == in_str:
                    in_str = None
                i += 1
                continue
            if c in ('"', "'"):
                in_str = c
                i += 1
                continue
            if c == "/" and i + 1 < n:
                if line[i + 1] == "/":
                    i = n
                    continue
                if line[i + 1] == "*":
                    in_block = True
                    start = (idx, i)
                    i += 2
                    continue
            i += 1
    return blocks


def whole_line_runs(lines, marker, covered, skip_shebang):
    runs = []
    start = None
    for idx, line in enumerate(lines, start=1):
        whole = line.strip().startswith(marker) and idx not in covered
        if whole and skip_shebang and idx == 1 and line.startswith("#!"):
            whole = False
        if whole:
            if start is None:
                start = idx
        elif start is not None:
            runs.append((start, idx - 1))
            start = None
    if start is not None:
        runs.append((start, len(lines)))
    return runs


def py_docstrings(src):
    found = []
    try:
        tree = ast.parse(src)
    except SyntaxError:
        return found
    holders = (ast.Module, ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)
    for node in ast.walk(tree):
        body = getattr(node, "body", None)
        if not isinstance(node, holders) or not body:
            continue
        first = body[0]
        if (isinstance(first, ast.Expr)
                and isinstance(first.value, ast.Constant)
                and isinstance(first.value.value, str)):
            sole = len(body) == 1 and not isinstance(node, ast.Module)
            found.append((first.lineno, first.end_lineno, sole))
    return found


def comment_blocks(lower, lines, src):
    spans = []
    if lower.endswith(PY_EXTS):
        covered = set()
        for start, end, sole in py_docstrings(src):
            for ln in range(start, end + 1):
                covered.add(ln)
            spans.append((start, end, "pass" if sole else None))
        for start, end in whole_line_runs(lines, "#", covered, True):
            spans.append((start, end, None))
        return spans

    covered = set()
    for sl, sc, el, ec in c_block_comments(lines):
        for ln in range(sl, el + 1):
            covered.add(ln)
        keep = (lines[sl - 1][:sc] + lines[el - 1][ec:]).rstrip()
        spans.append((sl, el, keep if keep.strip() else None))
    for start, end in whole_line_runs(lines, "//", covered, False):
        spans.append((start, end, None))
    return spans


def apply_wipe(lines, doomed, is_py):
    drop = set()
    replace = {}
    for start, end, keep in doomed:
        for ln in range(start, end + 1):
            drop.add(ln)
        if keep is None:
            continue
        if is_py:
            pad = len(lines[start - 1]) - len(lines[start - 1].lstrip())
            replace[start] = " " * pad + keep
        else:
            replace[start] = keep
    out = []
    for idx, line in enumerate(lines, start=1):
        if idx in replace:
            out.append(replace[idx])
        elif idx not in drop:
            out.append(line)
    return "\n".join(out)


def verification_text(rel_path, sample):
    return (
        f"CITATION-VERIFICATION: this {rel_path} write/edit added a "
        f"comment carrying a citation:\n\n{sample}\n\n"
        f"FALSE-POSITIVE GATE: if those lines were ALREADY in the file "
        f"and you are just moving / relocating them - not authoring a "
        f"new citation - ignore the rest of this message and proceed.\n\n"
        f"OTHERWISE - VERIFICATION REQUIRED in your NEXT MESSAGE. The "
        f"'citation written from training memory' failure mode has "
        f"shipped wrong bit fields / wrong offsets / wrong encodings "
        f"into CERF in roughly 9 of 10 incidents. The hook exists to "
        f"force the verification step before the downstream bug "
        f"lands.\n\n"
        f"YOUR NEXT MESSAGE MUST name the reference path (or path + "
        f"line) you sourced the citation from. Example shapes:\n"
        f"  - 'references/arm/DDI0406C_arm_arm.pdf page 1042 section "
        f"A8.8.384'\n"
        f"  - 'references/s3c2410/s3c2410a_um.pdf PDF page 366 printed "
        f"14-18'\n\n"
        f"THE PATH MUST BE A PERMITTED SOURCE (agent_docs/rules.md "
        f"§ Reference Licence Hygiene): datasheets, SoC user manuals, "
        f"CPU architecture manuals, standards / RFCs, QEMU, Linux, "
        f"NetBSD and other open-source projects, or the guest ROM in "
        f"IDA. The Microsoft Device Emulator source and the Microsoft "
        f"Platform Builder / Windows CE Shared Source trees, including "
        f"every BSP / PUBLIC / PRIVATE / OAK subtree under them, "
        f"are FORBIDDEN: naming one does not satisfy this "
        f"hook, it converts the comment into a licence exposure. "
        f"Re-ground the fact on a permitted source and cite that "
        f"instead - and note that deleting the citation while keeping "
        f"the code is separately forbidden, per the section below.\n\n"
        f"WHAT COUNTS AS VERIFICATION - read before you claim it. "
        f"Verification is an on-disk reference file you OPENED THIS "
        f"SESSION, named by path. These are NOT verification - they "
        f"are exactly the fabrication this hook catches, dressed "
        f"up:\n"
        f"  - 'I verified each byte / bit / offset against the "
        f"standard <USB / I2C / PCI / ...> format' - that is checking "
        f"against your MEMORY of the format. Your memory of the "
        f"format IS the training data that is wrong 9 of 10 times "
        f"(off-by-one lengths, swapped fields, wrong bit positions). "
        f"Deriving a value 'byte-by-byte' from remembered structure "
        f"is fabrication, not verification.\n"
        f"  - 'this is a universal / standard / well-known value, no "
        f"reference needed' - standard values are PRECISELY what "
        f"memory mangles. 'Standard' is not a path.\n"
        f"  - 'I know this from the spec' - if the spec is not on "
        f"disk and you did not open it this session, you do not know "
        f"it, you REMEMBER it, and remembering is the failure mode.\n"
        f"Only a path to a file you actually opened satisfies the "
        f"hook. If the reference genuinely isn't obtainable, that "
        f"does not downgrade the requirement - it means you cannot "
        f"write this code yet; say so to the user.\n\n"
        f"IF YOU CANNOT NAME A PATH, OR YOU NEVER OPENED THE "
        f"REFERENCE THIS SESSION: the code you just wrote is "
        f"fabricated from training memory. In 9 of 10 such cases the "
        f"bit-fields / offsets / encodings are MANGLED vs the real "
        f"document. Continuing on it is shipping a bug. In that "
        f"case:\n"
        f"  1. INSTANTLY REVERT the code change you just made.\n"
        f"  2. Perform end-to-end verification per "
        f"agent_docs/workflow.md and CLAUDE.md - open the reference, "
        f"find the section, paste the relevant passage into the "
        f"conversation BEFORE writing code.\n"
        f"  3. Re-author the code from the verified reference.\n"
        f"  4. AFTER verification, SHOW THE USER explicitly how much "
        f"your training-memory version differed from the official "
        f"document - list the specific bits / offsets / encodings "
        f"that were wrong.\n\n"
        f"FORBIDDEN RESPONSE - DELETING THE CITATION IS NOT A FIX, IT "
        f"IS EVIDENCE TAMPERING. When you catch a fabricated "
        f"citation, the instinct to just remove the citation line "
        f"and KEEP the code is the single worst move available, and "
        f"it is FORBIDDEN. Reasons:\n"
        f"  - The fabrication is not in the comment - it is in the "
        f"CODE the comment described. A wrong Fig/§ you invented "
        f"means the bits / offsets / encodings you wrote FROM that "
        f"invention are suspect. Deleting the citation removes the "
        f"WARNING LABEL, not the wrong data.\n"
        f"  - Unverified code with NO citation looks MORE trustworthy "
        f"than unverified code with a visibly-wrong citation. "
        f"Stripping the citation upgrades fabricated code to "
        f"clean-looking code. That is strictly worse - it hides the "
        f"evidence the next reader needs to catch the bug.\n"
        f"  - This is the same category as gaming a compliance check "
        f"or destroying evidence to pass an audit: the check said "
        f"'prove this is real', and deleting the thing it asked "
        f"about so the check goes quiet is fraud, not compliance. It "
        f"is the 'Disclosure Destruction' pattern named in "
        f"agent_docs/rules.md, applied to citations.\n"
        f"  - 'The code is correct / I verified it, so removing the "
        f"un-pointable citation just makes it self-contained' - THIS "
        f"IS THE RATIONALIZATION, not an exception. You cannot "
        f"self-certify the code is correct: that self-certification "
        f"is the training-memory judgement the hook exists to "
        f"distrust. 'My bytes aren't a bug' is exactly the belief "
        f"that ships the bug.\n\n"
        f"So: catching your own fabrication is GOOD - but the ONLY "
        f"valid response is the 4-step revert+reverify+reauthor"
        f"+disclose procedure above, applied to the CODE. Removing "
        f"the citation while leaving the code in place is the one "
        f"thing you may not do.\n\n"
        f"NEITHER PATH STOPS THE WORKFLOW. Do NOT ask the user "
        f"'should I continue?'. Verify and continue, or revert + "
        f"reverify + continue."
    )


def main() -> int:
    try:
        payload = json.loads(sys.stdin.buffer.read().decode("utf-8-sig"))
    except (json.JSONDecodeError, ValueError, UnicodeDecodeError):
        return 0

    tool_input = payload.get("tool_input") or {}
    tool_response = payload.get("tool_response") or {}
    file_path = _hookpath.normalize(
        tool_response.get("filePath") or tool_input.get("file_path"))
    if not file_path:
        return 0
    lower = file_path.lower()
    if not lower.endswith(C_EXTS + PY_EXTS):
        return 0
    if os.path.abspath(file_path) == os.path.abspath(__file__):
        return 0
    if not os.path.isfile(file_path):
        return 0

    blob = tool_input.get("content")
    if not isinstance(blob, str):
        blob = tool_input.get("new_string")
    if not isinstance(blob, str) or not blob:
        return 0
    old_blob = tool_input.get("old_string")
    old_blob = old_blob if isinstance(old_blob, str) else ""

    try:
        with open(file_path, encoding="utf-8", newline="") as fh:
            raw = fh.read()
    except OSError:
        return 0
    eol = "\r\n" if "\r\n" in raw else "\n"
    src = raw.replace("\r\n", "\n")
    lines = src.split("\n")
    is_py = lower.endswith(PY_EXTS)

    too_long = []
    no_citation = []
    verify = []
    for start, end, keep in comment_blocks(lower, lines, src):
        text = "\n".join(lines[start - 1:end])
        if text not in blob:
            continue
        if old_blob and text in old_blob:
            continue
        if end - start + 1 > MAX_LINES:
            too_long.append((start, end, keep))
        elif not cited(text):
            no_citation.append((start, end, keep))
        else:
            verify.append((start, end, text))

    try:
        rel = os.path.relpath(file_path).replace("\\", "/")
    except ValueError:
        rel = file_path.replace("\\", "/")

    doomed = sorted(too_long + no_citation)
    wiped = False
    if doomed:
        new = apply_wipe(lines, doomed, is_py)
        ok = True
        if is_py:
            try:
                ast.parse(new)
            except SyntaxError:
                ok = False
        if ok:
            try:
                with open(file_path, "w", encoding="utf-8", newline="") as fh:
                    fh.write(new.replace("\n", eol))
                wiped = True
            except OSError:
                pass

    parts = []
    if wiped:
        head = [f"WIPED {len(doomed)} comment block(s) from {rel}."]
        if too_long:
            where = ", ".join(f"{s}-{e}" for s, e, _ in too_long)
            head.append(
                f"OVER {MAX_LINES} LINES (was at {where}): a comment that "
                f"long is a mess.")
        if no_citation:
            where = ", ".join(f"{s}-{e}" for s, e, _ in no_citation)
            head.append(
                f"NO CITATION (was at {where}): nothing in it matched the "
                f"citation whitelist, so it was not a citation.")
        head.append(
            "If a wiped block was a real citation, it belongs in the "
            "transcript to the user and in the /verify subagent prompt, "
            "not in the file. If the whitelist failed to recognise a real "
            "citation shape, tell the user and suggest extending "
            ".claude/hooks/comment_gate.py.")
        parts.append("\n\n".join(head))

    if verify:
        sample = "\n".join(
            f"  {rel}:{s}: " + t.splitlines()[0].strip()[:100]
            for s, _e, t in verify[:5])
        parts.append(verification_text(rel, sample))

    if not parts:
        return 0

    if wiped and verify:
        headline = f"wiped {len(doomed)}, verify {len(verify)} in {rel}"
    elif wiped:
        headline = f"wiped {len(doomed)} comment block(s) from {rel}"
    else:
        headline = f"CITATION-VERIFICATION required in {rel}"

    out = {
        "hookSpecificOutput": {
            "hookEventName": "PostToolUse",
            "additionalContext": "\n\n".join(parts),
        },
        "systemMessage": f"[CLAUDE.md hook] {headline}",
    }
    json.dump(out, sys.stdout)
    return 0


if __name__ == "__main__":
    sys.exit(main())
