"""Compile a Solidity test file to puya-sol ARC56 artifacts."""
from __future__ import annotations

import hashlib
import json
import os
import shutil
import subprocess
import tempfile
from dataclasses import dataclass, field
from pathlib import Path

from .paths import CACHE_DIR, COMPILER, PUYA, PUYA_BACKEND_SRC


# Lazily-computed signature of the compiler stack (puya-sol binary + puya
# backend source tree). Recomputed once per process — both are stable for
# the duration of a test run. If either changes (rebuild puya-sol, edit
# puya/src), the signature changes → cache misses for everything → fresh
# compiles. Cheap because we only stat one file + walk one tree (no read).
_COMPILER_STACK_SIG: str | None = None


def _puya_backend_sig() -> str:
    """Stable signature of the puya backend source tree.

    Hashes `git rev-parse HEAD:` of the puya submodule (content-
    addressable), plus a content hash of any locally-modified files
    so dirty work-in-progress still invalidates the cache. Replaces
    the prior `max mtime over all .py files` scheme — that
    over-invalidated on every `git stash` / `git checkout` of the
    submodule (file mtimes bump even when content is unchanged),
    forcing a full re-compile of all ~1322 semantic tests after any
    submodule switch.

    Returns the empty string if `puya/` isn't a git checkout (e.g.
    fresh tarball); the caller's fallback isn't worth the
    complexity — full re-compile is correct in that case.
    """
    import subprocess
    puya_dir = PUYA_BACKEND_SRC.parent  # puya/
    if not (puya_dir / ".git").exists():
        return ""
    try:
        head = subprocess.run(
            ["git", "rev-parse", "HEAD"],
            cwd=str(puya_dir), capture_output=True, text=True, timeout=5
        ).stdout.strip()
    except Exception:
        return ""
    parts = [f"head:{head}"]
    # If any tracked file under src/ is modified locally, mix its
    # content into the signature so iterating on puya code without
    # committing still invalidates the cache.
    try:
        dirty = subprocess.run(
            ["git", "status", "--porcelain", "--", "src/"],
            cwd=str(puya_dir), capture_output=True, text=True, timeout=5
        ).stdout
        for line in dirty.splitlines():
            # Format: "XY <path>"
            if len(line) < 4:
                continue
            relpath = line[3:].strip()
            fpath = puya_dir / relpath
            try:
                content = fpath.read_bytes()
                parts.append(f"dirty:{relpath}:{hashlib.sha256(content).hexdigest()}")
            except (FileNotFoundError, IsADirectoryError):
                pass
    except Exception:
        pass
    return "|".join(parts)


def _compiler_stack_sig() -> str:
    global _COMPILER_STACK_SIG
    if _COMPILER_STACK_SIG is not None:
        return _COMPILER_STACK_SIG
    h = hashlib.sha256()
    # puya-sol binary: mtime_ns + size (cheap and content-equivalent for our
    # build → never two builds same mtime+size with different content).
    try:
        st = COMPILER.stat()
        h.update(f"compiler:{st.st_mtime_ns}:{st.st_size}".encode())
    except FileNotFoundError:
        h.update(b"compiler:missing")
    # puya backend source tree: git HEAD of the puya submodule + dirty
    # file content hashes. Stable across `git stash`/`git checkout`
    # round-trips, unlike the prior mtime walk.
    h.update(f"puya_src:{_puya_backend_sig()}".encode())
    _COMPILER_STACK_SIG = h.hexdigest()
    return _COMPILER_STACK_SIG


def _compute_cache_key(
    source_path: Path,
    all_sources: list[Path],
    import_dir: Path | None,
    remappings: list[str],
    ensure_budget: dict[str, int] | None,
    via_yul_behavior: bool,
    evm_version: str | None,
) -> str:
    """Cache key from inputs that affect the compile output.

    Hashes: every source file's contents, the compile flags, and the
    compiler stack signature (puya-sol binary + puya backend tree).
    `import_dir` content is captured indirectly via `all_sources` (the
    multisource splitter writes all sources to the temp import_dir and
    lists them in `all_sources`).
    """
    h = hashlib.sha256()
    h.update(_compiler_stack_sig().encode())
    # Hash source contents in stable order
    seen = set()
    for p in [source_path, *all_sources]:
        p = p.resolve()
        if p in seen:
            continue
        seen.add(p)
        try:
            data = p.read_bytes()
        except FileNotFoundError:
            data = b""
        # Include the basename so renaming a file invalidates the cache
        h.update(f"src:{p.name}:{len(data)}\n".encode())
        h.update(data)
    # Flags
    flags = {
        "remappings": sorted(remappings),
        "ensure_budget": dict(sorted((ensure_budget or {}).items())),
        "via_yul_behavior": via_yul_behavior,
        "evm_version": evm_version,
    }
    h.update(json.dumps(flags, sort_keys=True).encode())
    return h.hexdigest()


def _cache_lookup(key: str, out_dir: Path) -> bool:
    """If the cache has an entry for `key`, copy its files into `out_dir`.

    Returns True on hit (out_dir now populated), False on miss.
    """
    entry = CACHE_DIR / key
    if not entry.is_dir():
        return False
    # Sanity check: cache entry must have at least one .arc56.json
    if not any(entry.glob("*.arc56.json")):
        return False
    out_dir.mkdir(parents=True, exist_ok=True)
    for src in entry.iterdir():
        if src.is_file():
            shutil.copy2(src, out_dir / src.name)
    return True


def _cache_store(key: str, out_dir: Path) -> None:
    """Atomically store the compile artifacts in `out_dir` under `key`.

    Concurrent stores (e.g. xdist with -n 2) are safe: each writer
    populates a private tmp dir then atomically renames. If the target
    already exists (raced), the rename fails and we silently drop the
    duplicate — the first writer wins, and the result is identical.
    """
    CACHE_DIR.mkdir(parents=True, exist_ok=True)
    entry = CACHE_DIR / key
    if entry.exists():
        return
    # Build a sibling tmp dir, copy artifacts in, rename into place.
    tmp = Path(tempfile.mkdtemp(prefix="cache_", dir=str(CACHE_DIR)))
    try:
        for src in out_dir.iterdir():
            if src.is_file():
                shutil.copy2(src, tmp / src.name)
        try:
            os.rename(tmp, entry)
            tmp = None  # successful rename — don't clean up
        except OSError:
            # Another worker raced and won; that's fine.
            pass
    finally:
        if tmp is not None and tmp.exists():
            shutil.rmtree(tmp, ignore_errors=True)


@dataclass
class CompiledArtifacts:
    """One compiled .sol → multiple contracts keyed by name.

    Each value is a dict with: arc56 (Path), approval_teal (Path),
    clear_teal (Path), sol_path (Path to the resolved source).

    `main_source_text` holds the body of the main source so callers can
    pick a "last-declared" contract after the temp directory used by the
    multi-source splitter has been removed.
    """
    by_contract: dict[str, dict] = field(default_factory=dict)
    main_source: Path | None = None
    main_source_text: str = ""

    def __bool__(self) -> bool:
        return bool(self.by_contract)

    def last_deployable(self, contract_name: str | None = None) -> str | None:
        """Pick the last-declared contract in source order, optionally filtered by name."""
        if contract_name:
            return contract_name if contract_name in self.by_contract else None
        text = self.main_source_text or (
            self.main_source.read_text() if self.main_source and self.main_source.exists() else ""
        )
        if not text:
            return next(iter(self.by_contract), None)
        import re
        names = re.findall(r"(?:contract|library)\s+(\w+)", text)
        for n in reversed(names):
            if n in self.by_contract:
                return n
        return next(iter(self.by_contract), None)


class CompileError(Exception):
    def __init__(self, message: str, stdout: str = "", stderr: str = ""):
        super().__init__(message)
        self.stdout = stdout
        self.stderr = stderr


def compile_sol(
    sol_path: Path,
    out_dir: Path,
    *,
    ensure_budget: dict[str, int] | None = None,
    via_yul_behavior: bool = False,
    evm_version: str | None = None,
    timeout: int = 120,
) -> CompiledArtifacts:
    """Compile a .sol file with puya-sol → puya. Returns CompiledArtifacts.

    Raises CompileError on non-zero exit. The caller is expected to
    interpret a compile failure as the test's terminal outcome.
    """
    from multisource_splitter import split_multisource

    out_dir.mkdir(parents=True, exist_ok=True)
    source_path, all_sources, import_dir, remappings = split_multisource(sol_path)

    # Cache lookup: hash all source files + flags + compiler-stack signature.
    # On hit, copy artifacts straight into out_dir and skip the subprocess.
    cache_key = _compute_cache_key(
        source_path=source_path,
        all_sources=all_sources,
        import_dir=import_dir,
        remappings=remappings,
        ensure_budget=ensure_budget,
        via_yul_behavior=via_yul_behavior,
        evm_version=evm_version,
    )
    cache_hit = _cache_lookup(cache_key, out_dir)
    main_source_text = ""
    try:
        main_source_text = source_path.read_text()
    except Exception:
        pass
    if cache_hit:
        if import_dir:
            shutil.rmtree(import_dir, ignore_errors=True)
        artifacts = CompiledArtifacts(
            main_source=source_path, main_source_text=main_source_text
        )
        for arc56 in out_dir.glob("*.arc56.json"):
            name = arc56.stem.replace(".arc56", "")
            artifacts.by_contract[name] = {
                "arc56": arc56,
                "approval_teal": out_dir / f"{name}.approval.teal",
                "clear_teal": out_dir / f"{name}.clear.teal",
                "sol_path": source_path,
            }
        return artifacts

    cmd = [str(COMPILER), "--source", str(source_path)]
    for extra in all_sources:
        if str(extra) != str(source_path):
            cmd += ["--source", str(extra)]
    cmd += ["--output-dir", str(out_dir), "--puya-path", str(PUYA)]
    if import_dir:
        cmd += ["--import-path", str(import_dir)]
    for rmap in remappings:
        cmd += ["--remapping", rmap]
    if ensure_budget:
        for func, budget in ensure_budget.items():
            cmd += ["--ensure-budget", f"{func}:{budget}"]
    if via_yul_behavior:
        cmd += ["--via-yul-behavior"]
    if evm_version:
        cmd += ["--evm-version", evm_version]

    # Strip PYTHONPATH from the env when invoking puya-sol → puya. The
    # test runner often needs `PYTHONPATH=~/.local/lib/python3.12/site-packages`
    # set (for algosdk), but that user-site contains an OLDER puya install
    # which shadows the project's puya/.venv puya when puya-sol spawns the
    # backend subprocess. Result: missing optimizations like
    # box_dynamic_array_concat_fixed → unoptimized concat hits the 4KB
    # stack-value cap on long dynamic arrays.
    env = {k: v for k, v in os.environ.items() if k != "PYTHONPATH"}
    try:
        result = subprocess.run(
            cmd, capture_output=True, text=True, timeout=timeout, env=env
        )
    except subprocess.TimeoutExpired as e:
        if import_dir:
            shutil.rmtree(import_dir, ignore_errors=True)
        raise CompileError(f"compilation timed out after {timeout}s") from e

    # Snapshot the main source body BEFORE rmtree so `last_deployable`
    # keeps working after the multi-source splitter's temp dir is removed.
    try:
        main_source_text = source_path.read_text()
    except Exception:
        main_source_text = ""

    if import_dir:
        shutil.rmtree(import_dir, ignore_errors=True)

    if result.returncode != 0:
        raise CompileError(
            f"puya-sol exited {result.returncode}",
            stdout=result.stdout,
            stderr=result.stderr,
        )

    # Successful compile — populate cache for next run. Failures are
    # deliberately NOT cached (could be transient: disk full, segfault,
    # etc., and the cost of a re-run is bounded by the test timeout).
    _cache_store(cache_key, out_dir)

    artifacts = CompiledArtifacts(main_source=source_path, main_source_text=main_source_text)
    for arc56 in out_dir.glob("*.arc56.json"):
        name = arc56.stem.replace(".arc56", "")
        artifacts.by_contract[name] = {
            "arc56": arc56,
            "approval_teal": out_dir / f"{name}.approval.teal",
            "clear_teal": out_dir / f"{name}.clear.teal",
            "sol_path": source_path,
        }
    return artifacts
