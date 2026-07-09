#!/usr/bin/env python3.11
"""Frontend CEC campaign driver for classic vs update-map lowering.

The driver intentionally keeps all generated RTLIL/BLIF/log artifacts outside
the repository.  The checked-in surface is this re-runnable orchestration
script; the scratchpad contains the generated Yosys scripts and command logs.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import datetime as _dt
import hashlib
import json
import os
import re
import shutil
import shlex
import subprocess
import sys
import textwrap
import time
from collections import Counter, defaultdict
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any, Iterable


REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_WORKDIR = Path(
    "/tmp/claude-206352/-scratch-phsauter-synthesis-red-arrow/"
    "1563344d-4f0e-4e22-9a7d-56e45e4727c2/scratchpad/cec_frontend"
)

YOSYS = Path("/scratch/phsauter/synthesis/yosys/yosys")
ABC = Path("/scratch/phsauter/synthesis/yosys/yosys-abc")
SLANG_PLUGIN = Path("/scratch/phsauter/synthesis/yosys-slang-dev/build/slang.so")
MANTA_DIR = Path("/scratch/phsauter/synthesis/manta")
MANTA_FLIST = (
    MANTA_DIR
    / "build/red_arrow_gf22_exp_nomux_20260703/manta_rtl_yosys.flist"
)
MANTA_TOP = "manta_hwpe_top_wrapper"
UPDATE_MAP_TEST = REPO_ROOT / "tests/various/update_map_lowering.ys"
REPORT_PATH = REPO_ROOT / "CEC_FRONTEND_REPORT.md"

VALIDATION_PAIRS = ["lowering", "loop", "nested_loop", "order", "lhsidx"]

VERDICTS = ["EQUIV", "EQUIV_MOD_X", "FAIL", "TIMEOUT", "SKIP"]


@dataclass
class CommandResult:
    cmd: list[str]
    cwd: str
    log: str
    returncode: int | None
    timeout: bool
    duration_s: float

    @property
    def command_line(self) -> str:
        return " ".join(shlex.quote(part) for part in self.cmd)


@dataclass
class Port:
    name: str
    direction: str
    width: int

    def key(self) -> tuple[str, str, int]:
        return (self.name, self.direction, self.width)


@dataclass
class CheckSpec:
    key: str
    display: str
    gold_module: str
    gate_module: str
    depth: int = 0
    subtree_cells: int = 0
    fallback: bool = False
    timeout_s: int = 1200


@dataclass
class CheckResult:
    key: str
    module: str
    gold_module: str
    gate_module: str
    verdict: str
    reason: str
    depth: int
    subtree_cells: int
    fallback: bool
    timeout_s: int
    duration_s: float
    workdir: str
    gold_ports: list[dict[str, Any]] = field(default_factory=list)
    gate_ports: list[dict[str, Any]] = field(default_factory=list)
    port_mismatch: bool = False
    abc_log: str | None = None
    xaware_log: str | None = None
    gold_prepare_log: str | None = None
    gate_prepare_log: str | None = None
    notes: list[str] = field(default_factory=list)


def utc_now() -> str:
    return _dt.datetime.now(_dt.timezone.utc).isoformat(timespec="seconds")


def ensure_assets() -> None:
    missing = [
        str(path)
        for path in [YOSYS, ABC, SLANG_PLUGIN, MANTA_FLIST, UPDATE_MAP_TEST]
        if not path.exists()
    ]
    if missing:
        raise SystemExit("Missing required asset(s):\n" + "\n".join(missing))


def shell_join(cmd: Iterable[str]) -> str:
    return " ".join(shlex.quote(str(part)) for part in cmd)


def safe_name(raw: str) -> str:
    label = raw[1:] if raw.startswith("\\") else raw
    label = re.sub(r"[^A-Za-z0-9_.-]+", "_", label).strip("._")
    if not label:
        label = "module"
    digest = hashlib.sha1(raw.encode()).hexdigest()[:10]
    return f"{label[:90]}__{digest}"


def display_name(raw: str) -> str:
    return raw[1:] if raw.startswith("\\") else raw


def rtlil_id(name: str) -> str:
    if name.startswith("\\"):
        return name
    return "\\" + name


def write_text(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)


def read_text_if_exists(path: Path) -> str:
    return path.read_text(errors="replace") if path.exists() else ""


def run_command(
    cmd: list[str], cwd: Path, timeout_s: int | float, log_path: Path
) -> CommandResult:
    start = time.monotonic()
    log_path.parent.mkdir(parents=True, exist_ok=True)
    header = f"$ cd {cwd}\n$ {shell_join(cmd)}\n\n"
    try:
        proc = subprocess.run(
            [str(part) for part in cmd],
            cwd=str(cwd),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=timeout_s,
        )
        duration = time.monotonic() - start
        body = proc.stdout or ""
        write_text(log_path, header + body)
        return CommandResult(
            cmd=[str(part) for part in cmd],
            cwd=str(cwd),
            log=str(log_path),
            returncode=proc.returncode,
            timeout=False,
            duration_s=duration,
        )
    except subprocess.TimeoutExpired as exc:
        duration = time.monotonic() - start
        body = exc.stdout or ""
        if isinstance(body, bytes):
            body = body.decode(errors="replace")
        body += f"\nTIMEOUT after {timeout_s} seconds\n"
        write_text(log_path, header + body)
        return CommandResult(
            cmd=[str(part) for part in cmd],
            cwd=str(cwd),
            log=str(log_path),
            returncode=None,
            timeout=True,
            duration_s=duration,
        )


def run_yosys_script(
    script: str, script_path: Path, cwd: Path, timeout_s: int | float, log_path: Path
) -> CommandResult:
    write_text(script_path, script)
    return run_command([str(YOSYS), "-Q", "-s", str(script_path)], cwd, timeout_s, log_path)


def require_ok(result: CommandResult, what: str) -> None:
    if result.timeout:
        raise RuntimeError(f"{what} timed out; see {result.log}")
    if result.returncode != 0:
        raise RuntimeError(f"{what} failed with rc={result.returncode}; see {result.log}")


def extract_sv_module(source: str, module_name: str) -> str:
    pattern = re.compile(
        r"(?ms)^module\s+"
        + re.escape(module_name)
        + r"\b.*?^endmodule\s*(?=\n|$)"
    )
    match = pattern.search(source)
    if not match:
        raise ValueError(f"Could not find module {module_name} in {UPDATE_MAP_TEST}")
    return match.group(0).strip() + "\n"


def build_validation_designs(workdir: Path) -> dict[str, Any]:
    val_dir = workdir / "validation"
    val_dir.mkdir(parents=True, exist_ok=True)
    source = UPDATE_MAP_TEST.read_text()

    gold_modules = [extract_sv_module(source, f"{base}_gold") for base in VALIDATION_PAIRS]
    gate_modules = [extract_sv_module(source, f"{base}_gate") for base in VALIDATION_PAIRS]

    gold_sv = "\n".join(gold_modules)
    gate_sv = "\n".join(gate_modules)
    write_text(val_dir / "good_gold.sv", gold_sv)
    write_text(val_dir / "good_gate.sv", gate_sv)

    common_prefix = f"plugin -i {SLANG_PLUGIN}\n"
    gold_rtlil = val_dir / "good_gold.rtlil"
    gate_rtlil = val_dir / "good_gate.rtlil"

    gold_script = f"""{common_prefix}
read_slang <<SV
{gold_sv}
SV
proc
opt_clean
write_rtlil -sort {gold_rtlil}
"""
    gate_script = f"""{common_prefix}
read_slang --use-update-map-lowering <<SV
{gate_sv}
SV
proc
opt_clean
write_rtlil -sort {gate_rtlil}
"""

    require_ok(
        run_yosys_script(
            gold_script,
            val_dir / "build_good_gold.ys",
            REPO_ROOT,
            300,
            val_dir / "build_good_gold.log",
        ),
        "validation gold build",
    )
    require_ok(
        run_yosys_script(
            gate_script,
            val_dir / "build_good_gate.ys",
            REPO_ROOT,
            300,
            val_dir / "build_good_gate.log",
        ),
        "validation gate build",
    )

    bug_gate = extract_sv_module(source, "lowering_gate")
    bug_gate = bug_gate.replace("module lowering_gate", "module lowering_gate_bug", 1)
    old = "z[idx_i] = data_i[0];"
    new = "z[idx_i] = ~data_i[0];"
    if old not in bug_gate:
        raise RuntimeError("Could not plant lowering_gate bug: target statement missing")
    bug_gate = bug_gate.replace(old, new, 1)
    write_text(val_dir / "bug_gate.sv", bug_gate)
    bug_rtlil = val_dir / "bug_gate.rtlil"
    bug_script = f"""{common_prefix}
read_slang --use-update-map-lowering <<SV
{bug_gate}
SV
proc
opt_clean
write_rtlil -sort {bug_rtlil}
"""
    require_ok(
        run_yosys_script(
            bug_script,
            val_dir / "build_bug_gate.ys",
            REPO_ROOT,
            300,
            val_dir / "build_bug_gate.log",
        ),
        "validation planted-bug gate build",
    )

    good_specs = [
        CheckSpec(
            key=base,
            display=base,
            gold_module=rtlil_id(f"{base}_gold"),
            gate_module=rtlil_id(f"{base}_gate"),
            timeout_s=300,
        )
        for base in VALIDATION_PAIRS
    ]
    bug_specs = [
        CheckSpec(
            key="lowering_planted_bug",
            display="lowering_planted_bug",
            gold_module=rtlil_id("lowering_gold"),
            gate_module=rtlil_id("lowering_gate_bug"),
            timeout_s=300,
        )
    ]

    return {
        "good_gold_rtlil": str(gold_rtlil),
        "good_gate_rtlil": str(gate_rtlil),
        "bug_gold_rtlil": str(gold_rtlil),
        "bug_gate_rtlil": str(bug_rtlil),
        "good_specs": good_specs,
        "bug_specs": bug_specs,
        "validation_pairs": VALIDATION_PAIRS,
    }


def parse_rtlil_modules(path: Path) -> set[str]:
    modules: set[str] = set()
    for line in path.read_text(errors="replace").splitlines():
        stripped = line.strip()
        if stripped.startswith("module "):
            parts = stripped.split()
            if len(parts) >= 2:
                modules.add(parts[1])
    return modules


def parse_rtlil_hierarchy(path: Path) -> dict[str, list[str]]:
    modules = parse_rtlil_modules(path)
    current: str | None = None
    children: dict[str, list[str]] = defaultdict(list)
    for line in path.read_text(errors="replace").splitlines():
        stripped = line.strip()
        if stripped.startswith("module "):
            parts = stripped.split()
            current = parts[1] if len(parts) >= 2 else None
            if current:
                children.setdefault(current, [])
            continue
        if stripped == "end":
            current = None
            continue
        if current and stripped.startswith("cell "):
            parts = stripped.split()
            if len(parts) >= 3 and parts[1] in modules:
                children[current].append(parts[1])
    return dict(children)


def compute_depths_and_sizes(
    children: dict[str, list[str]]
) -> tuple[dict[str, int], dict[str, int]]:
    depth_cache: dict[str, int] = {}
    size_cache: dict[str, int] = {}

    def depth(mod: str, stack: set[str]) -> int:
        if mod in depth_cache:
            return depth_cache[mod]
        if mod in stack:
            return 0
        child_mods = children.get(mod, [])
        if not child_mods:
            depth_cache[mod] = 0
        else:
            depth_cache[mod] = 1 + max(depth(child, stack | {mod}) for child in child_mods)
        return depth_cache[mod]

    def size(mod: str, stack: set[str]) -> int:
        if mod in size_cache:
            return size_cache[mod]
        if mod in stack:
            return 0
        total = 0
        for child in children.get(mod, []):
            total += 1 + size(child, stack | {mod})
        size_cache[mod] = total
        return total

    for module in children:
        depth(module, set())
        size(module, set())
    return depth_cache, size_cache


def parse_ports(path: Path) -> list[Port]:
    ports: list[Port] = []
    for line in path.read_text(errors="replace").splitlines():
        stripped = line.strip()
        if not stripped.startswith("wire "):
            continue
        tokens = stripped.split()
        direction = None
        if "input" in tokens:
            direction = "input"
        elif "output" in tokens:
            direction = "output"
        if not direction:
            continue
        width = 1
        if "width" in tokens:
            idx = tokens.index("width")
            if idx + 1 < len(tokens):
                try:
                    width = int(tokens[idx + 1])
                except ValueError:
                    width = 1
        ports.append(Port(name=tokens[-1], direction=direction, width=width))
    return ports


def port_diff(gold_ports: list[Port], gate_ports: list[Port]) -> str:
    gold = {p.key() for p in gold_ports}
    gate = {p.key() for p in gate_ports}
    only_gold = sorted(gold - gate)
    only_gate = sorted(gate - gold)
    pieces = []
    if only_gold:
        pieces.append(
            "only classic: "
            + ", ".join(f"{name}/{direction}[{width}]" for name, direction, width in only_gold[:12])
        )
        if len(only_gold) > 12:
            pieces.append(f"... plus {len(only_gold) - 12} more classic-only ports")
    if only_gate:
        pieces.append(
            "only update-map: "
            + ", ".join(f"{name}/{direction}[{width}]" for name, direction, width in only_gate[:12])
        )
        if len(only_gate) > 12:
            pieces.append(f"... plus {len(only_gate) - 12} more update-map-only ports")
    return "; ".join(pieces)


def is_abc_equiv(log_text: str, returncode: int | None) -> bool:
    lower = log_text.lower()
    if "not equivalent" in lower or "are not equivalent" in lower:
        return False
    if "equivalent" in lower and returncode == 0:
        return True
    return False


def abc_failure_reason(log_text: str, returncode: int | None) -> str:
    lower = log_text.lower()
    if "not equivalent" in lower:
        return "ABC cec found a 2-valued mismatch"
    if returncode not in (0, None):
        return f"ABC cec errored with rc={returncode}"
    return "ABC cec did not prove equivalence"


def is_sat_success(log_text: str, returncode: int | None) -> bool:
    lower = log_text.lower()
    return returncode == 0 and (
        "success" in lower
        or "no model found" in lower
        or "sat proof finished" in lower and "fail" not in lower
    )


def sat_failure_reason(log_text: str, returncode: int | None) -> str:
    lower = log_text.lower()
    if "model found" in lower or "proof did fail" in lower or "fail" in lower:
        return "x-aware SAT miter found a counterexample"
    if "can't perform" in lower or "unsupported" in lower or "unknown" in lower:
        return "x-aware SAT miter could not model the prepared circuit"
    return f"x-aware SAT miter failed with rc={returncode}"


def check_pair(
    spec: CheckSpec,
    gold_rtlil: str,
    gate_rtlil: str,
    out_root: str,
) -> dict[str, Any]:
    start = time.monotonic()
    module_dir = Path(out_root) / safe_name(spec.key)
    module_dir.mkdir(parents=True, exist_ok=True)

    gold_evert = module_dir / "gold.evert.rtlil"
    gate_evert = module_dir / "gate.evert.rtlil"
    gold_blif = module_dir / "gold.blif"
    gate_blif = module_dir / "gate.blif"

    notes: list[str] = []

    def elapsed() -> float:
        return time.monotonic() - start

    def remaining() -> float:
        return max(1.0, spec.timeout_s - elapsed())

    def finish(
        verdict: str,
        reason: str,
        *,
        port_mismatch: bool = False,
        gold_ports: list[Port] | None = None,
        gate_ports: list[Port] | None = None,
        abc_log: Path | None = None,
        xaware_log: Path | None = None,
        gold_prepare_log: Path | None = None,
        gate_prepare_log: Path | None = None,
    ) -> dict[str, Any]:
        result = CheckResult(
            key=spec.key,
            module=spec.display,
            gold_module=spec.gold_module,
            gate_module=spec.gate_module,
            verdict=verdict,
            reason=reason,
            depth=spec.depth,
            subtree_cells=spec.subtree_cells,
            fallback=spec.fallback,
            timeout_s=spec.timeout_s,
            duration_s=elapsed(),
            workdir=str(module_dir),
            gold_ports=[asdict(p) for p in (gold_ports or [])],
            gate_ports=[asdict(p) for p in (gate_ports or [])],
            port_mismatch=port_mismatch,
            abc_log=str(abc_log) if abc_log else None,
            xaware_log=str(xaware_log) if xaware_log else None,
            gold_prepare_log=str(gold_prepare_log) if gold_prepare_log else None,
            gate_prepare_log=str(gate_prepare_log) if gate_prepare_log else None,
            notes=notes,
        )
        return asdict(result)

    gold_prepare_log = module_dir / "prepare_gold.log"
    gate_prepare_log = module_dir / "prepare_gate.log"

    prepare_gold = f"""read_rtlil {gold_rtlil}
hierarchy -top {spec.gold_module}
flatten
async2sync
dffunmap
rename -enumerate -pattern ff_% t:$ff
expose -evert -evert-dff t:$dff t:$adff t:$aldff t:$ff
opt_clean
write_rtlil -sort {gold_evert}
"""
    prepare_gate = f"""read_rtlil {gate_rtlil}
hierarchy -top {spec.gate_module}
flatten
async2sync
dffunmap
rename -enumerate -pattern ff_% t:$ff
expose -evert -evert-dff t:$dff t:$adff t:$aldff t:$ff
opt_clean
write_rtlil -sort {gate_evert}
"""

    gold_prep = run_yosys_script(
        prepare_gold,
        module_dir / "prepare_gold.ys",
        REPO_ROOT,
        remaining(),
        gold_prepare_log,
    )
    if gold_prep.timeout:
        return finish(
            "TIMEOUT",
            "classic FF-evert prepare step timed out",
            gold_prepare_log=gold_prepare_log,
        )
    if gold_prep.returncode != 0:
        return finish(
            "SKIP",
            f"classic FF-evert prepare step failed with rc={gold_prep.returncode}",
            gold_prepare_log=gold_prepare_log,
        )

    gate_prep = run_yosys_script(
        prepare_gate,
        module_dir / "prepare_gate.ys",
        REPO_ROOT,
        remaining(),
        gate_prepare_log,
    )
    if gate_prep.timeout:
        return finish(
            "TIMEOUT",
            "update-map FF-evert prepare step timed out",
            gold_prepare_log=gold_prepare_log,
            gate_prepare_log=gate_prepare_log,
        )
    if gate_prep.returncode != 0:
        return finish(
            "SKIP",
            f"update-map FF-evert prepare step failed with rc={gate_prep.returncode}",
            gold_prepare_log=gold_prepare_log,
            gate_prepare_log=gate_prepare_log,
        )

    gold_ports = parse_ports(gold_evert)
    gate_ports = parse_ports(gate_evert)
    if {p.key() for p in gold_ports} != {p.key() for p in gate_ports}:
        diff = port_diff(gold_ports, gate_ports)
        write_text(
            module_dir / "port_mismatch.json",
            json.dumps(
                {
                    "gold_ports": [asdict(p) for p in gold_ports],
                    "gate_ports": [asdict(p) for p in gate_ports],
                    "diff": diff,
                },
                indent=2,
            )
            + "\n",
        )
        return finish(
            "SKIP",
            "FF-everted port set mismatch: " + diff,
            port_mismatch=True,
            gold_ports=gold_ports,
            gate_ports=gate_ports,
            gold_prepare_log=gold_prepare_log,
            gate_prepare_log=gate_prepare_log,
        )

    blif_gold_log = module_dir / "blif_gold.log"
    blif_gate_log = module_dir / "blif_gate.log"
    blif_gold = f"""read_rtlil {gold_evert}
techmap
aigmap
write_blif -top {spec.gold_module} {gold_blif}
"""
    blif_gate = f"""read_rtlil {gate_evert}
techmap
aigmap
write_blif -top {spec.gate_module} {gate_blif}
"""
    gold_blif_result = run_yosys_script(
        blif_gold,
        module_dir / "blif_gold.ys",
        REPO_ROOT,
        remaining(),
        blif_gold_log,
    )
    if gold_blif_result.timeout:
        return finish(
            "TIMEOUT",
            "classic BLIF generation timed out",
            gold_ports=gold_ports,
            gate_ports=gate_ports,
            gold_prepare_log=gold_prepare_log,
            gate_prepare_log=gate_prepare_log,
        )

    gate_blif_result = run_yosys_script(
        blif_gate,
        module_dir / "blif_gate.ys",
        REPO_ROOT,
        remaining(),
        blif_gate_log,
    )
    if gate_blif_result.timeout:
        return finish(
            "TIMEOUT",
            "update-map BLIF generation timed out",
            gold_ports=gold_ports,
            gate_ports=gate_ports,
            gold_prepare_log=gold_prepare_log,
            gate_prepare_log=gate_prepare_log,
        )

    abc_log: Path | None = None
    if gold_blif_result.returncode != 0:
        notes.append(
            f"classic BLIF generation failed with rc={gold_blif_result.returncode}; see {blif_gold_log}"
        )
    if gate_blif_result.returncode != 0:
        notes.append(
            f"update-map BLIF generation failed with rc={gate_blif_result.returncode}; see {blif_gate_log}"
        )

    if gold_blif_result.returncode == 0 and gate_blif_result.returncode == 0:
        abc_log = module_dir / "abc_cec.log"
        abc = run_command(
            [str(ABC), "-c", f"cec {gold_blif} {gate_blif}"],
            module_dir,
            remaining(),
            abc_log,
        )
        if abc.timeout:
            return finish(
                "TIMEOUT",
                "ABC cec timed out",
                gold_ports=gold_ports,
                gate_ports=gate_ports,
                abc_log=abc_log,
                gold_prepare_log=gold_prepare_log,
                gate_prepare_log=gate_prepare_log,
            )
        abc_text = read_text_if_exists(abc_log)
        if is_abc_equiv(abc_text, abc.returncode):
            return finish(
                "EQUIV",
                "ABC cec proved 2-valued equivalence",
                gold_ports=gold_ports,
                gate_ports=gate_ports,
                abc_log=abc_log,
                gold_prepare_log=gold_prepare_log,
                gate_prepare_log=gate_prepare_log,
            )

        notes.append(abc_failure_reason(abc_text, abc.returncode))
    else:
        notes.append("ABC cec skipped because BLIF generation did not complete")

    xaware_log = module_dir / "xaware_sat.log"
    xaware = f"""read_rtlil {gold_evert}
rename {spec.gold_module} gold
read_rtlil {gate_evert}
rename {spec.gate_module} gate
miter -equiv -flatten -ignore_gold_x gold gate miter
sat -verify -prove trigger 0 -enable_undef -set-def-inputs miter
"""
    sat = run_yosys_script(
        xaware,
        module_dir / "xaware_sat.ys",
        REPO_ROOT,
        remaining(),
        xaware_log,
    )
    if sat.timeout:
        return finish(
            "TIMEOUT",
            "x-aware Yosys SAT fallback timed out",
            gold_ports=gold_ports,
            gate_ports=gate_ports,
            abc_log=abc_log,
            xaware_log=xaware_log,
            gold_prepare_log=gold_prepare_log,
            gate_prepare_log=gate_prepare_log,
        )
    sat_text = read_text_if_exists(xaware_log)
    if is_sat_success(sat_text, sat.returncode):
        return finish(
            "EQUIV_MOD_X",
            "x-aware SAT miter passed after ABC/BLIF flow did not prove 2-valued equivalence",
            gold_ports=gold_ports,
            gate_ports=gate_ports,
            abc_log=abc_log,
            xaware_log=xaware_log,
            gold_prepare_log=gold_prepare_log,
            gate_prepare_log=gate_prepare_log,
        )

    return finish(
        "FAIL",
        sat_failure_reason(sat_text, sat.returncode),
        gold_ports=gold_ports,
        gate_ports=gate_ports,
        abc_log=abc_log,
        xaware_log=xaware_log,
        gold_prepare_log=gold_prepare_log,
        gate_prepare_log=gate_prepare_log,
    )


def load_jsonl(path: Path) -> list[dict[str, Any]]:
    if not path.exists():
        return []
    rows = []
    for line in path.read_text(errors="replace").splitlines():
        if line.strip():
            rows.append(json.loads(line))
    return rows


def write_json(path: Path, data: Any) -> None:
    write_text(path, json.dumps(data, indent=2, sort_keys=True) + "\n")


def run_sweep(
    *,
    label: str,
    gold_rtlil: Path,
    gate_rtlil: Path,
    specs: list[CheckSpec],
    workdir: Path,
    jobs: int,
    resume: bool = False,
) -> dict[str, Any]:
    sweep_dir = workdir / label
    modules_dir = sweep_dir / "modules"
    results_jsonl = sweep_dir / "results.jsonl"
    summary_json = sweep_dir / "summary.json"

    done_by_key: dict[str, dict[str, Any]] = {}
    if resume:
        for row in load_jsonl(results_jsonl):
            done_by_key[row["key"]] = row
    elif results_jsonl.exists():
        results_jsonl.unlink()
    if not resume and modules_dir.exists():
        shutil.rmtree(modules_dir)
    modules_dir.mkdir(parents=True, exist_ok=True)

    pending = [spec for spec in specs if spec.key not in done_by_key]
    pending.sort(key=lambda spec: (spec.depth, spec.subtree_cells, spec.display))

    started = utc_now()
    print(
        f"[{label}] checking {len(pending)} module pair(s)"
        + (f" ({len(done_by_key)} resumed)" if done_by_key else "")
        + f" with jobs={jobs}",
        flush=True,
    )

    with results_jsonl.open("a") as jsonl:
        with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
            future_to_spec = {
                pool.submit(
                    check_pair,
                    spec,
                    str(gold_rtlil),
                    str(gate_rtlil),
                    str(modules_dir),
                ): spec
                for spec in pending
            }
            for future in concurrent.futures.as_completed(future_to_spec):
                spec = future_to_spec[future]
                try:
                    result = future.result()
                except Exception as exc:  # keep the campaign moving
                    result = asdict(
                        CheckResult(
                            key=spec.key,
                            module=spec.display,
                            gold_module=spec.gold_module,
                            gate_module=spec.gate_module,
                            verdict="FAIL",
                            reason=f"harness exception: {exc}",
                            depth=spec.depth,
                            subtree_cells=spec.subtree_cells,
                            fallback=spec.fallback,
                            timeout_s=spec.timeout_s,
                            duration_s=0.0,
                            workdir=str(modules_dir / safe_name(spec.key)),
                        )
                    )
                jsonl.write(json.dumps(result, sort_keys=True) + "\n")
                jsonl.flush()
                done_by_key[result["key"]] = result
                print(
                    f"[{label}] {result['verdict']:11s} "
                    f"{result['module']} :: {result['reason']}",
                    flush=True,
                )

    results = [done_by_key[key] for key in sorted(done_by_key)]
    counts = Counter(row["verdict"] for row in results)
    summary = {
        "label": label,
        "started_utc": started,
        "finished_utc": utc_now(),
        "gold_rtlil": str(gold_rtlil),
        "gate_rtlil": str(gate_rtlil),
        "jobs": jobs,
        "results_jsonl": str(results_jsonl),
        "counts": {verdict: counts.get(verdict, 0) for verdict in VERDICTS},
        "total": len(results),
    }
    write_json(summary_json, summary)
    print(f"[{label}] counts: {summary['counts']}", flush=True)
    return {"summary": summary, "results": results}


def parse_fallback_modules(log_path: Path, known_modules: set[str]) -> dict[str, Any]:
    text = read_text_if_exists(log_path)
    lines = [line.strip() for line in text.splitlines() if "slang-update-map falling back" in line]
    known_display = {display_name(m): m for m in known_modules}
    found: set[str] = set()
    unmatched: list[str] = []
    for line in lines:
        match = re.search(r"module=([^ ,;]+)", line)
        if match:
            token = match.group(1)
            found.add(known_display.get(token, rtlil_id(token)))
            continue
        hits = [
            raw
            for disp, raw in known_display.items()
            if re.search(rf"(?<![A-Za-z0-9_$]){re.escape(disp)}(?![A-Za-z0-9_$])", line)
        ]
        if hits:
            found.update(hits)
        else:
            unmatched.append(line)
    return {
        "modules": sorted(found, key=display_name),
        "lines": lines,
        "unmatched_lines": unmatched,
    }


def elaborate_manta(workdir: Path, reuse: bool) -> dict[str, Any]:
    elab_dir = workdir / "manta" / "elaboration"
    elab_dir.mkdir(parents=True, exist_ok=True)
    classic_rtlil = elab_dir / "classic.proc_clean.rtlil"
    update_rtlil = elab_dir / "update_map.proc_clean.rtlil"

    classic_script = f"""plugin -i {SLANG_PLUGIN}
read_slang --ignore-assertions --ignore-timing --top {MANTA_TOP} -f {MANTA_FLIST} --keep-hierarchy --module-uniquify param --ff-naming signal
proc
opt_clean
write_rtlil -sort {classic_rtlil}
"""
    update_script = f"""plugin -i {SLANG_PLUGIN}
read_slang --ignore-assertions --ignore-timing --top {MANTA_TOP} -f {MANTA_FLIST} --keep-hierarchy --module-uniquify param --ff-naming signal --use-update-map-lowering
proc
opt_clean
write_rtlil -sort {update_rtlil}
"""
    if not reuse or not classic_rtlil.exists():
        result = run_yosys_script(
            classic_script,
            elab_dir / "elaborate_classic.ys",
            MANTA_DIR,
            7200,
            elab_dir / "elaborate_classic.log",
        )
        require_ok(result, "Manta classic elaboration")
    if not reuse or not update_rtlil.exists():
        result = run_yosys_script(
            update_script,
            elab_dir / "elaborate_update_map.ys",
            MANTA_DIR,
            7200,
            elab_dir / "elaborate_update_map.log",
        )
        require_ok(result, "Manta update-map elaboration")

    classic_modules = parse_rtlil_modules(classic_rtlil)
    update_modules = parse_rtlil_modules(update_rtlil)
    common = classic_modules & update_modules
    classic_only = sorted(classic_modules - update_modules, key=display_name)
    update_only = sorted(update_modules - classic_modules, key=display_name)

    children = parse_rtlil_hierarchy(classic_rtlil)
    depths, sizes = compute_depths_and_sizes(children)
    fallback = parse_fallback_modules(elab_dir / "elaborate_update_map.log", common)

    specs = [
        CheckSpec(
            key=module,
            display=display_name(module),
            gold_module=module,
            gate_module=module,
            depth=depths.get(module, 0),
            subtree_cells=sizes.get(module, 0),
            fallback=module in set(fallback["modules"]),
            timeout_s=10800 if display_name(module) == MANTA_TOP else 3600,
        )
        for module in common
    ]
    specs.sort(key=lambda spec: (spec.depth, spec.subtree_cells, spec.display))

    meta = {
        "classic_rtlil": str(classic_rtlil),
        "update_rtlil": str(update_rtlil),
        "classic_log": str(elab_dir / "elaborate_classic.log"),
        "update_log": str(elab_dir / "elaborate_update_map.log"),
        "classic_script": str(elab_dir / "elaborate_classic.ys"),
        "update_script": str(elab_dir / "elaborate_update_map.ys"),
        "common_modules": len(common),
        "classic_only": [display_name(m) for m in classic_only],
        "update_only": [display_name(m) for m in update_only],
        "fallback": fallback,
    }
    write_json(elab_dir / "elaboration_meta.json", meta)
    print(
        "[manta] elaboration ready: "
        f"{len(common)} common module(s), "
        f"{len(fallback['modules'])} parsed fallback module(s), "
        f"{len(classic_only)} classic-only, {len(update_only)} update-only",
        flush=True,
    )
    return {"meta": meta, "specs": specs}


def validation_command(args: argparse.Namespace) -> int:
    ensure_assets()
    workdir = Path(args.workdir)
    data = build_validation_designs(workdir)

    good = run_sweep(
        label="validation_good",
        gold_rtlil=Path(data["good_gold_rtlil"]),
        gate_rtlil=Path(data["good_gate_rtlil"]),
        specs=data["good_specs"],
        workdir=workdir,
        jobs=min(args.jobs, len(data["good_specs"])),
        resume=False,
    )
    bug = run_sweep(
        label="validation_bug",
        gold_rtlil=Path(data["bug_gold_rtlil"]),
        gate_rtlil=Path(data["bug_gate_rtlil"]),
        specs=data["bug_specs"],
        workdir=workdir,
        jobs=1,
        resume=False,
    )

    good_ok = all(row["verdict"] == "EQUIV" for row in good["results"])
    bug_ok = len(bug["results"]) == 1 and bug["results"][0]["verdict"] == "FAIL"
    validation_summary = {
        "started_utc": utc_now(),
        "pairs": VALIDATION_PAIRS,
        "good_counts": good["summary"]["counts"],
        "bug_counts": bug["summary"]["counts"],
        "good_ok": good_ok,
        "bug_ok": bug_ok,
        "passed": good_ok and bug_ok,
        "good_summary": workdir.joinpath("validation_good/summary.json").as_posix(),
        "bug_summary": workdir.joinpath("validation_bug/summary.json").as_posix(),
    }
    write_json(workdir / "validation" / "validation_summary.json", validation_summary)

    print("\nValidation gate output:")
    print(f"  gate 1 known-equivalent pairs: {'PASS' if good_ok else 'FAIL'}")
    for row in sorted(good["results"], key=lambda r: r["module"]):
        print(f"    {row['module']}: {row['verdict']} - {row['reason']}")
    print(f"  gate 2 planted bug: {'PASS' if bug_ok else 'FAIL'}")
    for row in bug["results"]:
        print(f"    {row['module']}: {row['verdict']} - {row['reason']}")

    return 0 if validation_summary["passed"] else 2


def manta_command(args: argparse.Namespace) -> int:
    ensure_assets()
    workdir = Path(args.workdir)
    elab = elaborate_manta(workdir, reuse=args.reuse_elab)
    results = run_sweep(
        label="manta",
        gold_rtlil=Path(elab["meta"]["classic_rtlil"]),
        gate_rtlil=Path(elab["meta"]["update_rtlil"]),
        specs=elab["specs"],
        workdir=workdir,
        jobs=args.jobs,
        resume=args.resume,
    )
    campaign = {
        "finished_utc": utc_now(),
        "workdir": str(workdir),
        "elaboration": elab["meta"],
        "manta_summary": results["summary"],
    }
    write_json(workdir / "manta" / "campaign.json", campaign)
    if args.report:
        write_report(workdir, REPORT_PATH)
    return 0


def select_spot_checks(results: list[dict[str, Any]]) -> list[dict[str, Any]]:
    checks = []
    for verdict in ["EQUIV", "EQUIV_MOD_X"]:
        match = next((row for row in results if row["verdict"] == verdict), None)
        if match:
            checks.append(match)
    checks.extend(row for row in results if row["verdict"] == "FAIL")
    return checks


def summarize_evidence(row: dict[str, Any]) -> str:
    if row["verdict"] == "EQUIV":
        text = read_text_if_exists(Path(row["abc_log"])) if row.get("abc_log") else ""
        if re.search(r"equivalent", text, re.IGNORECASE) and not re.search(
            r"not equivalent", text, re.IGNORECASE
        ):
            return "ABC log contains an equivalence proof line and no not-equivalent line."
        return "ABC log did not contain the expected proof text; inspect artifact."
    if row["verdict"] == "EQUIV_MOD_X":
        abc = read_text_if_exists(Path(row["abc_log"])) if row.get("abc_log") else ""
        sat = read_text_if_exists(Path(row["xaware_log"])) if row.get("xaware_log") else ""
        abc_note = abc_failure_reason(abc, 0)
        sat_ok = is_sat_success(sat, 0) or "SUCCESS" in sat
        return (
            f"ABC fallback reason: {abc_note}; "
            + ("x-aware SAT log contains SUCCESS." if sat_ok else "x-aware SAT proof text missing.")
        )
    if row["verdict"] == "FAIL":
        sat = read_text_if_exists(Path(row["xaware_log"])) if row.get("xaware_log") else ""
        if re.search(r"model found|proof did fail|FAIL", sat, re.IGNORECASE):
            return "x-aware SAT log contains counterexample/failure proof text."
        return "FAIL came from harness/Yosys error path; inspect artifact."
    return row["reason"]


def spotcheck_command(args: argparse.Namespace) -> int:
    workdir = Path(args.workdir)
    results = load_jsonl(workdir / "manta" / "results.jsonl")
    checks = select_spot_checks(results)
    evidence = [
        {
            "module": row["module"],
            "verdict": row["verdict"],
            "reason": row["reason"],
            "workdir": row["workdir"],
            "evidence": summarize_evidence(row),
        }
        for row in checks
    ]
    write_json(workdir / "manta" / "spot_checks.json", evidence)
    print("Spot-check evidence:")
    if not evidence:
        print("  no Manta results available")
    for item in evidence:
        print(
            f"  {item['verdict']:11s} {item['module']}: "
            f"{item['evidence']}"
        )
    if args.report:
        write_report(workdir, REPORT_PATH)
    return 0


def load_summary(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text()) if path.exists() else {}


def table_escape(text: Any) -> str:
    s = str(text)
    return s.replace("|", "\\|").replace("\n", " ")


def compact_reason(reason: str, limit: int = 150) -> str:
    reason = re.sub(r"\s+", " ", reason).strip()
    return reason if len(reason) <= limit else reason[: limit - 3] + "..."


def write_report(workdir: Path, report_path: Path) -> None:
    validation = load_summary(workdir / "validation" / "validation_summary.json")
    manta_summary = load_summary(workdir / "manta" / "summary.json")
    campaign = load_summary(workdir / "manta" / "campaign.json")
    elab_meta = campaign.get("elaboration", {})
    results = load_jsonl(workdir / "manta" / "results.jsonl")
    spot_checks = load_summary(workdir / "manta" / "spot_checks.json")

    counts = manta_summary.get("counts", {verdict: 0 for verdict in VERDICTS})
    fallback_modules = {
        display_name(module)
        for module in elab_meta.get("fallback", {}).get("modules", [])
    }
    fail_rows = [row for row in results if row["verdict"] == "FAIL"]
    skip_rows = [row for row in results if row["verdict"] == "SKIP"]
    timeout_rows = [row for row in results if row["verdict"] == "TIMEOUT"]
    mismatch_rows = [row for row in results if row.get("port_mismatch")]

    def cmd_block(path_key: str) -> str:
        path = elab_meta.get(path_key)
        return f"- `{path}`" if path else "- not generated"

    lines: list[str] = []
    lines.append("# CEC Frontend Report")
    lines.append("")
    lines.append(f"- Generated: {utc_now()}")
    lines.append(f"- Repository: `{REPO_ROOT}`")
    lines.append(f"- Scratch workdir: `{workdir}`")
    lines.append(f"- Yosys: `{YOSYS}`")
    lines.append(f"- ABC: `{ABC}`")
    lines.append(f"- slang plugin: `{SLANG_PLUGIN}`")
    lines.append(f"- Manta flist: `{MANTA_FLIST}`")
    lines.append(f"- Manta top: `{MANTA_TOP}`")
    lines.append("")

    lines.append("## Validation Gates")
    lines.append("")
    if validation:
        lines.append(
            f"- Gate 1, known-equivalent pairs from `tests/various/update_map_lowering.ys`: "
            f"{'PASS' if validation.get('good_ok') else 'FAIL'}"
        )
        lines.append(f"  - Pairs: `{', '.join(validation.get('pairs', []))}`")
        lines.append(f"  - Counts: `{validation.get('good_counts')}`")
        lines.append(
            f"- Gate 2, planted bug in `lowering_gate_bug`: "
            f"{'PASS' if validation.get('bug_ok') else 'FAIL'}"
        )
        lines.append(f"  - Counts: `{validation.get('bug_counts')}`")
    else:
        lines.append("- Validation summary not found.")
    lines.append("")

    lines.append("## Exact Commands and Recipes")
    lines.append("")
    lines.append("Driver commands used:")
    lines.append("")
    lines.append(
        "```sh\n"
        f"python3.11 tests/formal_cec/run_frontend_cec.py --workdir {workdir} validate --jobs 8\n"
        f"python3.11 tests/formal_cec/run_frontend_cec.py --workdir {workdir} manta --jobs 8 --report\n"
        f"python3.11 tests/formal_cec/run_frontend_cec.py --workdir {workdir} spotcheck --report\n"
        "```"
    )
    lines.append("")
    lines.append("Manta elaboration scripts generated and run from `/scratch/phsauter/synthesis/manta`:")
    lines.append(cmd_block("classic_script"))
    lines.append(cmd_block("update_script"))
    lines.append("")
    lines.append("Elaboration recipe:")
    lines.append("")
    lines.append("```yosys")
    lines.append(f"plugin -i {SLANG_PLUGIN}")
    lines.append(
        f"read_slang --ignore-assertions --ignore-timing --top {MANTA_TOP} "
        f"-f {MANTA_FLIST} --keep-hierarchy --module-uniquify param "
        "--ff-naming signal [--use-update-map-lowering]"
    )
    lines.append("proc")
    lines.append("opt_clean")
    lines.append("write_rtlil -sort <side>.rtlil")
    lines.append("```")
    lines.append("")
    lines.append("Per-module recipe:")
    lines.append("")
    lines.append("```yosys")
    lines.append("read_rtlil <side>.rtlil")
    lines.append("hierarchy -top <module>")
    lines.append("flatten")
    lines.append("async2sync")
    lines.append("dffunmap")
    lines.append("rename -enumerate -pattern ff_% t:$ff")
    lines.append("expose -evert -evert-dff t:$dff t:$adff t:$aldff t:$ff")
    lines.append("opt_clean")
    lines.append("write_rtlil -sort <module>.evert.rtlil")
    lines.append("techmap")
    lines.append("aigmap")
    lines.append("write_blif -top <module> <module>.blif")
    lines.append("```")
    lines.append("")
    lines.append("ABC and x-aware fallback commands:")
    lines.append("")
    lines.append("```sh")
    lines.append("yosys-abc -c 'cec gold.blif gate.blif'")
    lines.append("```")
    lines.append("")
    lines.append("```yosys")
    lines.append("read_rtlil gold.evert.rtlil")
    lines.append("rename <gold_module> gold")
    lines.append("read_rtlil gate.evert.rtlil")
    lines.append("rename <gate_module> gate")
    lines.append("miter -equiv -flatten -ignore_gold_x gold gate miter")
    lines.append("sat -verify -prove trigger 0 -enable_undef -set-def-inputs miter")
    lines.append("```")
    lines.append("")

    lines.append("## Verdict Counts")
    lines.append("")
    lines.append("| Verdict | Count |")
    lines.append("|---|---:|")
    for verdict in VERDICTS:
        lines.append(f"| {verdict} | {counts.get(verdict, 0)} |")
    lines.append(f"| Total | {manta_summary.get('total', len(results))} |")
    lines.append("")

    no_space_fails = [
        row["module"]
        for row in fail_rows
        if "No space left on device" in row.get("reason", "")
    ]
    if manta_summary.get("note") or no_space_fails:
        lines.append("## Run Notes")
        lines.append("")
        if manta_summary.get("note"):
            lines.append(f"- {manta_summary['note']}.")
        if no_space_fails:
            lines.append(
                "- The `No space left on device` FAIL rows are infrastructure failures, "
                "not SAT counterexamples: "
                + ", ".join(f"`{name}`" for name in no_space_fails)
                + "."
            )
        lines.append("")

    lines.append("## Fallback Cross-Reference")
    lines.append("")
    fallback_count = len(fallback_modules)
    lines.append(
        f"Parsed `{fallback_count}` module(s) from update-map fallback log lines. "
        "These rows are marked `yes` in the verdict table."
    )
    unmatched = elab_meta.get("fallback", {}).get("unmatched_lines", [])
    if unmatched:
        lines.append(f"Unmatched fallback log lines: `{len(unmatched)}`.")
    lines.append("")

    lines.append("## Failures, Timeouts, and Skips")
    lines.append("")
    if fail_rows:
        lines.append("FAIL modules:")
        for row in fail_rows:
            lines.append(f"- `{row['module']}`: {compact_reason(row['reason'])}")
    else:
        lines.append("- FAIL modules: none")
    if timeout_rows:
        lines.append("TIMEOUT modules:")
        for row in timeout_rows:
            lines.append(f"- `{row['module']}`: {compact_reason(row['reason'])}")
    else:
        lines.append("- TIMEOUT modules: none")
    if skip_rows:
        lines.append("SKIP modules:")
        for row in skip_rows:
            lines.append(f"- `{row['module']}`: {compact_reason(row['reason'])}")
    else:
        lines.append("- SKIP modules: none")
    lines.append("")

    lines.append("## FF Port Matching")
    lines.append("")
    if mismatch_rows:
        lines.append("The following modules had mismatched FF-everted port sets and were not silently checked:")
        for row in mismatch_rows:
            lines.append(f"- `{row['module']}`: {compact_reason(row['reason'], 240)}")
    else:
        lines.append("No FF-everted port-set mismatches were recorded.")
    lines.append("")

    lines.append("## Spot Checks")
    lines.append("")
    if spot_checks:
        for item in spot_checks:
            lines.append(
                f"- `{item['module']}` ({item['verdict']}): {item['evidence']} "
                f"Artifact: `{item['workdir']}`"
            )
    else:
        lines.append("- Spot-check summary not found.")
    lines.append("")

    lines.append("## Per-Module Verdicts")
    lines.append("")
    lines.append("| Module | Depth | Subtree Cells | Fallback | Verdict | Reason | Artifact |")
    lines.append("|---|---:|---:|:---:|---|---|---|")
    for row in sorted(results, key=lambda r: (r["depth"], r["subtree_cells"], r["module"])):
        fallback = "yes" if row.get("fallback") else ""
        lines.append(
            "| "
            + " | ".join(
                [
                    f"`{table_escape(row['module'])}`",
                    str(row["depth"]),
                    str(row["subtree_cells"]),
                    fallback,
                    row["verdict"],
                    table_escape(compact_reason(row["reason"])),
                    f"`{table_escape(row['workdir'])}`",
                ]
            )
            + " |"
        )
    lines.append("")

    lines.append("## Created Files")
    lines.append("")
    lines.append("- `tests/formal_cec/run_frontend_cec.py`")
    lines.append("- `CEC_FRONTEND_REPORT.md`")
    lines.append(f"- Scratch artifacts under `{workdir}`")
    lines.append("")

    lines.append("## Assumptions and Soundness Holes")
    lines.append("")
    lines.append("- Classic lowering is always treated as gold; update-map lowering is gate.")
    lines.append("- The elaboration sides are compared after exactly `proc; opt_clean`; no other pre-CEC optimization is applied before the saved RTLIL.")
    lines.append("- Per-module checks flatten each module's full subtree, so the top-level check covers the whole chip and smaller modules localize issues.")
    lines.append("- The FF cut uses `rename -enumerate -pattern ff_% t:$ff` then `expose -evert -evert-dff t:$dff t:$adff t:$aldff t:$ff`; in this Yosys build, `-evert-dff` alone creates boundary ports but leaves FF cells behind, and latch-derived `$ff` cells are private until renamed.")
    lines.append("- The harness compares the complete FF-everted port set by name, direction, and width before invoking ABC or SAT.")
    lines.append("- Modules that cannot be prepared into FF-everted BLIF are reported as SKIP; they are not treated as silently equivalent.")
    lines.append("- ABC is a 2-valued check. When ABC fails or errors after successful FF-everted preparation, the harness escalates to `miter -ignore_gold_x` plus `sat -enable_undef` and reports `EQUIV_MOD_X` only on that proof.")
    lines.append("- Fallback log parsing is best-effort when the log line does not contain `module=<name>`; unmatched raw lines are counted above if present.")
    if manta_summary.get("note"):
        lines.append("- The final `summary.json`/`campaign.json` were reconstructed from the complete `results.jsonl` after `/tmp` filled during driver finalization.")
    if elab_meta.get("classic_only") or elab_meta.get("update_only"):
        lines.append(
            f"- Module set mismatch: classic-only `{elab_meta.get('classic_only')}`, "
            f"update-only `{elab_meta.get('update_only')}`."
        )
    else:
        lines.append("- No classic-only or update-only modules were reported by the elaboration RTLIL parser.")
    lines.append("")

    write_text(report_path, "\n".join(lines) + "\n")
    print(f"Wrote report: {report_path}")


def report_command(args: argparse.Namespace) -> int:
    write_report(Path(args.workdir), REPORT_PATH)
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--workdir", default=str(DEFAULT_WORKDIR))
    sub = parser.add_subparsers(dest="command", required=True)

    validate = sub.add_parser("validate", help="run both mandatory validation gates")
    validate.add_argument("--jobs", type=int, default=8)
    validate.set_defaults(func=validation_command)

    manta = sub.add_parser("manta", help="elaborate and sweep the full Manta design")
    manta.add_argument("--jobs", type=int, default=8)
    manta.add_argument("--reuse-elab", action="store_true")
    manta.add_argument("--resume", action="store_true")
    manta.add_argument("--report", action="store_true")
    manta.set_defaults(func=manta_command)

    spot = sub.add_parser("spotcheck", help="record log-evidence spot checks")
    spot.add_argument("--report", action="store_true")
    spot.set_defaults(func=spotcheck_command)

    report = sub.add_parser("report", help="regenerate the markdown report")
    report.set_defaults(func=report_command)

    args = parser.parse_args(argv)
    if getattr(args, "jobs", 1) < 1:
        parser.error("--jobs must be >= 1")
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
