#!/usr/bin/env python3
"""Joint local-EVM leg for the CCTP historical replay.

  ../WIP/tiny-fuzzing-oracle/.evmvenv/bin/python cctp_evm_leg.py cases \
      --output cases/cctp_evm_results.json

Deploys MessageTransmitter, TokenMessenger, TokenMinter and the StubERC20 in
ONE py-evm chain and replays the same merged historical stream the AVM oracle
leg runs, so cross-contract calls execute as real EVM calls.

Fidelity decisions (mirroring oracle_cctp_historical.py exactly):
  * RAW historical addresses everywhere — senders execute via py-evm
    SpoofTransaction (no keys), and address arguments pass through untranslated
    EXCEPT the four tracked contract addresses, which map to this leg's
    deployed addresses. Raw addresses are load-bearing: receiveMessage's
    ecrecover recovers the REAL Circle attester, so enableAttester must store
    the historical address bytes.
  * StubERC20 stands in for USDC with the same per-successful-deposit
    mint+approve reconstruction, so its state is synthetic on BOTH legs.
  * Statuses compare against historical receipts (with the same metadata
    corrections); storage is reported as a slot→word map per contract from
    traced SSTOREs — the AVM leg's --evm-storage-layout boxes use the same
    keccak slot space, so the maps compare slot-for-slot.

Output: {statuses: {hash: bool}, logs: {hash: [...]}, storage: {tag: {slot: word}},
         addresses: {tag: local}, steps: [...]}
"""

from __future__ import annotations

import argparse
import json
import runpy
import shutil
import sys
from pathlib import Path
from typing import Any

HERE = Path(__file__).parent
DRIVER = runpy.run_path(str(HERE / "oracle_cctp_historical.py"))

CASE_CONFIG = DRIVER["CASE_CONFIG"]
STUB_CONFIG = DRIVER["STUB_CONFIG"]
CaseData = DRIVER["CaseData"]
historical_stream = DRIVER["historical_stream"]
historical_ok = DRIVER["historical_ok"]

TRACKED = {cfg["address"].lower(): tag for tag, cfg in CASE_CONFIG.items()}
TRACKED[STUB_CONFIG["address"].lower()] = "stub_usdc"

GAS = 15_000_000
GAS_PRICE = 10**9


def compile_unit(
    path: Path,
    extra_sources: list[Path] | None = None,
    import_dir: Path | None = None,
    remappings: list[str] | None = None,
) -> dict[str, dict[str, Any]]:
    import solcx

    solcx.set_solc_version("0.8.26")
    kwargs: dict[str, Any] = {}
    if import_dir is not None:
        kwargs["allow_paths"] = str(import_dir)
        kwargs["base_path"] = str(import_dir)
    if remappings:
        kwargs["import_remappings"] = list(remappings)
    return solcx.compile_files(
        [str(path), *[str(p) for p in (extra_sources or [])]],
        output_values=["abi", "bin"],
        optimize=True,
        optimize_runs=200,
        via_ir=False,
        **kwargs,
    )


def link_placeholders(unit: dict[str, dict[str, Any]]) -> dict[str, str]:
    """solc link placeholder (34-hex keccak prefix) → fully-qualified name."""
    from Crypto.Hash import keccak as ck

    return {
        ck.new(digest_bits=256, data=str(key).encode()).hexdigest()[:34]: str(key)
        for key in unit
    }


class SstoreTrace:
    """Patch AccountDB.set_storage — the per-contract lane's proven trick.

    Over-approximates (a write in a later-reverted frame is journalled away
    but still traced): safe direction, can only widen the compared slot set.
    """

    def __init__(self) -> None:
        self.writes: dict[bytes, set[int]] = {}
        self._orig = None
        self._armed = False

    def install(self) -> None:
        from eth.db.account import AccountDB

        trace = self

        self._orig = AccountDB.set_storage

        def patched(db, address, slot, value):
            if trace._armed:
                trace.writes.setdefault(bytes(address), set()).add(int(slot))
            return trace._orig(db, address, slot, value)

        AccountDB.set_storage = patched

    def armed(self, flag: bool) -> None:
        self._armed = flag


class JointEvmLeg:
    def __init__(self, cases_dir: Path):
        self.cases = {tag: CaseData.load(cases_dir, tag) for tag in CASE_CONFIG}
        self.cases_dir = cases_dir
        self.local: dict[str, bytes] = {}  # historical addr (0x..) -> local 20B
        self.lib_addresses: dict[str, bytes] = {}  # fq library name -> local 20B
        self.abis: dict[str, list] = {}  # tag -> abi
        self.trace = SstoreTrace()
        self.steps: list[dict[str, Any]] = []
        self.statuses: dict[str, bool] = {}
        self.logs: dict[str, list[dict[str, Any]]] = {}
        # Mid-history upgrades: runtime code + ABI per DRIVER["UPGRADES"]
        # entry, harvested by a phase-A scratch deploy (ctor runs, so
        # immutables are baked into the harvested code).
        self.upgrade_code: list[bytes] = []
        self.upgrade_abi: list[list] = []

    # ── chain setup ──────────────────────────────────────────────────────
    def all_senders(self) -> set[bytes]:
        """Every address that will ever send: creators + per-call senders.

        Spoofed transactions skip signatures but py-evm's balance/nonce checks
        are real, so senders are prefunded at GENESIS (a later get_vm() state
        write does not persist into apply_transaction's state).
        """
        out: set[bytes] = set()
        for data in self.cases.values():
            out.add(self.to_local20(data.registry["creator"]))
        # The StubERC20's creator comes from its own case, which is NOT in
        # self.cases under a v2 config (the stub reuses the v1 minter's).
        stub_tag = DRIVER["STUB_SOURCE"]["tag"]
        stub_registry = json.loads(
            (self.cases_dir / stub_tag / "registry.json").read_text()
        )
        out.add(self.to_local20(stub_registry["creator"]))
        for item in historical_stream(self.cases):
            if item["kind"] != "call":
                continue
            data = self.cases[item["tag"]]
            marker = (item["call"].get("sender") or {}).get("__addr__")
            raw = (
                data.raw_address(marker)
                if marker is not None
                else item["txn"]["from"].lower()
            )
            out.add(self.to_local20(raw))
        for entry in DRIVER["UPGRADES"]:
            if entry.get("sender"):
                out.add(self.to_local20(entry["sender"]))
        return out

    def build_chain(self) -> None:
        from eth_tester import EthereumTester, PyEVMBackend
        from eth_tester.backends.pyevm.main import (
            get_default_genesis_params,
        )

        first = min(
            int(d.case["creation"]["ts"]) for d in self.cases.values()
        )
        genesis = get_default_genesis_params(
            {"timestamp": max(61, first) - 60, "gas_limit": 60_000_000}
        )
        genesis_state = {
            addr: {
                "balance": 10**24,
                "nonce": 0,
                "code": b"",
                "storage": {},
            }
            for addr in self.all_senders()
        }
        backend = PyEVMBackend(
            genesis_parameters=genesis, genesis_state=genesis_state
        )
        self.backend = backend
        self.chain = backend.chain
        self.trace.install()

    def fund(self, addr20: bytes) -> None:
        # All senders are prefunded at genesis; nothing to do per call.
        return None

    def execute(
        self,
        sender20: bytes,
        to20: bytes | None,
        data: bytes,
        label: str,
    ) -> tuple[bool, Any, bytes | None]:
        """Spoofed apply; returns (ok, computation, created_address)."""
        from eth.vm.spoof import SpoofTransaction

        self.fund(sender20)
        vm = self.chain.get_vm()
        nonce = vm.state.get_nonce(sender20)
        builder = vm.get_transaction_builder()
        fields = dict(
            nonce=nonce,
            gas_price=GAS_PRICE,
            gas=GAS,
            to=b"" if to20 is None else to20,
            value=0,
            data=data,
        )
        # Spoof the SIGNED class (it owns encode/make_receipt/hash) with a junk
        # signature: SpoofAttributes checks overrides before delegation, so a
        # `sender` override shadows signature recovery entirely, and the from_
        # guard only fires on the literal from_ key.
        twin = builder.new_transaction(v=27, r=1, s=1, **fields)
        spoofed = SpoofTransaction(
            twin,
            sender=sender20,
            get_sender=lambda: sender20,
            check_signature_validity=lambda: None,
        )
        self.trace.armed(True)
        try:
            _block, _receipt, computation = self.chain.apply_transaction(spoofed)
        finally:
            self.trace.armed(False)
        # One block per transaction: keeps each txn under the block gas limit
        # and mirrors LocalNet's block-per-txn cadence.
        self.chain.mine_block()
        created = None
        if to20 is None and not computation.is_error:
            created = bytes(computation.msg.storage_address)
        ok = not computation.is_error
        self.steps.append(
            {
                "name": label,
                "ok": ok,
                "error": repr(computation.error) if computation.is_error else None,
            }
        )
        return ok, computation, created

    # ── address + arg resolution (mirror of the AVM leg's resolve()) ─────
    def to_local20(self, address: str) -> bytes:
        """Identity: contracts are re-homed at their HISTORICAL addresses
        before any call replays, so every address — senders, arguments, and
        ctor references to sibling contracts — is the raw historical bytes.
        (Phase-A ctor args must ALSO use the final historical homes, which is
        why this never consults the scratch deployment addresses.)"""
        low = address.lower()
        return bytes.fromhex(low[2:] if low.startswith("0x") else low)

    def resolve_arg(self, data: CaseData, spec_type: str, value: Any) -> Any:
        if isinstance(value, dict) and set(value) == {"__addr__"}:
            addr = data.raw_address(value["__addr__"])
            return "0x" + self.to_local20(addr).hex()
        if isinstance(value, dict) and set(value) == {"__dep__"}:
            return "0x" + self.to_local20(value["__dep__"]).hex()
        if isinstance(value, dict) and set(value) == {"__b__"}:
            return bytes.fromhex(value["__b__"])
        if isinstance(value, list):
            return [self.resolve_arg(data, spec_type, item) for item in value]
        return value

    def encode_call(
        self, abi: list, signature: str, data: CaseData, args: list[Any]
    ) -> bytes:
        from eth_abi import encode
        from eth_utils import keccak

        name, types_str = signature.split("(", 1)
        types = types_str.rstrip(")")
        type_list = [] if types == "" else self._split_types(types)
        selector = keccak(text=signature)[:4]
        resolved = [
            self._coerce(t, self.resolve_arg(data, t, v))
            for t, v in zip(type_list, args or [])
        ]
        return selector + (encode(type_list, resolved) if type_list else b"")

    @staticmethod
    def _split_types(types: str) -> list[str]:
        out, depth, cur = [], 0, ""
        for ch in types:
            if ch == "," and depth == 0:
                out.append(cur)
                cur = ""
                continue
            if ch == "(":
                depth += 1
            if ch == ")":
                depth -= 1
            cur += ch
        if cur:
            out.append(cur)
        return out

    @staticmethod
    def _coerce(abi_type: str, value: Any) -> Any:
        if abi_type == "address":
            if isinstance(value, (bytes, bytearray)):
                return "0x" + bytes(value)[-20:].hex()
            return value
        if abi_type.startswith(("uint", "int")) and isinstance(value, str):
            return int(value)
        if abi_type.startswith("bytes") and isinstance(value, str):
            v = value[2:] if value.startswith("0x") else value
            return bytes.fromhex(v)
        return value

    # ── deployment (mirror of deploy_dependency/deploy_case) ─────────────
    def resolve_bin(
        self,
        unit: dict[str, dict[str, Any]],
        fq_name: str,
        deployer: bytes,
    ) -> bytes:
        """Link a contract's bin, deploying referenced libraries on demand."""
        import re

        raw = unit[fq_name]["bin"]
        names = link_placeholders(unit)
        for marker in set(re.findall(r"__\$([0-9a-f]{34})\$__", raw)):
            lib_fq = names.get(marker)
            if lib_fq is None:
                raise RuntimeError(f"unknown link placeholder {marker} in {fq_name}")
            if lib_fq not in self.lib_addresses:
                lib_bin = self.resolve_bin(unit, lib_fq, deployer)
                ok, comp, created = self.execute(
                    deployer, None, lib_bin, f"create library {lib_fq}"
                )
                if not ok or not created:
                    raise RuntimeError(f"library deploy failed: {lib_fq}")
                self.lib_addresses[lib_fq] = created
            raw = raw.replace(
                f"__${marker}$__", self.lib_addresses[lib_fq].hex()
            )
        return bytes.fromhex(raw)

    PRE08_TMV = "uint8 bitLength = _bytes * 8;"
    PRE08_TMV_FIX = (
        "uint8 bitLength; unchecked { bitLength = uint8(uint256(_bytes) * 8); }"
        " // pre-0.8 compat: uint8(32*8)==0 asks leftMask for all 256 bits"
    )

    # v2 patches, byte-for-byte the same edits build_v2_avm.py makes to the
    # AVM tree — both legs must run identical semantics.
    P1_MARK = "if (msg.sender != tx.origin) {"
    P2_MARK = "_disableInitializers();"

    def patch_tree(self, tree: Path) -> None:
        """Apply P1/P2 in place on a disposable copy of a multi-file tree."""
        for path in sorted(tree.rglob("*.sol")):
            text = path.read_text()
            changed = text
            if self.P1_MARK in changed:
                lines = changed.splitlines(keepends=True)
                out, i = [], 0
                while i < len(lines):
                    if self.P1_MARK in lines[i]:
                        depth = lines[i].count("{") - lines[i].count("}")
                        i += 1
                        while i < len(lines) and depth > 0:
                            depth += lines[i].count("{") - lines[i].count("}")
                            i += 1
                        self.steps.append(
                            {"name": f"P1 tx.origin: {path.name}", "ok": True,
                             "error": None}
                        )
                        continue
                    out.append(lines[i])
                    i += 1
                changed = "".join(out)
            if self.P2_MARK in changed:
                changed = changed.replace(self.P2_MARK, "")
                self.steps.append(
                    {"name": f"P2 initializers: {path.name}", "ok": True,
                     "error": None}
                )
            if self.PRE08_TMV in changed:
                changed = changed.replace(self.PRE08_TMV, self.PRE08_TMV_FIX)
                self.steps.append(
                    {"name": f"pre08 TMV wrap: {path.name}", "ok": True,
                     "error": None}
                )
            if changed != text:
                path.write_text(changed)

    def compile_case_tree(
        self, data: CaseData, contract: str
    ) -> tuple[dict[str, Any], dict[str, dict[str, Any]], str]:
        """Multi-file (v2) or single-file (v1) compile of a case's source."""
        import tempfile

        case = json.loads((data.path / "case.json").read_text())
        mf = case.get("multifile")
        tmp = Path(tempfile.mkdtemp(prefix=f"cctp-evm-{data.tag}-"))
        if mf:
            tree = tmp / "src"
            shutil.copytree(data.path / "src", tree)
            self.patch_tree(tree)
            unit = compile_unit(
                tree / mf["main"],
                extra_sources=[tree / f for f in mf["files"]],
                import_dir=tree,
                remappings=mf["remappings"],
            )
        else:
            tree = tmp
            tree.mkdir(exist_ok=True)
            (tree / "prepared.sol").write_text(
                (data.path / "prepared.sol").read_text()
            )
            self.patch_tree(tree)
            unit = compile_unit(tree / "prepared.sol")
        for key, art in unit.items():
            if str(key).endswith(f":{contract}"):
                return art, unit, str(key)
        raise RuntimeError(f"{contract} not found for {data.tag}")

    def compile_contract(
        self, path: Path, contract: str
    ) -> tuple[dict[str, Any], dict[str, dict[str, Any]], str]:
        """Single standalone file (the StubERC20 dependency)."""
        import tempfile

        tmp = Path(tempfile.mkdtemp(prefix="cctp-evm-")) / path.name
        tmp.write_text(path.read_text())
        self.patch_tree(tmp.parent)
        unit = compile_unit(tmp)
        for key, art in unit.items():
            if str(key).endswith(f":{contract}"):
                return art, unit, str(key)
        raise RuntimeError(f"{contract} not found in {path}")

    def deploy_stub(self) -> None:
        stub_tag = DRIVER["STUB_SOURCE"]["tag"]
        stub_sol = (
            self.cases_dir / stub_tag / "deps" / "argdep_a0b86991" / "prepared.sol"
        )
        art, unit, fq = self.compile_contract(stub_sol, "StubERC20")
        self.abis["stub_usdc"] = art["abi"]
        stub_registry = json.loads(
            (self.cases_dir / stub_tag / "registry.json").read_text()
        )
        creator = self.to_local20(stub_registry["creator"])
        # Kept for the re-homed genesis: under a v2 config the stub's case
        # (v1 minter) is not in self.cases, so its creator is not a sender.
        self.stub_creator = creator
        ok, _c, created = self.execute(
            creator, None, self.resolve_bin(unit, fq, creator), "create StubERC20"
        )
        if not ok or not created:
            raise RuntimeError("StubERC20 deploy failed")
        self.local[STUB_CONFIG["address"].lower()] = created

    def deploy_case(self, data: CaseData) -> None:
        art, unit, fq = self.compile_case_tree(data, data.config["contract"])
        self.abis[data.tag] = art["abi"]
        from eth_abi import encode

        ctor_types = []
        for item in art["abi"]:
            if item.get("type") == "constructor":
                ctor_types = [inp["type"] for inp in item.get("inputs", [])]
        raw_args = data.calls["meta"]["ctor_args"]
        resolved = [
            self._coerce(t, self.resolve_arg(data, t, v))
            for t, v in zip(ctor_types, raw_args)
        ]
        creator = self.to_local20(data.registry["creator"])
        payload = self.resolve_bin(unit, fq, creator) + (
            encode(ctor_types, resolved) if ctor_types else b""
        )
        ok, comp, created = self.execute(
            creator, None, payload, f"create {data.config['contract']}"
        )
        if not ok or not created:
            raise RuntimeError(f"{data.config['contract']} deploy failed: {comp.error}")
        self.local[data.config["address"].lower()] = created

    # ── mid-history upgrades (mirror of Runner.apply_upgrade) ────────────
    def compile_upgrade(self, entry: dict[str, Any]) -> tuple:
        """Compile an upgrade's source tree (multifile or prepared.sol)."""
        import tempfile

        src = self.cases_dir / entry["src"]
        mf = entry.get("multifile")
        tmp = Path(tempfile.mkdtemp(prefix="cctp-evm-upg-"))
        if mf:
            tree = tmp / "src"
            shutil.copytree(src, tree)
            self.patch_tree(tree)
            unit = compile_unit(
                tree / mf["main"],
                extra_sources=[tree / f for f in mf["files"]],
                import_dir=tree,
                remappings=mf["remappings"],
            )
        else:
            tree = tmp
            (tree / "prepared.sol").write_text((src / "prepared.sol").read_text())
            self.patch_tree(tree)
            unit = compile_unit(tree / "prepared.sol")
        for key, art in unit.items():
            if str(key).endswith(f":{entry['contract']}"):
                return art, unit, str(key)
        raise RuntimeError(f"{entry['contract']} not found in {src}")

    def harvest_upgrades(self) -> None:
        """Phase A: scratch-deploy each new implementation for its runtime
        code. The ctor runs (immutables bake into the code); its storage
        writes land at the scratch address, which the re-home never carries —
        exactly right, since a proxied implementation's own storage is dead."""
        from eth_abi import encode

        for entry in DRIVER["UPGRADES"]:
            data = self.cases[entry["tag"]]
            art, unit, fq = self.compile_upgrade(entry)
            self.upgrade_abi.append(art["abi"])
            ctor_types = []
            for item in art["abi"]:
                if item.get("type") == "constructor":
                    ctor_types = [inp["type"] for inp in item.get("inputs", [])]
            resolved = [
                self._coerce(t, self.resolve_arg(data, t, v))
                for t, v in zip(ctor_types, entry.get("ctor_args") or [])
            ]
            creator = self.to_local20(data.registry["creator"])
            payload = self.resolve_bin(unit, fq, creator) + (
                encode(ctor_types, resolved) if ctor_types else b""
            )
            ok, comp, created = self.execute(
                creator, None, payload, f"scratch-deploy upgrade {entry['contract']}"
            )
            if not ok or not created:
                raise RuntimeError(
                    f"upgrade {entry['contract']} scratch deploy failed: {comp.error}"
                )
            self.upgrade_code.append(self.chain.get_vm().state.get_code(created))

    def apply_upgrade(self, item: dict[str, Any]) -> None:
        """Swap the historical address's code at the upgrade block.

        py-evm state writes outside apply_transaction don't persist, so the
        proven mechanism is the re-home one: snapshot every account and
        rebuild the genesis with the new runtime code at the target — its
        storage (the proxy's, in EVM terms) carries over untouched."""
        from eth_tester import PyEVMBackend
        from eth_tester.backends.pyevm.main import get_default_genesis_params

        entry = item["upgrade"]
        data = self.cases[entry["tag"]]
        hist = data.config["address"].lower()
        accounts: dict[bytes, dict[str, Any]] = {}
        for h, local in self.local.items():
            accounts[bytes.fromhex(h[2:])] = self.snapshot_account(local)
        for lib20 in self.lib_addresses.values():
            accounts[lib20] = self.snapshot_account(lib20)
        accounts[bytes.fromhex(hist[2:])]["code"] = self.upgrade_code[
            item["upgrade_index"]
        ]
        funded = set(self.all_senders())
        funded.update(
            self.to_local20(d.registry["creator"]) for d in self.cases.values()
        )
        funded.add(self.stub_creator)
        for sender in funded:
            if sender not in accounts:
                accounts[sender] = {
                    "balance": 10**24,
                    "nonce": 0,
                    "code": b"",
                    "storage": {},
                }
            else:
                accounts[sender]["balance"] = 10**24
        genesis = get_default_genesis_params(
            {"timestamp": max(62, int(entry["ts"])) - 1, "gas_limit": 60_000_000}
        )
        self.backend = PyEVMBackend(
            genesis_parameters=genesis, genesis_state=accounts
        )
        self.chain = self.backend.chain
        self.steps.append(
            {
                "name": f"code swap {entry['contract']} at {hist} "
                f"(block {entry['block']})",
                "ok": True,
                "error": None,
            }
        )
        self.abis[data.tag] = self.upgrade_abi[item["upgrade_index"]]
        if entry.get("init_sig"):
            sender = self.to_local20(
                entry["sender"] if entry.get("sender") else data.registry["creator"]
            )
            payload = self.encode_call(
                self.abis[data.tag],
                entry["init_sig"],
                data,
                entry.get("init_args") or [],
            )
            ok, comp, _ = self.execute(
                sender,
                self.local[hist],
                payload,
                f"upgrade initialize {entry['contract']}",
            )
            if not ok:
                raise RuntimeError(
                    f"{entry['contract']} upgrade initialize failed: {comp.error}"
                )

    # ── USDC reconstruction (mirror of seed_usdc) ────────────────────────
    def seed_usdc(self, data: CaseData, call: dict[str, Any], sender20: bytes) -> None:
        amount = int(call["args"][0])
        target = data.raw_address(call["sender"]["__addr__"])
        stub = self.local[STUB_CONFIG["address"].lower()]
        # The spender is the messenger making this deposit — `data` IS that
        # case, so no literal tag lookup (v2 tags differ).
        messenger = self.local[data.config["address"].lower()]
        mint = self.encode_call(
            self.abis["stub_usdc"],
            "mint(address,uint256)",
            data,
            ["0x" + self.to_local20(target).hex(), amount],
        )
        ok, comp, _ = self.execute(sender20, stub, mint, f"usdc mint {call['hash']}")
        if not ok:
            raise RuntimeError(f"stub mint failed: {comp.error}")
        approve = self.encode_call(
            self.abis["stub_usdc"],
            "approve(address,uint256)",
            data,
            ["0x" + messenger.hex(), amount],
        )
        ok, comp, _ = self.execute(
            sender20, stub, approve, f"usdc approve {call['hash']}"
        )
        if not ok:
            raise RuntimeError(f"stub approve failed: {comp.error}")

    # ── replay ───────────────────────────────────────────────────────────
    def snapshot_account(self, addr20: bytes) -> dict[str, Any]:
        vm = self.chain.get_vm()
        storage = {}
        for slot in sorted(self.trace.writes.get(bytes(addr20), set())):
            value = vm.state.get_storage(addr20, slot)
            if value:
                storage[slot] = value
        return {
            "balance": 0,
            "nonce": 1,
            "code": vm.state.get_code(addr20),
            "storage": storage,
        }

    def rehome_to_historical(self) -> None:
        """Rebuild the chain with every contract AT its historical address.

        CCTP messages embed the recipient contract's HISTORICAL address in the
        signed bytes; the AVM leg resolves those via app_id = low64(address).
        The EVM mirror is placement: snapshot each deployed account (runtime
        code + ctor storage) and seed a fresh genesis that homes tracked
        contracts at their historical addresses (libraries keep their scratch
        address — it is baked into linked code). Translation then collapses
        to the identity.
        """
        from eth_tester import PyEVMBackend
        from eth_tester.backends.pyevm.main import get_default_genesis_params

        accounts: dict[bytes, dict[str, Any]] = {}
        for hist, local in list(self.local.items()):
            hist20 = bytes.fromhex(hist[2:])
            accounts[hist20] = self.snapshot_account(local)
        for _fq, lib20 in self.lib_addresses.items():
            accounts[lib20] = self.snapshot_account(lib20)
        # Senders + creators (creators also send the config-era initialize
        # calls after re-homing, and a contract address that got re-homed onto
        # a creator's slot must keep its code, so contracts win the merge).
        funded = set(self.all_senders())
        funded.update(
            self.to_local20(d.registry["creator"]) for d in self.cases.values()
        )
        funded.add(self.stub_creator)
        for sender in funded:
            if sender not in accounts:
                accounts[sender] = {
                    "balance": 10**24,
                    "nonce": 0,
                    "code": b"",
                    "storage": {},
                }
            else:
                accounts[sender]["balance"] = 10**24

        first = min(int(d.case["creation"]["ts"]) for d in self.cases.values())
        genesis = get_default_genesis_params(
            {"timestamp": max(61, first) - 60, "gas_limit": 60_000_000}
        )
        backend = PyEVMBackend(
            genesis_parameters=genesis, genesis_state=accounts
        )
        self.backend = backend
        self.chain = backend.chain
        # Re-key the phase-A ctor-write trace onto the historical homes so the
        # final storage report covers ctor-only slots (maxMessageBodySize, the
        # ctor attester, its enabled flag) — clearing here made them read as
        # zero on this leg while the replay proved they were seeded.
        rehomed: dict[bytes, set[int]] = {}
        for hist, local in self.local.items():
            slots = self.trace.writes.get(bytes(local))
            if slots:
                rehomed[bytes.fromhex(hist[2:])] = set(slots)
        self.trace.writes = rehomed
        # Contracts now live at their historical addresses: identity mapping.
        self.local = {hist: bytes.fromhex(hist[2:]) for hist in self.local}

    def run(self, limit: int | None) -> dict[str, Any]:
        # Phase A: scratch chain — deployments only, to harvest runtime code
        # and constructor-written storage.
        self.build_chain()
        self.deploy_stub()
        for item in historical_stream(self.cases):
            if item["kind"] == "create":
                self.deploy_case(self.cases[item["tag"]])
        self.harvest_upgrades()
        # Phase B: re-home everything at historical addresses and replay.
        self.rehome_to_historical()
        # Config era (v2): replay each proxy's historical initialize calldata
        # against the directly-deployed implementation, exactly as the AVM
        # leg does after its deploy.
        for entry in DRIVER["INIT_CALLS"]:
            data = self.cases[entry["tag"]]
            target = self.local[data.config["address"].lower()]
            creator = self.to_local20(data.registry["creator"])
            payload = self.encode_call(
                self.abis[data.tag], entry["sig"], data, entry["args"]
            )
            ok, comp, _ = self.execute(
                creator, target, payload, f"initialize {data.config['contract']}"
            )
            if not ok:
                raise RuntimeError(
                    f"{data.config['contract']} initialize failed: {comp.error}"
                )
        replayed = 0
        for item in historical_stream(self.cases):
            if item["kind"] == "create":
                continue
            if limit is not None and replayed >= limit:
                break
            if item["kind"] == "upgrade":
                self.apply_upgrade(item)
                continue
            data = self.cases[item["tag"]]
            call = item["call"]
            sender_marker = (call.get("sender") or {}).get("__addr__")
            sender_raw = (
                data.raw_address(sender_marker)
                if sender_marker is not None
                else item["txn"]["from"].lower()
            )
            sender20 = self.to_local20(sender_raw)
            # Method-keyed, not tag-keyed: only the TokenMessenger declares
            # depositForBurn*, and v2's tags/arity differ.
            if call["sig"] and (
                call["sig"].startswith("depositForBurn") and historical_ok(call)
            ):
                self.seed_usdc(data, call, sender20)

            target = self.local[data.config["address"].lower()]
            if call.get("sig"):
                payload = self.encode_call(
                    self.abis[data.tag], call["sig"], data, call.get("args") or []
                )
            else:
                # Undecodable historical calldata (all such txns failed on
                # chain): replay the raw input bytes, mirroring the AVM leg's
                # empty-args dispatch failure.
                raw = (item["txn"].get("input") or "0x")
                payload = bytes.fromhex(raw[2:] if raw.startswith("0x") else raw)
            ok, comp, _ = self.execute(
                sender20, target, payload, f"historical {call['hash']}"
            )
            self.statuses[call["hash"]] = ok
            self.logs[call["hash"]] = self.collect_logs(comp)
            replayed += 1
        return self.report()

    def collect_logs(self, computation) -> list[dict[str, Any]]:
        out = []
        for address, topics, data in computation.get_log_entries():
            out.append(
                {
                    "address": "0x" + bytes(address).hex(),
                    "topics": ["0x%064x" % t for t in topics],
                    "data": "0x" + bytes(data).hex(),
                }
            )
        return out

    def report(self) -> dict[str, Any]:
        # Final storage: traced written slots, read back from the final state.
        vm = self.chain.get_vm()
        local_to_tag = {
            self.local[addr].hex(): tag
            for addr, tag in TRACKED.items()
            if addr in self.local
        }
        storage: dict[str, dict[str, str]] = {}
        for addr_bytes, slots in self.trace.writes.items():
            tag = local_to_tag.get(addr_bytes.hex())
            if tag is None:
                continue
            words = {}
            for slot in sorted(slots):
                value = vm.state.get_storage(addr_bytes, slot)
                if value:
                    words[str(slot)] = "%064x" % value
            storage[tag] = words
        return {
            "scope": "joint local-EVM CCTP replay (py-evm, spoofed raw senders)",
            "addresses": {
                tag: "0x" + self.local[addr].hex()
                for addr, tag in TRACKED.items()
                if addr in self.local
            },
            "statuses": self.statuses,
            "logs": self.logs,
            "storage": storage,
            "steps": self.steps,
        }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("cases", type=Path)
    parser.add_argument("--limit", type=int)
    parser.add_argument("--output", type=Path)
    parser.add_argument(
        "--config", type=Path, help="joint config JSON (e.g. joint_config_v2.json)"
    )
    args = parser.parse_args()
    if args.config:
        DRIVER["apply_joint_config"](json.loads(args.config.read_text()))
        TRACKED.clear()
        TRACKED.update(
            {cfg["address"].lower(): tag for tag, cfg in CASE_CONFIG.items()}
        )
        TRACKED[STUB_CONFIG["address"].lower()] = "stub_usdc"
    leg = JointEvmLeg(args.cases)
    report = leg.run(args.limit)
    from collections import Counter, defaultdict

    # Identical-payload relayer races (same rule as the AVM driver): compare
    # the OUTCOME MULTISET within byte-identical payload groups — the chain
    # accepted exactly one of the group, so does the replay; order is
    # gas/indexer-environmental.
    groups = defaultdict(list)
    matched, total, race_matched = 0, 0, 0
    for data in leg.cases.values():
        for call in data.calls["calls"]:
            h = call["hash"]
            if "#" in h or h not in report["statuses"]:
                continue
            total += 1
            ok = report["statuses"][h] == historical_ok(call)
            matched += ok
            if not ok and call.get("sig"):
                key = (
                    data.tag,
                    call["sig"],
                    json.dumps(call.get("args"), sort_keys=True),
                )
                groups[key].append(call)
    for key, rows in groups.items():
        data = leg.cases[key[0]]
        siblings = [
            c
            for c in data.calls["calls"]
            if c.get("sig") == key[1]
            and json.dumps(c.get("args"), sort_keys=True) == key[2]
        ]
        hist = Counter(bool(historical_ok(c)) for c in siblings)
        seen = Counter(bool(report["statuses"].get(c["hash"])) for c in siblings)
        if hist == seen:
            race_matched += len(rows)
    print(
        f"evm joint leg: {matched}/{total} statuses match history"
        + (f" (+{race_matched} identical-payload race reordered)" if race_matched else "")
    )
    if args.output:
        args.output.write_text(json.dumps(report))
    return 0


if __name__ == "__main__":
    sys.exit(main())
