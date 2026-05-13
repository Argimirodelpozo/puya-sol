"""Compile a Solidity test file to puya-sol ARC56 artifacts."""
from __future__ import annotations

import shutil
import subprocess
from dataclasses import dataclass, field
from pathlib import Path

from .paths import COMPILER, PUYA


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
    source_path, all_sources, import_dir = split_multisource(sol_path)

    cmd = [str(COMPILER), "--source", str(source_path)]
    for extra in all_sources:
        if str(extra) != str(source_path):
            cmd += ["--source", str(extra)]
    cmd += ["--output-dir", str(out_dir), "--puya-path", str(PUYA)]
    if import_dir:
        cmd += ["--import-path", str(import_dir)]
    if ensure_budget:
        for func, budget in ensure_budget.items():
            cmd += ["--ensure-budget", f"{func}:{budget}"]
    if via_yul_behavior:
        cmd += ["--via-yul-behavior"]
    if evm_version:
        cmd += ["--evm-version", evm_version]

    try:
        result = subprocess.run(
            cmd, capture_output=True, text=True, timeout=timeout
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
