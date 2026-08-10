"""Compile a Solidity test file to puya-sol ARC56 artifacts."""
from __future__ import annotations

import hashlib
import json
import os
import shutil
import shlex
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
            # Always mix in the status record itself. Deleted and renamed files
            # have no single current path to hash, but still change the backend.
            parts.append(f"status:{line}")
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
    extra_args: list[str] | None = None,
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
        "extra_args": list(extra_args or []),
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


# ── Two-stage (AWST-content) backend cache ──────────────────────────────────
# The puya-sol *frontend* (C++) is ~0.05s and deterministic; the puya *backend*
# (Python) is ~5s, ~65% of which is interpreter+import startup paid per contract.
# The L1 cache above keys on the puya-sol *binary* (mtime+size), so any rebuild
# of the compiler invalidates every entry — a cold run then re-pays the backend
# for all ~1322 contracts (~70 min, on every dev iteration). This L2 cache keys
# the backend artifacts on the AWST *content* instead: a localized codegen
# change leaves most contracts' AWST byte-identical, so they hit here and only
# the genuinely-changed contracts re-run puya. Safe by construction — a hit
# means (AWST + semantic options + puya version) are byte-identical, so the
# emitted TEAL is identical; any mismatch is a miss (slower, never wrong).
_BACKEND_CACHE_DIR = CACHE_DIR / "backend"
# A no-op "backend" so puya-sol writes awst.json/options.json then exits without
# the ~5s Python step — used to compute the L2 key cheaply.
_NOOP_PUYA = shutil.which("true") or "/bin/true"
# Files the frontend writes; everything else in out_dir is a backend artifact.
_FRONTEND_ONLY_FILES = {"awst.json", "options.json", "puya-sol.log"}


def _puya_sol_cmd(
    source_path: Path,
    all_sources: list[Path],
    out_dir: Path,
    import_dir: Path | None,
    remappings: list[str],
    ensure_budget: dict[str, int] | None,
    via_yul_behavior: bool,
    evm_version: str | None,
    puya_path: str,
    extra_args: list[str] | None = None,
) -> list[str]:
    """Build the puya-sol argv. `puya_path` selects the backend: the real PUYA
    for a full compile, or a no-op (`true`) to emit AWST only."""
    cmd = [str(COMPILER), "--source", str(source_path)]
    for extra in all_sources:
        if str(extra) != str(source_path):
            cmd += ["--source", str(extra)]
    cmd += ["--output-dir", str(out_dir), "--puya-path", puya_path]
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
    if extra_args:
        cmd += list(extra_args)
    return cmd


def _normalized_options_bytes(options_path: Path) -> bytes:
    """Options JSON with run-specific paths stripped, for content-keying.

    `compilation_set` maps "<source-path>.<ContractName>" → output-dir; both
    sides vary per test run / out-dir but don't affect the emitted TEAL.
    Replace it with the sorted contract names (which DO affect output) and keep
    the semantic compile settings (optimization/debug level, target avm version,
    output flags, template vars).
    """
    try:
        data = json.loads(options_path.read_text())
    except Exception:
        return b""
    cset = data.get("compilation_set", {})
    names = sorted(str(k).rsplit(".", 1)[-1] for k in cset.keys())
    norm = {k: v for k, v in data.items() if k != "compilation_set"}
    norm["_contract_names"] = names
    return json.dumps(norm, sort_keys=True).encode()


def _flags_blob(
    remappings: list[str],
    ensure_budget: dict[str, int] | None,
    via_yul_behavior: bool,
    evm_version: str | None,
    extra_args: list[str] | None = None,
) -> bytes:
    """Stable serialization of the compile flags that affect output."""
    return json.dumps({
        "remappings": sorted(remappings),
        "ensure_budget": dict(sorted((ensure_budget or {}).items())),
        "via_yul_behavior": via_yul_behavior,
        "evm_version": evm_version,
        "extra_args": list(extra_args or []),
    }, sort_keys=True).encode()


def _backend_cache_key(
    out_dir: Path,
    flags_blob: bytes,
) -> str | None:
    """Content key for the backend step: AWST + normalized options + puya sig
    + compile flags.

    Deliberately EXCLUDES the puya-sol binary signature — that's the point: the
    frontend re-runs cheaply and emits identical AWST for any contract its
    change didn't touch, so those skip the backend. The flags are folded in as
    belt-and-suspenders in case any (ensure-budget/evm-version/via-yul) reaches
    the backend by a channel other than awst.json/options.json. Returns None if
    the frontend didn't produce the inputs.
    """
    awst = out_dir / "awst.json"
    options = out_dir / "options.json"
    if not awst.is_file() or not options.is_file():
        return None
    h = hashlib.sha256()
    h.update(b"backend_v1\n")
    h.update(f"puya:{_puya_backend_sig()}\n".encode())
    h.update(b"flags:")
    h.update(flags_blob)
    h.update(b"\nawst:")
    h.update(awst.read_bytes())
    h.update(b"\nopts:")
    h.update(_normalized_options_bytes(options))
    return h.hexdigest()


def _backend_cache_lookup(key: str, out_dir: Path) -> bool:
    """Restore cached backend artifacts (TEAL/ARC56/bin/tmpl) into out_dir.

    out_dir already holds the freshly-emitted awst.json/options.json from the
    frontend run; only backend files live in the cache entry. Returns True on
    hit (requires at least one .arc56.json, matching _cache_lookup).
    """
    entry = _BACKEND_CACHE_DIR / key
    if not entry.is_dir() or not any(entry.glob("*.arc56.json")):
        return False
    out_dir.mkdir(parents=True, exist_ok=True)
    for src in entry.iterdir():
        if src.is_file():
            shutil.copy2(src, out_dir / src.name)
    return True


def _backend_cache_store(key: str, out_dir: Path) -> None:
    """Atomically store out_dir's backend artifacts under `key` (same race-safe
    tmp-dir+rename scheme as _cache_store). The frontend trio
    (awst.json/options.json/puya-sol.log) is excluded — it's regenerated cheaply
    on every run."""
    _BACKEND_CACHE_DIR.mkdir(parents=True, exist_ok=True)
    entry = _BACKEND_CACHE_DIR / key
    if entry.exists():
        return
    tmp = Path(tempfile.mkdtemp(prefix="bcache_", dir=str(_BACKEND_CACHE_DIR)))
    try:
        for src in out_dir.iterdir():
            if src.is_file() and src.name not in _FRONTEND_ONLY_FILES:
                shutil.copy2(src, tmp / src.name)
        try:
            os.rename(tmp, entry)
            tmp = None
        except OSError:
            # Another worker raced and won; identical result, drop ours.
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


def _first_error_line(stderr: str, stdout: str = "") -> str:
    """': <first compiler error>' — triage lives in the message, not buried in
    an attribute nobody prints (the slot-mode sweep produced 110
    indistinguishable 'puya-sol exited 1' failures)."""
    for text in (stderr or "", stdout or ""):
        for ln in text.splitlines():
            if "error:" in ln.lower():
                # drop the absolute-path prefix: it ate the whole 160-char
                # budget and truncated every message to "...error: --evm"
                k = ln.lower().rfind("error:")
                return ": " + ln[k:].strip()[:200]
    tail = (stderr or stdout or "").strip().splitlines()
    return (": " + tail[-1][:160]) if tail else ""


def compile_sol(
    sol_path: Path,
    out_dir: Path,
    *,
    ensure_budget: dict[str, int] | None = None,
    via_yul_behavior: bool = False,
    evm_version: str | None = None,
    timeout: int = 120,
    extra_sources: list[Path] | None = None,
    extra_import_dir: Path | None = None,
    extra_remappings: list[str] | None = None,
    extra_args: list[str] | None = None,
) -> CompiledArtifacts:
    """Compile a .sol file with puya-sol → puya. Returns CompiledArtifacts.

    Raises CompileError on non-zero exit. The caller is expected to
    interpret a compile failure as the test's terminal outcome.
    """
    from multisource_splitter import split_multisource

    # PUYA_SOL_EXTRA_ARGS: extra compiler flags for experiment sweeps (e.g.
    # "--evm-storage-layout" to run a category in slot mode). Folded into
    # extra_args BEFORE cache-key computation, so cached artifacts stay
    # correctly keyed per flag set.
    _env_extra = shlex.split(os.environ.get("PUYA_SOL_EXTRA_ARGS", ""))
    if _env_extra:
        extra_args = list(extra_args or []) + _env_extra

    out_dir.mkdir(parents=True, exist_ok=True)
    if extra_sources is not None:
        # Caller supplied a REAL multi-file tree (e.g. a verified on-chain
        # contract fetched with its own remappings) rather than an upstream
        # inline `==== Source: ====` fixture — use it as-is.
        source_path = sol_path
        all_sources = list(extra_sources)
        import_dir = extra_import_dir
        remappings = list(extra_remappings or [])
    else:
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
        extra_args=extra_args,
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

    # Strip PYTHONPATH from the env when invoking puya-sol → puya. The
    # test runner often needs `PYTHONPATH=~/.local/lib/python3.12/site-packages`
    # set (for algosdk), but that user-site contains an OLDER puya install
    # which shadows the project's puya/.venv puya when puya-sol spawns the
    # backend subprocess. Result: missing optimizations like
    # box_dynamic_array_concat_fixed → unoptimized concat hits the 4KB
    # stack-value cap on long dynamic arrays.
    env = {k: v for k, v in os.environ.items() if k != "PYTHONPATH"}

    def _run(puya_path: str):
        return subprocess.run(
            _puya_sol_cmd(
                source_path, all_sources, out_dir, import_dir, remappings,
                ensure_budget, via_yul_behavior, evm_version, puya_path,
                extra_args,
            ),
            capture_output=True, text=True, timeout=timeout, env=env,
        )

    try:
        # Stage 1 — frontend only (no-op backend): emit awst.json/options.json
        # cheaply (~0.05s) to compute the AWST-content backend key. A frontend
        # compile error (bad Solidity, hard-errored EVM feature) surfaces here
        # — same terminal outcome as before.
        front = _run(_NOOP_PUYA)
        if front.returncode != 0:
            raise CompileError(
                f"puya-sol exited {front.returncode}"
                + _first_error_line(front.stderr, front.stdout),
                stdout=front.stdout, stderr=front.stderr,
            )

        # Stage 2 — backend. Reuse cached TEAL if the AWST is unchanged.
        # On a miss, first try the session-persistent `puya serve` backend
        # (see puya_serve.py): it runs the same awst_to_teal + artifact
        # writing on the stage-1 AWST without re-paying the Python import
        # startup, and ports the deploy.tmpl.json child-template step. On
        # ANY non-success there, fall back to the full puya-sol run
        # (frontend+backend+child-tmpl gen, exactly as before) — the
        # subprocess stays the source of truth for failures. Either way the
        # backend artifacts land in out_dir and are cached identically
        # (serve output is byte-identical to subprocess output).
        bkey = _backend_cache_key(
            out_dir,
            _flags_blob(remappings, ensure_budget, via_yul_behavior, evm_version,
                        extra_args),
        )
        if not (bkey and _backend_cache_lookup(bkey, out_dir)):
            from .puya_serve import compile_via_server
            if not compile_via_server(out_dir, timeout=timeout):
                full = _run(str(PUYA))
                if full.returncode != 0:
                    raise CompileError(
                        f"puya-sol exited {full.returncode}"
                        + _first_error_line(full.stderr, full.stdout),
                        stdout=full.stdout, stderr=full.stderr,
                    )
            if bkey:
                _backend_cache_store(bkey, out_dir)
    except subprocess.TimeoutExpired as e:
        raise CompileError(f"compilation timed out after {timeout}s") from e
    finally:
        # Snapshot the main source body BEFORE rmtree so `last_deployable`
        # keeps working after the multi-source splitter's temp dir is removed.
        try:
            main_source_text = source_path.read_text()
        except Exception:
            main_source_text = ""
        if import_dir:
            shutil.rmtree(import_dir, ignore_errors=True)

    # Successful compile — populate the L1 cache for instant warm reruns.
    # Failures are deliberately NOT cached (could be transient: disk full,
    # segfault, etc., and the cost of a re-run is bounded by the test timeout).
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
