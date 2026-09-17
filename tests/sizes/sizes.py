#!/usr/bin/env python3
"""Deterministic contract-size tables; no LocalNet and no retained raw outputs.

Run --write before committing a compiler change, then --check to verify it.
The default regression corpus uses ARC4/named/legacy defaults; chainwide uses
the four tracked replay inputs and their EVM ABI/storage profiles. Optional
chainwide-local includes downloaded, untracked inputs and cannot write a
canonical baseline. Instruction counts are static TEAL lines, not runtime cost.
"""
from __future__ import annotations

import argparse
import concurrent.futures
import contextlib
import fcntl
import hashlib
import json
import os
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from dataclasses import asdict, dataclass
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
REGRESSION_DIR = ROOT / "tests/solidity-semantic-tests/tests/puyasolRegression/contracts"
CHAINWIDE_DIR = ROOT / "tests/chainwide-historical-diff/cases"
DEFAULT_CACHE_DIR = ROOT / "build/sizes/census"
CHAINWIDE_SLOT_MODE = {"eul"}
RESEARCH_FLAGS = ("--legacy-source-rewrite",) + tuple(
    x for name in (
        "block-chainid", "block-difficulty", "block-basefee", "block-blobbasefee",
        "block-gaslimit", "block-prevrandao", "tx-gasprice", "address-balance-units",
        "gasleft", "staticcall", "delegatecall", "low-level-call-outcome",
        "native-value-transfer", "self-call", "try-catch",
    ) for x in ("--allow-divergence", name))
TABLE_VERSION = 3
CACHE_KEEP = 8
STATUSES = {"ERR", "TIMEOUT", "EMPTY", "ARTIFACT_ERR"}
COL_SOURCE, COL_NAME, COL_NUM = 44, 28, 8
HEADER = (f"# {'source':<{COL_SOURCE}} {'contract':<{COL_NAME}} "
          f"{'approval':>{COL_NUM}} {'teal_ops':>{COL_NUM}} {'clear':>{COL_NUM}}")


@dataclass(frozen=True)
class Unit:
    id: str
    args: tuple[str, ...]
    inputs: tuple[Path, ...] = ()


@dataclass(frozen=True)
class Row:
    source: str
    name: str
    approval: str
    ops: str
    clear: str

    @property
    def ok(self) -> bool:
        return self.approval.isdigit()


def git(*args: str, cwd: Path = ROOT) -> str:
    return subprocess.run(["git", *args], cwd=cwd, capture_output=True,
                          text=True, check=True).stdout.strip()


def sha256(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def digest(value: object) -> str:
    return hashlib.sha256(json.dumps(value, sort_keys=True).encode()).hexdigest()


def source_identity(root: Path = ROOT) -> str:
    """Same serialization as cmake/WriteBuildManifest.cmake."""
    pin = git("rev-parse", "HEAD:solidity", cwd=root)
    paths = [root / "CMakeLists.txt"]
    paths += [p for folder in ("src", "cmake") for p in (root / folder).rglob("*") if p.is_file()]
    records = [f"solidity:{pin}\n"]
    records += [f"{p.relative_to(root).as_posix()}:{sha256(p)}\n" for p in sorted(paths)]
    return hashlib.sha256("".join(records).encode()).hexdigest()


def compiler_identity(compiler: Path, root: Path | None = ROOT) -> dict:
    manifest = compiler.parent / "puya-sol-build-manifest.txt"
    values = dict(line.split("=", 1) for line in manifest.read_text().splitlines()
                  if "=" in line and not line.startswith(" "))
    if values.get("puya_sol_sha256") != sha256(compiler):
        raise ValueError(f"{compiler}: binary does not match {manifest}")
    stdlib = compiler.parent / "share/puya-sol/libs/AVM.sol"
    if values.get("avm_stdlib_sha256") != sha256(stdlib):
        raise ValueError(f"{compiler}: packaged standard library does not match {manifest}")
    if not values.get("source_sha256"):
        raise ValueError(f"{manifest}: missing source identity; rebuild the build-manifest target")
    if root is not None and values["source_sha256"] != source_identity(root):
        raise ValueError(f"{compiler}: stale compiler; run cmake --build build first")
    if root is not None and (
            git("rev-parse", "HEAD", cwd=root / "solidity") != values["solidity_commit"]
            or git("status", "--porcelain", "--untracked-files=all", cwd=root / "solidity")):
        raise ValueError("Solidity checkout differs from the clean, pinned compiler dependency")
    return {k: values[k] for k in ("root_commit", "root_tree_state", "solidity_commit",
                                   "source_sha256", "puya_sol_sha256", "avm_stdlib_sha256")}


def default_compiler() -> Path:
    return Path(os.environ.get("PUYA_SOL_COMPILER") or ROOT / "build/puya-sol").resolve()


def default_puya() -> Path | None:
    path = os.environ.get("PUYA_SOL_PUYA")
    if not path:
        local = ROOT / "puya/.venv/bin/puya"
        path = str(local) if local.exists() else shutil.which("puya")
    return Path(path).resolve() if path else None


def puya_signature(puya: Path) -> dict:
    """Hash the selected entry point's actual installed backend, not a nearby checkout."""
    with puya.open() as stream:
        shebang = stream.readline().strip()
    if not shebang.startswith("#!"):
        raise ValueError(f"{puya}: expected the Puya Python entry point")
    interpreter = shlex.split(shebang[2:])
    # These packages contain the backend, AWST models, op definitions and templates.
    probe = r"""
import hashlib, importlib.util, json, pathlib
out = {}
for name in ('puya', '_puya_lib'):
    spec = importlib.util.find_spec(name)
    if spec is None:
        raise RuntimeError('missing backend package: ' + name)
    root = pathlib.Path(next(iter(spec.submodule_search_locations)))
    h = hashlib.sha256()
    for p in sorted(root.rglob('*')):
        if p.is_file() and '__pycache__' not in p.parts and p.suffix != '.pyc':
            h.update(p.relative_to(root).as_posix().encode() + b'\0' + p.read_bytes())
    out[name] = h.hexdigest()
print(json.dumps(out, sort_keys=True))
"""
    packages = subprocess.run([*interpreter, "-c", probe], capture_output=True,
                              text=True, check=True, timeout=60)
    version = subprocess.run([str(puya), "--version"], capture_output=True,
                             text=True, check=True, timeout=60).stdout.strip()
    return {"version": version, "packages": json.loads(packages.stdout)}


def regression_units() -> list[Unit]:
    # Conservative dependency closure: fixtures may import sibling contracts.
    inputs = tuple(sorted(REGRESSION_DIR.rglob("*.sol")))
    return [Unit(p.relative_to(REGRESSION_DIR).as_posix(),
                 ("--source", str(p), "--import-path", str(p.parent)), inputs) for p in inputs]


def chainwide_units(local: bool = False) -> list[Unit]:
    metadata = sorted(CHAINWIDE_DIR.glob("*/case.json")) if local else [
        ROOT / p for p in git("ls-files", "tests/chainwide-historical-diff/cases/*/case.json").splitlines()]
    units = []
    for meta in metadata:
        case_dir = meta.parent
        case = json.loads(meta.read_text())
        tag = case_dir.name
        multifile = case.get("multifile")
        if multifile:
            src = case_dir / "src"
            files = [multifile["main"]] + [f for f in multifile["files"] if f != multifile["main"]]
            sources = [src / f for f in files]
            args = [x for p in sources for x in ("--source", str(p))]
            args += ["--import-path", str(src)]
            args += [x for r in multifile.get("remappings", []) for x in ("--remapping", r)]
            inputs = [*sources, *src.rglob("*.sol")]
        else:
            inputs = [case_dir / "prepared.sol"]
            args = ["--source", str(inputs[0])]
        args += ["--contract-abi", "evm"]
        if tag in CHAINWIDE_SLOT_MODE:
            args.append("--evm-storage-layout")
        units.append(Unit(tag, tuple(args), tuple([meta, *inputs])))
    return units


CORPORA = {"regression": regression_units,
           "regression-slot": lambda: [Unit(u.id, (*u.args, "--evm-storage-layout"), u.inputs)
                                       for u in regression_units()],
           "chainwide": chainwide_units,
           "chainwide-local": lambda: chainwide_units(True)}


def inputs_identity(units: list[Unit]) -> str:
    def relative(value: str) -> str:
        return value.replace(str(ROOT) + "/", "$ROOT/")
    paths = {p for u in units for p in u.inputs}
    paths.update((ROOT / "src/libs").rglob("*.sol"))
    return digest({"flags": RESEARCH_FLAGS,
                   "units": [(u.id, [relative(a) for a in u.args]) for u in units],
                   "files": {relative(str(p)): sha256(p) for p in sorted(paths)}})


def ensure_idle() -> None:
    if subprocess.run(["pgrep", "-f", r"(^|/| )pytest tests/"],
                      stdout=subprocess.DEVNULL).returncode == 0:
        raise ValueError("a semantic suite is running; wait before building or measuring sizes")


@contextlib.contextmanager
def run_lock():
    ensure_idle()
    directory = ROOT / "build/sizes"
    directory.mkdir(parents=True, exist_ok=True)
    with (directory / ".lock").open("a") as stream:
        try:
            fcntl.flock(stream, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise ValueError("another size run is active") from None
        yield


def count_ops(teal: str) -> int:
    """Static TEAL instruction lines, NOT decoded opcodes or execution budget."""
    # Inspect only the opcode token: // and : may occur inside byte literals.
    return sum(bool(tokens := line.split())
               and not tokens[0].startswith(("#", "//")) and not tokens[0].endswith(":")
               for line in teal.splitlines())


def run_compiler(cmd: list[str], timeout: int) -> tuple[int | None, str]:
    with subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          text=True, start_new_session=True, cwd=ROOT) as process:
        try:
            output, _ = process.communicate(timeout=timeout)
            return process.returncode, output
        except (subprocess.TimeoutExpired, KeyboardInterrupt) as error:
            os.killpg(process.pid, signal.SIGTERM)
            try:
                output, _ = process.communicate(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                output, _ = process.communicate()
            if isinstance(error, KeyboardInterrupt):
                raise
            return None, output


def compile_unit(unit: Unit, compiler: Path, puya: Path, work: Path, timeout: int,
                 log_dir: Path | None, teal_dir: Path | None) -> tuple[list[Row], float]:
    started = time.monotonic()
    with tempfile.TemporaryDirectory(prefix="unit-", dir=work) as directory:
        out = Path(directory)
        cmd = [str(compiler), *RESEARCH_FLAGS, *unit.args, "--puya-path", str(puya),
               "--output-dir", str(out), "--log-level", "error", "--no-output-logs"]
        code, output = run_compiler(cmd, timeout)
        status = "TIMEOUT" if code is None else "ERR" if code else None
        rows = []
        if status is None:
            for approval in sorted(out.rglob("*.approval.bin")):
                stem = approval.relative_to(out).as_posix().removesuffix(".approval.bin")
                base = approval.name.removesuffix(".approval.bin")
                clear = approval.with_name(base + ".clear.bin")
                teal = approval.with_name(base + ".approval.teal")
                if not all(p.is_file() and p.stat().st_size for p in (approval, clear, teal)):
                    status = "ARTIFACT_ERR"
                    output += f"\nMissing or empty output for {stem}"
                    break
                rows.append(Row(unit.id, stem, str(approval.stat().st_size),
                                str(count_ops(teal.read_text())), str(clear.stat().st_size)))
                if teal_dir is not None:
                    dest = teal_dir / unit.id / (stem + ".approval.teal")
                    dest.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copyfile(teal, dest)
            if not rows and status is None:
                status = "EMPTY"
        if status:
            rows = [Row(unit.id, "*", status, status, status)]
            if log_dir is not None:
                target = log_dir / (unit.id + ".log")
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(shlex.join(cmd) + f"\n[{status}]\n" + output)
    return rows, time.monotonic() - started


def format_rows(corpus: str, rows: list[Row], metadata: dict | None = None) -> str:
    output = "--out /tmp/sizes.txt" if corpus == "chainwide-local" or metadata and metadata.get("only") else "--write"
    selection = f" --only {shlex.quote(metadata['only'])}" if metadata and metadata.get("only") else ""
    lines = [f"# puya-sol bytecode sizes v{TABLE_VERSION}: corpus {corpus}",
             "# approval/clear: assembled bytes; teal_ops: static instructions, not runtime cost",
             f"# regenerate: python3 tests/sizes/sizes.py --corpus {corpus}{selection} {output}"]
    if metadata:
        lines += [f"# inputs: {metadata['inputs']}", f"# backend: {metadata['backend']['version']}"]
    lines.append(HEADER)
    for r in sorted(rows, key=lambda row: (row.source, row.name)):
        lines.append(f"{r.source:<{COL_SOURCE}} {r.name:<{COL_NAME}} {r.approval:>{COL_NUM}} "
                     f"{r.ops:>{COL_NUM}} {r.clear:>{COL_NUM}}")
    return "\n".join(lines) + "\n"


def parse_rows(text: str) -> dict[tuple[str, str], Row]:
    table = {}
    for line in text.splitlines():
        if not line.strip() or line.startswith("#"):
            continue
        row = Row(*line.split())
        if not (all(v.isdigit() for v in (row.approval, row.ops, row.clear))
                or row.name == "*" and row.approval in STATUSES and row.approval == row.ops == row.clear):
            raise ValueError(f"invalid size row: {line}")
        key = (row.source, row.name)
        if key in table:
            raise ValueError(f"duplicate size row: {key}")
        table[key] = row
    return table


def prune_cache(directory: Path, keep: int = CACHE_KEEP) -> None:
    """Only our versioned, atomic JSON records; no raw TEAL or worktree deletion."""
    entries = sorted(directory.glob(f"v{TABLE_VERSION}-*.json"), key=lambda p: p.stat().st_mtime, reverse=True)
    for old in entries[keep:]:
        old.unlink()


def census(corpus: str, compiler: Path, puya: Path, *, jobs: int = 2, timeout: int = 900,
           only: str | None = None, log_dir: Path | None = None,
           cache_dir: Path | None = DEFAULT_CACHE_DIR, teal_dir: Path | None = None,
           compiler_root: Path | None = ROOT) -> tuple[list[Row], dict]:
    units = CORPORA[corpus]()
    if only:
        units = [u for u in units if only in u.id]
    if not units or len({u.id for u in units}) != len(units):
        raise ValueError("empty corpus or duplicate unit ids")
    metadata = {"schema": TABLE_VERSION, "tool": sha256(Path(__file__)),
                "corpus": corpus, "only": only, "units": [u.id for u in units],
                "inputs": inputs_identity(units),
                "compiler": compiler_identity(compiler, compiler_root), "backend": puya_signature(puya),
                "timeout": timeout}
    key = digest(metadata)
    cached = cache_dir / f"v{TABLE_VERSION}-{key}.json" if cache_dir else None
    if cached and cached.exists() and teal_dir is None:
        try:
            data = json.loads(cached.read_text())
            rows = list(parse_rows(data["table"]).values())
            if data["metadata"] == metadata and data["sha256"] == digest(data["table"]):
                print(f"{corpus}: cached census ({len(rows)} rows)", file=sys.stderr)
                cached.touch()
                return rows, metadata
        except (OSError, ValueError, KeyError, TypeError):
            pass
    rows = []
    with tempfile.TemporaryDirectory(prefix="puya-sol-sizes-") as directory:
        with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
            futures = {pool.submit(compile_unit, u, compiler, puya, Path(directory), timeout,
                                   log_dir, teal_dir): u for u in units}
            for i, future in enumerate(concurrent.futures.as_completed(futures), 1):
                result, elapsed = future.result()
                rows.extend(result)
                print(f"[{i}/{len(units)}] {futures[future].id}: "
                      f"{result[0].approval if not result[0].ok else 'ok'} ({elapsed:.1f}s)",
                      file=sys.stderr, flush=True)
    if (inputs_identity(units) != metadata["inputs"]
            or compiler_identity(compiler, compiler_root) != metadata["compiler"]
            or puya_signature(puya) != metadata["backend"]):
        raise ValueError("compiler, backend or corpus changed during measurement; discard this run")
    rows.sort(key=lambda row: (row.source, row.name))
    if cached and not any(r.approval in {"TIMEOUT", "ARTIFACT_ERR"} for r in rows):
        cache_dir.mkdir(parents=True, exist_ok=True)
        table = format_rows(corpus, rows, metadata)
        with tempfile.NamedTemporaryFile(mode="w", dir=cache_dir, delete=False) as stream:
            json.dump({"metadata": metadata, "table": table, "sha256": digest(table)}, stream)
        Path(stream.name).replace(cached)
        prune_cache(cache_dir)
    return rows, metadata


def diff_tables(old: dict[tuple[str, str], Row], new: dict[tuple[str, str], Row]) -> dict:
    """Only like-for-like successful programs contribute to size deltas."""
    common = {k for k in old.keys() & new.keys() if old[k].ok and new[k].ok}
    statuses = lambda rows: {r.source: r.approval for r in rows.values() if not r.ok}
    os_, ns = statuses(old), statuses(new)
    old_sources, new_sources = {r.source for r in old.values()}, {r.source for r in new.values()}
    transitions = [{"source": s, "before": os_.get(s, "OK"), "after": ns.get(s, "OK")}
                   for s in sorted(old_sources & new_sources) if os_.get(s) != ns.get(s)]
    changed = []
    for key in sorted(common):
        a, b = old[key], new[key]
        if a != b:
            changed.append({"source": key[0], "contract": key[1],
                            **{f"{name}_{suffix}": value for name, av, bv in (
                                ("bytes", a.approval, b.approval), ("ops", a.ops, b.ops),
                                ("clear", a.clear, b.clear))
                               for suffix, value in (("old", int(av)), ("new", int(bv)),
                                                     ("delta", int(bv) - int(av)))}})
    changed.sort(key=lambda c: (-abs(c["bytes_delta"]), -abs(c["ops_delta"]), c["source"], c["contract"]))
    totals = {f"{name}_total_{side}": sum(int(getattr(rows[k], field)) for k in common)
              for name, field in (("bytes", "approval"), ("ops", "ops"), ("clear", "clear"))
              for side, rows in (("old", old), ("new", new))}
    return {"programs_old": sum(r.ok for r in old.values()), "programs_new": sum(r.ok for r in new.values()),
            "comparable": len(common), **totals, "changed": changed, "status_changes": transitions,
            "units_added": [{"source": s, "status": ns.get(s, "OK")} for s in sorted(new_sources - old_sources)],
            "units_removed": [{"source": s, "status": os_.get(s, "OK")} for s in sorted(old_sources - new_sources)],
            "regressions": [s for s in transitions if s["before"] == "OK" and s["after"] != "OK"
                            or s["before"] == "EMPTY" and s["after"] not in {"OK", "EMPTY"}],
            "added": [asdict(new[k]) for k in sorted(new.keys() - old.keys())
                      if new[k].ok and k[0] not in os_],
            "removed": [asdict(old[k]) for k in sorted(old.keys() - new.keys())
                        if old[k].ok and k[0] not in ns],
            "grew": sum(c["bytes_delta"] > 0 for c in changed),
            "shrank": sum(c["bytes_delta"] < 0 for c in changed)}


def print_diff(d: dict, limit: int) -> None:
    print(f"compiled programs: {d['programs_old']} -> {d['programs_new']}; "
          f"comparable: {d['comparable']} (only these contribute to deltas)")
    for key, label in (("bytes", "approval bytes"), ("ops", "static TEAL instructions"), ("clear", "clear bytes")):
        old, new = d[f"{key}_total_old"], d[f"{key}_total_new"]
        pct = f"{(new-old)/old*100:+.2f}%" if old else "n/a"
        print(f"{label}: {old} -> {new} ({new-old:+d}, {pct})")
    print(f"changed: {len(d['changed'])}; grew: {d['grew']}; shrank: {d['shrank']}; "
          f"compile regressions: {len(d['regressions'])}")
    for c in d["changed"][:limit or None]:
        print(f"{c['source']} {c['contract']}: {c['bytes_old']} -> {c['bytes_new']} B "
              f"({c['bytes_delta']:+d}); TEAL {c['ops_delta']:+d}; clear {c['clear_delta']:+d}")
    for s in d["status_changes"]:
        print(f"status: {s['source']}: {s['before']} -> {s['after']}")
    for kind in ("units_added", "units_removed"):
        for unit in d[kind]:
            print(f"{kind}: {unit['source']}: {unit['status']}")
    for status in ("added", "removed"):
        for r in d[status]:
            print(f"{status}: {r['source']} {r['name']} ({r['approval']} B)")


def compare(old_text: str, new_text: str, limit: int) -> bool:
    print_diff(diff_tables(parse_rows(old_text), parse_rows(new_text)), limit)
    return old_text != new_text


def positive(value: str) -> int:
    number = int(value)
    if number < 1:
        raise argparse.ArgumentTypeError("must be positive")
    return number


def nonnegative(value: str) -> int:
    number = int(value)
    if number < 0:
        raise argparse.ArgumentTypeError("must be nonnegative")
    return number


def add_common_args(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--corpus", choices=sorted(CORPORA), default="regression")
    parser.add_argument("--puya", type=Path, default=default_puya())
    parser.add_argument("--jobs", type=positive, default=2)
    parser.add_argument("--timeout", type=positive, default=900)
    parser.add_argument("--only", help="select matching units (not valid with --write/--check)")
    parser.add_argument("--log-dir", type=Path)
    parser.add_argument("--limit", type=nonnegative, default=40, help="changed rows shown; 0 means all")
    parser.add_argument("--no-cache", action="store_true")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", type=Path, default=default_compiler())
    add_common_args(parser)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--write", action="store_true")
    mode.add_argument("--check", action="store_true")
    mode.add_argument("--out", type=Path)
    mode.add_argument("--compare", nargs=2, type=Path, metavar=("OLD", "NEW"))
    args = parser.parse_args()
    if args.compare:
        return int(compare(*(p.read_text() for p in args.compare), args.limit))
    if (args.write or args.check) and (args.only or args.corpus == "chainwide-local"):
        parser.error("canonical baselines require the full tracked corpus")
    committed = HERE / f"{args.corpus}.txt"
    if args.check and not committed.exists():
        parser.error(f"missing {committed}; run --write first")
    if args.puya is None:
        parser.error("Puya not found; supply --puya")
    with run_lock():
        rows, metadata = census(args.corpus, args.compiler.resolve(), args.puya.resolve(),
                                jobs=args.jobs, timeout=args.timeout, only=args.only,
                                log_dir=args.log_dir, cache_dir=None if args.no_cache else DEFAULT_CACHE_DIR)
        text = format_rows(args.corpus, rows, metadata)
        if any(r.approval in {"TIMEOUT", "ARTIFACT_ERR"} for r in rows):
            raise ValueError("incomplete measurements; baseline was not written")
        if args.write:
            committed.write_text(text)
            print(f"wrote {committed.relative_to(ROOT)}")
        elif args.check:
            if compare(committed.read_text(), text, args.limit):
                print(f"stale baseline: {committed}; run --write")
                return 1
            print(f"{committed.relative_to(ROOT)} is up to date")
        elif args.out:
            args.out.write_text(text)
        else:
            print(text, end="")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        sys.exit(str(error))
