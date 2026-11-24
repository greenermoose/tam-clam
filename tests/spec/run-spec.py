#!/usr/bin/env python3
"""POSIX spec suite runner for tam-clam (Oils spec/*.test.sh style).

Usage:
    python3 tests/spec/run-spec.py build/tam-clam [tests/spec/*.test.sh ...]
    python3 tests/spec/run-spec.py --shells bash,dash build/tam-clam
"""

import argparse
import glob
import json
import os
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Tuple


@dataclass
class Expectation:
    status: Optional[str] = None          # integer string or "nonzero"
    stdout: Optional[str] = None
    stderr_contains: Optional[str] = None
    kind: str = "PASS"                    # PASS, BUG, OK, N-I


@dataclass
class SpecCase:
    file: str
    line: int
    name: str
    code: str
    default: Expectation = field(default_factory=Expectation)
    overrides: Dict[str, Expectation] = field(default_factory=dict)


def parse_spec_file(path: str) -> List[SpecCase]:
    cases: List[SpecCase] = []
    with open(path, "r", encoding="utf-8") as f:
        lines = f.readlines()

    i = 0
    n = len(lines)
    while i < n:
        line = lines[i]
        if not line.startswith("#### "):
            i += 1
            continue
        title = line[5:].strip()
        start_line = i + 1
        i += 1
        code_lines: List[str] = []
        default = Expectation()
        overrides: Dict[str, Expectation] = {}
        in_meta = False

        while i < n and not lines[i].startswith("#### "):
            cur = lines[i]
            if cur.startswith("## "):
                in_meta = True
                meta = cur[3:].rstrip("\n")
                i += 1
                # Determine target expectation (default or BUG/OK/N-I <shell>)
                target = default
                parts = meta.split(None, 2)
                if parts and parts[0] in ("BUG", "OK", "N-I") and len(parts) >= 2:
                    kind = parts[0]
                    shell = parts[1]
                    rest = parts[2] if len(parts) > 2 else ""
                    if shell not in overrides:
                        overrides[shell] = Expectation(
                            status=default.status,
                            stdout=default.stdout,
                            stderr_contains=default.stderr_contains,
                            kind=kind,
                        )
                    else:
                        overrides[shell].kind = kind
                    target = overrides[shell]
                    meta = rest
                    if kind == "N-I" and not meta:
                        continue

                if meta.startswith("status:"):
                    target.status = meta[len("status:"):].strip()
                elif meta.startswith("stdout:"):
                    val = meta[len("stdout:"):]
                    if val.startswith(" "):
                        val = val[1:]
                    target.stdout = val + "\n"
                elif meta.startswith("stdout-json:"):
                    target.stdout = json.loads(meta[len("stdout-json:"):].strip())
                elif meta.startswith("stderr-contains:"):
                    target.stderr_contains = meta[len("stderr-contains:"):].strip()
                elif meta == "STDOUT:":
                    buf: List[str] = []
                    while i < n and lines[i].rstrip("\n") != "## END":
                        buf.append(lines[i])
                        i += 1
                    if i < n and lines[i].rstrip("\n") == "## END":
                        i += 1
                    target.stdout = "".join(buf)
                else:
                    raise ValueError(f"{path}:{i}: unknown directive: {cur.strip()}")
            elif not in_meta:
                code_lines.append(cur)
                i += 1
            else:
                # Allow blank lines or comments between directives
                if cur.strip() == "" or cur.lstrip().startswith("#"):
                    i += 1
                else:
                    raise ValueError(f"{path}:{i+1}: code after metadata: {cur.strip()}")

        if default.status is None:
            default.status = "0"
        cases.append(SpecCase(
            file=os.path.basename(path),
            line=start_line,
            name=title,
            code="".join(code_lines),
            default=default,
            overrides=overrides,
        ))
    return cases


def detect_shells(tam_clam: str, shells_arg: Optional[str]) -> List[Tuple[str, List[str]]]:
    result: List[Tuple[str, List[str]]] = [("tam-clam", [os.path.abspath(tam_clam)])]
    env_shells = shells_arg if shells_arg is not None else os.environ.get("TC_SPEC_SHELLS")
    if env_shells is not None:
        if env_shells.strip() in ("", "none"):
            return result
        wanted = [s.strip() for s in env_shells.split(",") if s.strip()]
    else:
        wanted = ["dash", "bash", "mksh", "ash"]

    candidates = {
        "dash": ("dash", ["dash"]),
        "bash": ("bash", ["bash", "--posix"]),
        "mksh": ("mksh", ["mksh"]),
        "ash": ("busybox", ["busybox", "ash"]),
    }
    for name in wanted:
        if name not in candidates:
            continue
        exe, cmd = candidates[name]
        if shutil.which(exe):
            result.append((name, cmd))
    return result


def check_expectation(exp: Expectation, rc: int, out: str, err: str) -> Optional[str]:
    if exp.kind == "N-I" and exp.stdout is None and exp.status is None:
        return None
    if exp.status == "nonzero":
        if rc == 0:
            return f"status 0, want nonzero"
    elif exp.status is not None:
        want = int(exp.status)
        if rc != want:
            return f"status {rc}, want {want}"
    if exp.stdout is not None and out != exp.stdout:
        return f"stdout {out!r}, want {exp.stdout!r}"
    if exp.stderr_contains is not None and exp.stderr_contains not in err:
        return f"stderr {err!r} missing {exp.stderr_contains!r}"
    return None


def run_case(cmd: List[str], code: str, workdir: str) -> Tuple[int, str, str]:
    env = os.environ.copy()
    env["LC_ALL"] = "C"
    run_dir = os.path.join(workdir, "work")
    os.mkdir(run_dir)
    script_path = os.path.join(workdir, "_case.sh")
    with open(script_path, "w", encoding="utf-8") as f:
        f.write(code)
    try:
        proc = subprocess.run(
            cmd + [script_path],
            cwd=run_dir,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=env,
            timeout=5.0,
        )
        return (
            proc.returncode,
            proc.stdout.decode("utf-8", errors="replace"),
            proc.stderr.decode("utf-8", errors="replace"),
        )
    except subprocess.TimeoutExpired:
        return (-1, "", "TIMEOUT after 5s")


def main() -> int:
    ap = argparse.ArgumentParser(description="Run tam-clam POSIX spec suite.")
    ap.add_argument("tam_clam", help="Path to tam-clam binary")
    ap.add_argument("files", nargs="*", help="Spec .test.sh files (default: tests/spec/*.test.sh)")
    ap.add_argument("--shells", default=None, help="Comma-separated reference shells or 'none'")
    args = ap.parse_args()

    spec_dir = os.path.dirname(os.path.abspath(__file__))
    files = args.files if args.files else sorted(glob.glob(os.path.join(spec_dir, "*.test.sh")))
    if not files:
        print("run-spec: no spec files found", file=sys.stderr)
        return 1

    shells = detect_shells(args.tam_clam, args.shells)
    shell_names = [s[0] for s in shells]

    totals: Dict[str, List[int]] = {name: [0, 0, 0] for name in shell_names}  # pass, fail, known
    failures: List[str] = []

    hdr = f"{'SPEC FILE':<26}" + "".join(f"{s:>14}" for s in shell_names)
    print(hdr)
    print("-" * len(hdr))

    for path in files:
        cases = parse_spec_file(path)
        file_counts: Dict[str, List[int]] = {name: [0, 0, 0] for name in shell_names}
        for case in cases:
            for sh_name, cmd in shells:
                exp = case.overrides.get(sh_name, case.default)
                if exp.kind == "N-I":
                    file_counts[sh_name][2] += 1
                    totals[sh_name][2] += 1
                    continue
                with tempfile.TemporaryDirectory(prefix="tc-spec-") as td:
                    rc, out, err = run_case(cmd, case.code, td)
                reason = check_expectation(exp, rc, out, err)
                if reason is None:
                    if exp.kind == "BUG":
                        file_counts[sh_name][2] += 1
                        totals[sh_name][2] += 1
                    else:
                        file_counts[sh_name][0] += 1
                        totals[sh_name][0] += 1
                else:
                    file_counts[sh_name][1] += 1
                    totals[sh_name][1] += 1
                    failures.append(
                        f"FAIL [{sh_name}] {case.file}:{case.line} ({case.name}): {reason}"
                        + (f"\n    stderr: {err.strip()}" if err.strip() else "")
                    )

        row = f"{os.path.basename(path):<26}"
        for sh_name in shell_names:
            p, f_cnt, k = file_counts[sh_name]
            cell = f"{p}/{p + f_cnt + k}"
            if k:
                cell += f" ({k}k)"
            if f_cnt:
                cell += f" !{f_cnt}"
            row += f"{cell:>14}"
        print(row)

    print("-" * len(hdr))
    trow = f"{'TOTAL':<26}"
    for sh_name in shell_names:
        p, f_cnt, k = totals[sh_name]
        cell = f"{p}/{p + f_cnt + k}"
        if k:
            cell += f" ({k}k)"
        if f_cnt:
            cell += f" !{f_cnt}"
        trow += f"{cell:>14}"
    print(trow)

    if failures:
        print("\nFAILURES:")
        for msg in failures:
            print("  " + msg)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
