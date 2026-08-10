"""High-level test harness.

A `Harness` instance compiles a .sol file once per test, deploys one of
its contracts, and exposes a typed `call()` method.

Pytest usage:

    def test_my_contract(harness):
        app = harness.compile_and_deploy("smoke/basic.sol")
        result = harness.call(app, "f(uint256)", 3)
        assert result.abi_return == (3, 3)

Or split:

    def test_explicit_steps(harness):
        artifacts = harness.compile("smoke/basic.sol")
        app = harness.deploy(artifacts, "C")
        ...
"""
from __future__ import annotations

import shutil
from pathlib import Path
from typing import Any

from .compile import CompiledArtifacts, CompileError, compile_sol
from .deploy import DeployedApp, DeployError, deploy
from .call import Result, call as _call, call_raw as _call_raw
from .localnet import LocalNet
from .paths import OUT_DIR, TESTS_DIR


class App:
    """Public-facing handle returned from `Harness.deploy()`.

    Delegates to the underlying `DeployedApp` plus a back-pointer to the
    Harness so callers can do `app.call("f()")` without threading the
    harness through.
    """

    def __init__(self, harness: "Harness", deployed: DeployedApp, contract_name: str):
        self._harness = harness
        self._deployed = deployed
        self.name = contract_name

    @property
    def app_id(self) -> int:
        return self._deployed.app_id

    @property
    def app_addr(self) -> str:
        return self._deployed.app_addr

    @property
    def app_spec(self):
        return self._deployed.app_spec

    @property
    def client(self):
        return self._deployed.client

    @property
    def balance_baseline(self) -> int:
        return self._deployed.balance_baseline

    def call(self, sig: str, *args, **kwargs) -> Result:
        return self._harness.call(self, sig, *args, **kwargs)


class Harness:
    """Per-test compile + deploy + call orchestrator.

    Usage is via the `harness` pytest fixture; tests rarely build one
    directly. Each test gets its own output directory under `out/<id>/`.
    """

    def __init__(self, localnet: LocalNet, out_dir: Path):
        self.localnet = localnet
        self.out_dir = out_dir
        self.out_dir.mkdir(parents=True, exist_ok=True)
        self._compile_count = 0
        self._compile_dirs: list[Path] = []

    @staticmethod
    def resolve_sol_path(spec: str | Path) -> Path:
        """Accept absolute Path, relative-to-tests Path, or 'category/file.sol' string."""
        if isinstance(spec, Path) and spec.is_absolute():
            return spec
        p = Path(spec)
        if p.exists():
            return p.resolve()
        candidate = TESTS_DIR / p
        if candidate.exists():
            return candidate.resolve()
        raise FileNotFoundError(f"can't find .sol file: {spec}")

    def compile(
        self,
        sol_path: str | Path,
        **opts,
    ) -> CompiledArtifacts:
        """Compile a .sol file. Raises CompileError on failure.

        Each compile gets an isolated compile-NNNN dir so the compile cache
        never captures a previous compile's artifacts (cache stores copy the
        whole directory). The final artifacts are ALSO mirrored to the test's
        out dir top level: the harness fixture wipes that dir at setup, and
        this mirror is what refreshes the repo's tracked out/ artifacts —
        without it every test run just deletes them.
        """
        resolved = self.resolve_sol_path(sol_path)
        self._compile_count += 1
        compile_dir = self.out_dir / f"compile-{self._compile_count:04d}"
        self._compile_dirs.append(compile_dir)
        artifacts = compile_sol(resolved, compile_dir, **opts)
        for src in compile_dir.iterdir():
            if src.is_file():
                shutil.copy2(src, self.out_dir / src.name)
        return artifacts

    def deploy(
        self,
        artifacts: CompiledArtifacts,
        contract_name: str | None = None,
        **deploy_opts,
    ) -> App:
        """Deploy a named contract from the compiled artifacts. Raises DeployError on failure.

        Accepted deploy_opts: ctor_args, fund_wei, postinit_args,
        postinit_budget_pool, extra_funding_microalgos.
        """
        name = artifacts.last_deployable(contract_name)
        if name is None:
            raise DeployError("no deployable contract in compile output")
        deployed = deploy(self.localnet, artifacts.by_contract[name], **deploy_opts)
        return App(self, deployed, name)

    def compile_and_deploy(
        self,
        sol_path: str | Path,
        contract_name: str | None = None,
        *,
        ensure_budget: dict[str, int] | None = None,
        via_yul_behavior: bool = False,
        evm_version: str | None = None,
        extra_args: list[str] | None = None,
        ctor_args: list | None = None,
        fund_wei: int = 0,
        postinit_args: list | None = None,
        postinit_budget_pool: int = 0,
        postinit_inner_txns: int = 0,
    ) -> App:
        """Common path: compile a .sol file and deploy one of its contracts.

        postinit_budget_pool: budget-helper opcode pool size for __postInit
            call. Use when the constructor body is opcode-heavy (e.g. a
            long ctor loop pushing into storage).
        postinit_inner_txns: extra inner-txn fee headroom for the __postInit
            call. Use when the ctor body issues inner txns (e.g. spawning
            child apps with `new ChildContract()`).
        """
        artifacts = self.compile(
            sol_path,
            ensure_budget=ensure_budget,
            via_yul_behavior=via_yul_behavior,
            evm_version=evm_version,
            extra_args=extra_args,
        )
        return self.deploy(
            artifacts,
            contract_name,
            ctor_args=ctor_args,
            fund_wei=fund_wei,
            postinit_args=postinit_args,
            postinit_budget_pool=postinit_budget_pool,
            postinit_inner_txns=postinit_inner_txns,
        )

    def call(
        self,
        app: App,
        sig: str,
        *args: Any,
        **opts,
    ) -> Result:
        """Call a method on the deployed app. See call.call() for options."""
        return _call(self.localnet, app, sig, args, **opts)

    def call_raw(
        self,
        app: App,
        selector: bytes | None,
        **opts,
    ) -> Result:
        """Submit a raw 4-byte selector call (skip ABI dispatch).

        See call.call_raw() for keyword options. Use for fallback /
        allowNonExistingFunctions-style tests where the test wants to
        verify the router's behaviour on an unknown selector.

        `selector=None` makes a bare call with NumAppArgs==0 — exercises
        Solidity's `receive()`/`fallback()` entry path.
        """
        return _call_raw(self.localnet, app, selector, **opts)

    def call_bare(self, app: App, **opts) -> Result:
        """Bare app call (NumAppArgs==0) that triggers receive()/fallback()."""
        return _call_raw(self.localnet, app, None, **opts)

    def cleanup(self) -> None:
        """Remove isolated compile outputs while preserving legacy tracked files."""
        for compile_dir in self._compile_dirs:
            shutil.rmtree(compile_dir, ignore_errors=True)
