#!/usr/bin/env python3
"""Deterministic local safety checks for KrKr2 plugins.

This deliberately is not a C++ parser.  It masks comments and literals while
preserving source positions, pairs lexical braces, and only accepts safety
evidence visible in the same simple block as a dangerous sink.
"""

from __future__ import annotations

import argparse
import fnmatch
import json
import re
import sys
from dataclasses import asdict, dataclass
from pathlib import Path


SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp"}
SKIP_PARTS = {".git", "out", "build", "external", "third_party", "third-party"}
WAIVER_RE = re.compile(
    r"^\s*//\s*plugin-safety:\s*allow\s+(KPS\d{3})\s+--\s+(.+?)\s*$"
)
GENERIC_REASONS = {"safe here", "legacy code", "reviewed", "false positive", "ok"}
EVIDENCE_RE = re.compile(
    r"(?:\b\d+\b|<=|>=|==|capacity|sizeof|validated|checked|protocol|header\.|parent|fixed)",
    re.IGNORECASE,
)


@dataclass(frozen=True)
class Finding:
    path: str
    line: int
    column: int
    rule: str
    message: str


def mask_cpp(text: str) -> str:
    """Replace comments and literals with spaces, preserving newlines/columns."""
    out = list(text)
    i = 0
    state = "code"
    quote = ""
    while i < len(text):
        c = text[i]
        n = text[i + 1] if i + 1 < len(text) else ""
        if state == "code":
            if c == "/" and n == "/":
                out[i] = out[i + 1] = " "
                state = "line"
                i += 2
                continue
            if c == "/" and n == "*":
                out[i] = out[i + 1] = " "
                state = "block"
                i += 2
                continue
            if c in {'"', "'"}:
                quote = c
                out[i] = " "
                state = "literal"
                i += 1
                continue
        elif state == "line":
            if c == "\n":
                state = "code"
            else:
                out[i] = " "
        elif state == "block":
            if c == "*" and n == "/":
                out[i] = out[i + 1] = " "
                state = "code"
                i += 2
                continue
            if c != "\n":
                out[i] = " "
        else:
            if c == "\\" and i + 1 < len(text):
                out[i] = " "
                if text[i + 1] != "\n":
                    out[i + 1] = " "
                i += 2
                continue
            if c == quote:
                out[i] = " "
                state = "code"
            elif c != "\n":
                out[i] = " "
        i += 1
    return "".join(out)


def lexical_blocks(masked: str) -> tuple[list[int], dict[int, tuple[int, int]]]:
    """Return innermost block id per line and inclusive line ranges."""
    lines = masked.splitlines()
    stack = [0]
    starts = {0: 1}
    ranges: dict[int, tuple[int, int]] = {}
    line_blocks: list[int] = []
    parentheses: list[int] = []
    next_id = 1
    for line_no, line in enumerate(lines, 1):
        line_blocks.append(stack[-1])
        for c in line:
            if c == "{":
                starts[next_id] = line_no
                stack.append(next_id)
                next_id += 1
            elif c == "}":
                if len(stack) == 1:
                    raise ValueError(f"unmatched closing brace at line {line_no}")
                block_id = stack.pop()
                ranges[block_id] = (starts[block_id], line_no)
            elif c == "(":
                parentheses.append(line_no)
            elif c == ")":
                if not parentheses:
                    raise ValueError(f"unmatched closing parenthesis at line {line_no}")
                parentheses.pop()
    if len(stack) != 1:
        raise ValueError(f"unmatched opening brace at line {starts[stack[-1]]}")
    if parentheses:
        raise ValueError(f"unmatched opening parenthesis at line {parentheses[-1]}")
    ranges[0] = (1, max(1, len(lines)))
    return line_blocks, ranges


def column_for(line: str, pattern: str) -> int:
    match = re.search(pattern, line)
    return (match.start() + 1) if match else 1


def audit_text(text: str, display_path: str) -> list[Finding]:
    masked = mask_cpp(text)
    raw_lines = text.splitlines()
    lines = masked.splitlines()
    line_blocks, ranges = lexical_blocks(masked)
    findings: list[Finding] = []

    waivers: dict[int, tuple[str, str]] = {}
    invalid_waiver_lines: set[int] = set()
    for number, raw in enumerate(raw_lines, 1):
        if "plugin-safety:" not in raw:
            continue
        match = WAIVER_RE.match(raw)
        if not match:
            invalid_waiver_lines.add(number)
            continue
        rule, reason = match.groups()
        if reason.strip().lower() in GENERIC_REASONS or not EVIDENCE_RE.search(reason):
            invalid_waiver_lines.add(number)
        else:
            waivers[number] = (rule, reason)

    for number in sorted(invalid_waiver_lines):
        findings.append(Finding(display_path, number, 1, "KPS091",
                                "invalid or non-auditable plugin-safety waiver"))

    raw_findings: list[Finding] = []
    for index, line in enumerate(lines):
        number = index + 1
        block_start, block_end = ranges[line_blocks[index]]
        block = "\n".join(lines[block_start - 1:block_end])

        if re.search(r"\btjs_u(?:int|int32|int64|short|long)\w*\s+\w+\s*=\s*(?:\([^)]*\)\s*)?(?:param\b|\w*[Cc]ount\b|\w*[Oo]ffset\b|\w*[Ll]ength\b)", line):
            raw_findings.append(Finding(display_path, number,
                column_for(line, r"tjs_u"), "KPS101",
                "signed or externally-derived value is converted directly to an unsigned TJS type"))

        if re.search(
            r"\((?:tjs_(?:u?int64)|(?:u?int64_t)|size_t)\)\s*"
            r"\(\s*[A-Za-z_]\w*\s*[+*\-]\s*[A-Za-z_]\w*\s*\)", line):
            raw_findings.append(Finding(display_path, number,
                column_for(line, r"\((?:tjs_)?"), "KPS102",
                "arithmetic is widened only after it has already executed"))

        raw = raw_lines[index] if index < len(raw_lines) else ""
        if re.search(r'TJS_W\("mainImageBuffer(?:ForWrite)?"\)', raw) and "PropGet" in line:
            nearby = "\n".join(lines[max(0, index - 8):min(len(lines), index + 9)])
            if "LayerReadView" not in nearby and "LayerWriteView" not in nearby:
                raw_findings.append(Finding(display_path, number,
                    column_for(line, "mainImageBuffer"), "KPS103",
                    "Layer buffer access must use LayerReadView or LayerWriteView"))

        allocation = re.search(r"\b(?:new\s+[^;\[]+\[([^\]]+)\]|(?:resize|reserve)\s*\(([^)]*)\))", line)
        if allocation:
            expression = next((g for g in allocation.groups() if g is not None), "")
            identifiers = set(re.findall(r"\b[A-Za-z_]\w*\b", expression))
            direct_external = re.search(
                r"\b(?:param\s*\[|GetSize\s*\(|GetValue\s*\(|readBoundedInteger\s*\()",
                expression)
            assigned_external = any(re.search(
                rf"\b{re.escape(name)}\s*=\s*[^;]*(?:param\s*\[|GetSize\s*\(|GetValue\s*\(|\btmp\b)",
                block) for name in identifiers)
            risky = direct_external or assigned_external
            direct_token = "ValidatedAllocation" in line or ".elementCount()" in line or ".bytes()" in line
            chain_tokens = all(token in block for token in (
                "checkedElementBytes", "OperationBudget", "validateAllocationBudget"))
            checked_inputs = {
                name for name in identifiers
                if re.search(rf"checkedElementBytes\s*\([^;)]*\b{re.escape(name)}\b", block)
            }
            stable_inputs = all(
                len(re.findall(rf"\b{re.escape(name)}\s*=", block)) <= 1
                for name in checked_inputs
            )
            chain = chain_tokens and bool(checked_inputs) and stable_inputs
            if risky and not (direct_token or chain):
                raw_findings.append(Finding(display_path, number,
                    allocation.start() + 1, "KPS201",
                    "dynamic allocation from an external size lacks a visible checked allocation chain"))

        io_sink = re.search(r"\b(?:Seek|seek|Read|read)\s*\(([^;]*)\)", line)
        if io_sink and re.search(r"[+*]", io_sink.group(1)):
            if "checkedAdd" not in block and "checkedMultiply" not in block:
                raw_findings.append(Finding(display_path, number,
                    io_sink.start() + 1, "KPS202",
                    "seek/read arithmetic lacks a checked primitive in the same lexical block"))

        numeric_assignment = re.search(
            r"\b(?:time|duration|width|height|count|offset|length|size|index)\w*\s*=\s*"
            r"(?:\([^)]*\)\s*)?(?:tmp|param\s*\[|\*?param\b)", line, re.I)
        if numeric_assignment and not any(reader in block for reader in (
                "readBoundedInteger", "readFiniteReal", "readDuration")):
            raw_findings.append(Finding(display_path, number,
                numeric_assignment.start() + 1, "KPS204",
                "TJS numeric input used for a sensitive quantity lacks a bounded reader"))

        pass_call = re.search(r"PLUGIN_SMOKE_PASS|\bpluginSmokePass\s*\(", raw)
        if pass_call:
            evidence = "\n".join(raw_lines[max(0, index - 10):index + 1])
            if re.search(r"\bREADY\b|\bFrameCount\b|\bframes?\s*[><=]|\bdelay\b|stopTransition\s*\(", evidence, re.I):
                raw_findings.append(Finding(display_path, number,
                    pass_call.start() + 1, "KPS104",
                    "PASS marker is locally controlled by readiness, timing, frame count, or stopTransition"))

    # A second full budget in one lexical block defeats operation-scoped accounting.
    declarations: dict[int, list[int]] = {}
    for index, line in enumerate(lines):
        count = len(re.findall(r"\bOperationBudget\s+\w+\s*(?:[({;=])", line))
        if count:
            declarations.setdefault(line_blocks[index], []).extend([index + 1] * count)
    for numbers in declarations.values():
        for number in numbers[1:]:
            raw_findings.append(Finding(display_path, number,
                column_for(lines[number - 1], "OperationBudget"), "KPS205",
                "the same lexical operation creates more than one full OperationBudget"))

    used_waivers: set[int] = set()
    for finding in raw_findings:
        waiver_line = finding.line - 1
        waiver = waivers.get(waiver_line)
        if waiver and waiver[0] == finding.rule:
            used_waivers.add(waiver_line)
            continue
        if waiver and waiver[0] != finding.rule:
            findings.append(Finding(display_path, waiver_line, 1, "KPS092",
                                    f"waiver names {waiver[0]} but next finding is {finding.rule}"))
            used_waivers.add(waiver_line)
        findings.append(finding)

    for number, (rule, _) in waivers.items():
        if number not in used_waivers:
            findings.append(Finding(display_path, number, 1, "KPS090",
                                    f"stale waiver for {rule}; the next statement no longer matches"))
    return sorted(findings, key=lambda item: (item.line, item.column, item.rule))


def source_paths(root: Path, selected: list[str]) -> list[Path]:
    if selected:
        candidates: list[Path] = []
        for value in selected:
            path = (root / value).resolve() if not Path(value).is_absolute() else Path(value).resolve()
            if path.is_dir():
                candidates.extend(path.rglob("*"))
            else:
                candidates.append(path)
    else:
        candidates = list((root / "cpp" / "plugins").rglob("*"))
    exclusions: list[str] = []
    config_path = root / "tests" / "plugin-quality-gates.json"
    if config_path.is_file():
        config = json.loads(config_path.read_text(encoding="utf-8"))
        for item in config.get("sourceExclusions", []):
            if item.get("classification") not in {"third-party", "windows-only"}:
                raise ValueError("source exclusions must be third-party or windows-only")
            if not item.get("reason") or not item.get("path"):
                raise ValueError("source exclusion requires path and reason")
            exclusions.append(item["path"])

    result = set()
    for path in candidates:
        if not path.is_file() or path.suffix.lower() not in SOURCE_SUFFIXES:
            continue
        if any(part in SKIP_PARTS for part in path.parts):
            continue
        try:
            relative = path.resolve().relative_to(root).as_posix()
        except ValueError:
            raise ValueError(f"selected path is outside root: {path}")
        if any(fnmatch.fnmatch(relative, pattern) for pattern in exclusions):
            continue
        result.add(path)
    return sorted(result)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--paths", nargs="*", default=[])
    parser.add_argument("--format", choices=("text", "json"), default="text")
    args = parser.parse_args(argv)
    root = args.root.resolve()
    try:
        if not (root / "cpp" / "plugins").is_dir():
            raise ValueError(f"not a KrKr2 source root: {root}")
        findings: list[Finding] = []
        for path in source_paths(root, args.paths):
            try:
                relative = path.relative_to(root).as_posix()
            except ValueError:
                raise ValueError(f"selected path is outside root: {path}")
            findings.extend(audit_text(path.read_text(encoding="utf-8"), relative))
    except (OSError, UnicodeError, ValueError) as error:
        print(f"audit-plugin-safety: error: {error}", file=sys.stderr)
        return 2

    if args.format == "json":
        print(json.dumps({"findings": [asdict(item) for item in findings]},
                         ensure_ascii=False, indent=2))
    else:
        for item in findings:
            print(f"{item.path}:{item.line}:{item.column}: error: [{item.rule}] {item.message}")
        print(f"plugin-safety: {len(findings)} blocking finding(s) across "
              f"{len(source_paths(root, args.paths))} source file(s)")
    return 1 if findings else 0


if __name__ == "__main__":
    raise SystemExit(main())
