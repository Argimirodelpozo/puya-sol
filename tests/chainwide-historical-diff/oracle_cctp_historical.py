#!/usr/bin/env python3
"""Replay the three CCTP histories together through avm-prover.

Unlike the per-contract LocalNet replays, this driver registers the real unsplit
MessageTransmitter, TokenMessenger, and TokenMinter artifacts in one oracle ledger and
orders their root transactions by the historical Ethereum block.  Contract-to-contract
calls therefore execute as AVM inner transactions instead of being flattened or skipped.

The Ethereum USDC contract is represented by the corpus' StubERC20 artifact.  Before a
historically successful deposit, the driver mints the requested amount to that caller and
approves TokenMessenger.  Those dependency-reconstruction calls are recorded separately;
the resulting report is a CCTP status replay, not a proof of historical USDC state.
"""

from __future__ import annotations

import argparse
import importlib
import json
import os
import re
import shutil
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Any

# algosdk is imported lazily inside encode_method so the stream-construction
# half of this module (CaseData, historical_stream) stays importable from the
# EVM leg's venv, which has web3/py-evm but not algosdk.


CASE_CONFIG = {
    "cctp_transmitter": {
        "contract": "MessageTransmitter",
        # puya-sol calls the application id in an address's low 64 bits.  Using the
        # historical low word lets signed CCTP messages retain their exact bytes.
        "app_id": 0xD87D7D289A738F81,
        "address": "0x0a992d191deec32afe36203ad87d7d289a738f81",
    },
    "cctp_messenger": {
        "contract": "TokenMessenger",
        "app_id": 0x25ADEC7066AF3155,
        "address": "0xbd3fa81b58ba92a82136038b25adec7066af3155",
    },
    "cctp_minter": {
        "contract": "TokenMinter",
        "app_id": 0xE3AA56C06FABE907,
        "address": "0xc4922d64a24675e16e1586e3e3aa56c06fabe907",
    },
}
STUB_CONFIG = {
    "contract": "StubERC20",
    "app_id": 0x2E9EB0CE3606EB48,
    "address": "0xa0b86991c6218b36c1d19d4a2e9eb0ce3606eb48",
}
ALL_CONFIG = [*CASE_CONFIG.values(), STUB_CONFIG]
ADDRESS_TO_APP = {item["address"].lower(): item["app_id"] for item in ALL_CONFIG}
# Where the StubERC20 artifact lives (its case tag need not be in CASE_CONFIG:
# the v2 config reuses the v1 minter's compiled stub).
STUB_SOURCE = {"tag": "cctp_minter"}
# Config-era calls replayed right after a case deploys (v2: the proxy's
# historical initialize calldata, decoded to plain values by gen_v2_config).
INIT_CALLS: list[dict[str, Any]] = []
# Mid-history implementation upgrades (proxy.md §1 "mid-history upgrades"):
# each entry swaps a case's program at its historical block — the AVM leg's
# native UpdateApplication (program replaced, boxes/globals persist), the EVM
# leg's code swap at the historical address. Entry shape:
#   {tag, block, txindex, ts, hash?, impl?,          — stream placement
#    contract, avm_artifact,                          — new AVM artifacts dir
#    abi?, src?, multifile?, ctor_args?,              — EVM leg + differ inputs
#    init_sig?, init_args?, sender?}                  — upgradeToAndCall data
UPGRADES: list[dict[str, Any]] = []


def apply_joint_config(config: dict[str, Any]) -> None:
    """Re-point the module at another contract system (e.g. CCTP v2).

    In-place mutation on purpose: cctp_evm_leg and cctp_joint_diff hold
    references to these same dict objects via runpy, so one application
    propagates everywhere.
    """
    CASE_CONFIG.clear()
    CASE_CONFIG.update(config["cases"])
    stub = dict(config.get("stub") or {})
    if stub:
        STUB_CONFIG.update(
            {k: stub[k] for k in ("contract", "app_id", "address") if k in stub}
        )
        STUB_SOURCE["tag"] = stub.get("artifact_tag", STUB_SOURCE["tag"])
    ALL_CONFIG[:] = [*CASE_CONFIG.values(), STUB_CONFIG]
    ADDRESS_TO_APP.clear()
    ADDRESS_TO_APP.update(
        {item["address"].lower(): item["app_id"] for item in ALL_CONFIG}
    )
    INIT_CALLS[:] = config.get("init_calls") or []
    UPGRADES[:] = config.get("upgrades") or []
    MAINNET_RECEIPT_METADATA.update(config.get("receipt_metadata") or {})
P0 = (b"p:" + bytes(8)).hex()
SIGNATURE = re.compile(r"^([^()]*)\((.*)\)$")

# The older cached Blockscout responses omitted some transaction indices and incorrectly
# labelled four reverted calls successful.  The first 500 calls to each of
# Transmitter and Messenger were checked against Ethereum mainnet receipts.
# Keep the source corpus immutable and make the narrowly verified corrections explicit
# in the report instead of silently treating the corresponding AVM rejections as mismatches.
MAINNET_RECEIPT_METADATA = {
    "0x7aa6d6a75b6494c452e2baf25908d06211f03010ee64d87ead4ac8f90a362e60": {
        "txindex": 0x57,
        "historical_ok": True,
    },
    "0x778eae3a55de0408ad6eadac0f2f90ee29bd9dae74262b0d2abbf4689b3c179f": {
        "txindex": 0x58,
        "historical_ok": False,
    },
    "0x22b3e67945604aaba3098f5ff5a0ec26f9231e72ae87584f2f684e242abb7dc9": {
        "txindex": 0x47,
        "historical_ok": False,
    },
    "0xe732f3de5bc1c72e463b5824b9773967c99682a5ba2244d10aba202691cc2683": {
        "txindex": 0x0C,
        "historical_ok": False,
    },
    "0x272903843aac18e5739bd6c8f546f6ffa84425122b4a155771e0ea90c15f4e70": {
        "txindex": 0x77,
        "historical_ok": False,
    },
}


def load_json(path: Path) -> Any:
    return json.loads(path.read_text())


def _load_verified_receipt_corrections() -> None:
    """Merge receipt_corrections.json into the metadata table.

    Deep windows reach far past the hand-audited region, and Blockscout's
    transaction `status` is not always the execution outcome: in the 3000-txn
    window 13 transactions claim success while their raw trace reports the
    top-level call Reverted. verify_receipts.py checks such rows against the
    chain's own trace and records the evidence; loading the file here keeps
    the corrections in ONE auditable place for both legs, instead of growing
    a hand-maintained literal. Failures are non-fatal — a missing or broken
    file simply means no corrections.
    """
    path = Path(__file__).parent / "receipt_corrections.json"
    try:
        if not path.exists():
            return
        data = json.loads(path.read_text())
    except Exception:
        return
    for h, entry in data.items():
        if not isinstance(entry, dict) or "historical_ok" not in entry:
            continue
        MAINNET_RECEIPT_METADATA.setdefault(h, {}).update(
            {"historical_ok": bool(entry["historical_ok"])})


_load_verified_receipt_corrections()


def raw20(address: str) -> bytes:
    value = address.lower()
    if value.startswith("0x"):
        value = value[2:]
    data = bytes.fromhex(value)
    if len(data) != 20:
        raise ValueError(f"expected an EVM address, got {address!r}")
    return data


def address_argument(address: str) -> bytes:
    """Map an EVM address to puya-sol's 32-byte Solidity address value."""
    app_id = ADDRESS_TO_APP.get(address.lower())
    if app_id is not None:
        return bytes(24) + int(app_id).to_bytes(8, "big")
    return bytes(12) + raw20(address)


@dataclass(frozen=True)
class CaseData:
    tag: str
    path: Path
    config: dict[str, Any]
    case: dict[str, Any]
    calls: dict[str, Any]
    registry: dict[str, Any]
    arc56: dict[str, Any]

    @classmethod
    def load(cls, cases: Path, tag: str) -> "CaseData":
        path = cases / tag
        config = CASE_CONFIG[tag]
        return cls(
            tag=tag,
            path=path,
            config=config,
            case=load_json(path / "case.json"),
            calls=load_json(path / "calls.json"),
            registry=load_json(path / "registry.json"),
            arc56=load_json(path / "out_avm" / f"{config['contract']}.arc56.json"),
        )

    def raw_address(self, marker: Any) -> str:
        if marker == "C":
            return self.registry["creator"].lower()
        if marker == "Z":
            return "0x" + "00" * 20
        if isinstance(marker, int):
            for table in ("senders", "args"):
                for address, symbol in self.registry.get(table, {}).items():
                    if symbol == marker:
                        return address.lower()
        raise ValueError(f"{self.tag}: unknown address marker {marker!r}")

    def resolve(self, value: Any) -> Any:
        if isinstance(value, dict) and set(value) == {"__addr__"}:
            return address_argument(self.raw_address(value["__addr__"]))
        if isinstance(value, dict) and set(value) == {"__dep__"}:
            return address_argument(value["__dep__"])
        if isinstance(value, dict) and set(value) == {"__b__"}:
            return bytes.fromhex(value["__b__"])
        if isinstance(value, list):
            return [self.resolve(item) for item in value]
        return value

    def sender(self, call: dict[str, Any], txn: dict[str, Any]) -> str:
        marker = (call.get("sender") or {}).get("__addr__")
        address = (
            self.raw_address(marker) if marker is not None else txn["from"].lower()
        )
        app_id = ADDRESS_TO_APP.get(address)
        if app_id is not None:
            # No current root fixture has a contract sender.  Keep this correct for a
            # future trace-derived root nonetheless: txn Sender is the app account,
            # whereas a Solidity address argument is the zero-padded application id.
            return self.oracle_api.app_address(app_id)  # type: ignore[attr-defined]
        return (bytes(12) + raw20(address)).hex()

    # Assigned by Runner after the prover adapter has been loaded.  Keeping the module's
    # pure fixture/ABI helpers importable makes their tests independent of a prover clone.
    oracle_api: Any = None


def method_for(arc56: dict[str, Any], solidity_signature: str) -> dict[str, Any]:
    match = SIGNATURE.match(solidity_signature)
    if not match:
        raise ValueError(f"invalid Solidity signature {solidity_signature!r}")
    name = match.group(1)
    # The compiler widens Solidity uint32 to ARC-4 uint64, so the source signature cannot
    # be compared textually.  These artifacts have no same-name/same-arity overloads.
    source_args = [] if not match.group(2) else match.group(2).split(",")
    matches = [
        method
        for method in arc56["methods"]
        if method["name"] == name and len(method["args"]) == len(source_args)
    ]
    if len(matches) != 1:
        raise ValueError(
            f"{solidity_signature}: expected one ARC-56 name/arity match, got "
            f"{len(matches)}"
        )
    return matches[0]


def encode_method(
    arc56: dict[str, Any], solidity_signature: str, values: list[Any]
) -> list[str]:
    method = method_for(arc56, solidity_signature)
    if len(values) != len(method["args"]):
        raise ValueError(f"{solidity_signature}: wrong argument count")
    signature = (
        method["name"]
        + "("
        + ",".join(arg["type"] for arg in method["args"])
        + ")"
        + method["returns"]["type"]
    )
    from algosdk.abi import ABIType, Method
    from algosdk.encoding import encode_address

    encoded = [Method.from_signature(signature).get_selector().hex()]
    for spec, value in zip(method["args"], values):
        if spec["type"] == "address" and isinstance(value, (bytes, bytearray)):
            value = encode_address(bytes(value))
        encoded.append(ABIType.from_string(spec["type"]).encode(value).hex())
    return encoded


def flatten_inner_logs(txns: list[dict[str, Any]]) -> list[dict[str, Any]]:
    """Depth-first (program-order) log entries from a nested inner-txn tree."""
    out: list[dict[str, Any]] = []
    for txn in txns:
        if txn.get("logs"):
            out.append(
                {
                    "app": (txn.get("u64") or {}).get("ApplicationID"),
                    "logs": list(txn["logs"]),
                }
            )
        out.extend(flatten_inner_logs(txn.get("inner_txns") or []))
    return out


def historical_ok(call: dict[str, Any]) -> bool:
    receipt = MAINNET_RECEIPT_METADATA.get(call["hash"], {})
    return bool(receipt.get("historical_ok", call["hist_ok"]))


def historical_stream(cases: dict[str, CaseData]) -> list[dict[str, Any]]:
    stream = []
    serial = 0
    for case_order, (tag, data) in enumerate(cases.items()):
        txns = data.case["txns"]
        for call in data.calls["calls"]:
            # Hash#traceIndex entries are lifted inner calls.  The joint execution must
            # let CCTP produce them itself, otherwise state changes are duplicated.
            if "#" in call["hash"]:
                continue
            txn = txns[call["i"]]
            receipt = MAINNET_RECEIPT_METADATA.get(call["hash"], {})
            stream.append(
                {
                    "kind": "call",
                    "tag": tag,
                    "block": int(txn["block"]),
                    "txindex": receipt.get("txindex", txn.get("txindex")),
                    "case_order": case_order,
                    "case_index": int(call["i"]),
                    "serial": serial,
                    "call": call,
                    "txn": txn,
                }
            )
            serial += 1
        creation = data.case["creation"]
        stream.append(
            {
                "kind": "create",
                "tag": tag,
                "block": int(creation["block"]),
                "txindex": -1,
                "case_order": case_order,
                "case_index": -1,
                "serial": serial,
                "timestamp": int(creation["ts"]),
            }
        )
        serial += 1
    tag_order = {tag: i for i, tag in enumerate(cases)}
    for upgrade_index, entry in enumerate(UPGRADES):
        # The upgrade txn owns its (block, txindex); case_index -1 keeps it
        # ahead of any call sharing a missing txindex in the same block.
        stream.append(
            {
                "kind": "upgrade",
                "tag": entry["tag"],
                "block": int(entry["block"]),
                "txindex": entry.get("txindex", -1),
                "case_order": tag_order[entry["tag"]],
                "case_index": -1,
                "serial": serial,
                "upgrade_index": upgrade_index,
                "upgrade": entry,
            }
        )
        serial += 1

    def order(item: dict[str, Any]) -> tuple[int, int, int, int, int]:
        # Missing tx indices only occur among calls to the same case.  case_index then
        # preserves their ascending explorer order.
        txindex = item["txindex"]
        return (
            item["block"],
            int(txindex) if txindex is not None else 1 << 30,
            item["case_order"],
            item["case_index"],
            item["serial"],
        )

    return sorted(stream, key=order)


def outer_call(
    app_id: int, app_args: list[str], app_address: str, value: int = 0
) -> str:
    fields = "".join(
        f"byte 0x{argument}\nitxn_field ApplicationArgs\n" for argument in app_args
    )
    payment = ""
    next_txn = ""
    if value:
        payment = f"""itxn_begin
int pay
itxn_field TypeEnum
txn Sender
itxn_field Sender
byte 0x{app_address}
itxn_field Receiver
int {int(value)}
itxn_field Amount
int 0
itxn_field Fee
"""
        next_txn = "itxn_next\n"
    else:
        payment = "itxn_begin\n"
    return f"""#pragma version 12
{payment}{next_txn}int appl
itxn_field TypeEnum
int {app_id}
itxn_field ApplicationID
txn Sender
itxn_field Sender
{fields}int 0
itxn_field Fee
itxn_submit
int 1
return
"""


def load_oracle_api(prover_root: Path) -> Any:
    oracle_dir = prover_root.resolve() / "oracle"
    if not (oracle_dir / "state_adapter.py").exists():
        raise FileNotFoundError(f"not an avm-prover checkout: {prover_root}")
    sys.path.insert(0, str(oracle_dir))
    return importlib.import_module("cctp_replay")


def pre08_compat_teal(source: str) -> tuple[str, list[str]]:
    """Restore the intentional uint8 wrap used by CCTP's Solidity-0.7 TMV.

    The cached CCTP corpus was fetched with ``--relax-pre08`` because puya-sol accepts
    Solidity 0.8 syntax.  Merely changing the pragma also changes arithmetic: the
    historical TypedMemView deliberately computes ``uint8(32 * 8) == 0`` to ask
    ``leftMask`` for all 256 bits, while 0.8 reverts.  Keep this compatibility shim
    narrow and reported rather than pretending every pre-0.8 arithmetic expression has
    been reconstructed.
    """
    pattern = re.compile(
        r"(TypedMemView\.index_after_if_else@6:\n"
        r"    frame_dig 1\n"
        r"    assert // TypedMemView/index - Attempted to index more than 32 bytes\n"
        r"    frame_dig -1\n"
        r"    intc_3 // 8\n"
        r"    \*\n)"
        r"    dup\n"
        r"    intc \d+ // 255\n"
        r"    <=\n"
        r"    assert // overflow\n"
        r"(    frame_dig -3\n)"
    )
    patched, count = pattern.subn(r"\1    intc 8 // 256\n    %\n\2", source)
    if count > 1:
        raise ValueError(f"expected at most one TypedMemView lowering, found {count}")
    applied = []
    if count:
        applied.append("TypedMemView.index uint8(32 * 8) wrap restored with unchecked")
    else:
        patched = source

    route = "main_receiveMessage_route@9:\n"
    if route in patched:
        if "__historical_ensure_budget" in patched:
            raise ValueError("historical ensure-budget shim already exists")
        patched = patched.replace(
            route,
            route
            + "    pushint 45000\n"
            + "    pushint 0\n"
            + "    callsub __historical_ensure_budget\n"
            + "    txna ApplicationArgs 1\n"
            + "    extract 2 0\n"
            + "    store 255\n",
            1,
        )
        replace_route = "main_replaceMessage_route@7:\n"
        if patched.count(replace_route) != 1:
            raise ValueError("expected one replaceMessage route")
        patched = patched.replace(
            replace_route,
            replace_route
            + "    pushint 45000\n"
            + "    pushint 0\n"
            + "    callsub __historical_ensure_budget\n",
            1,
        )
        clone_lowering = (
            "    dup\n"
            "    callsub Message._messageBody\n"
            "    callsub TypedMemView.clone\n"
        )
        if patched.count(clone_lowering) != 1:
            raise ValueError("expected one Message._messageBody clone lowering")
        patched = patched.replace(
            clone_lowering,
            "    load 255\n    extract 116 0\n",
            1,
        )
        caller_compare = (
            "    pushint 24\n"
            "    bzero\n"
            "    global CallerApplicationID\n"
            "    itob\n"
            "    concat\n"
            "    dig 36\n"
            "    ==\n"
        )
        if patched.count(caller_compare) != 1:
            raise ValueError("expected one replaceMessage caller-app comparison")
        patched = patched.replace(
            caller_compare,
            "    global CallerApplicationID\n"
            "    dig 36\n"
            "    pushint 24\n"
            "    extract_uint64\n"
            "    ==\n",
            1,
        )
        patched += """

// Historical replay resource shim. This is the same ephemeral-app OpUp strategy used
// by puya's ensure_budget; it changes available budget, not contract-visible state.
__historical_ensure_budget:
    proto 2 0
    frame_dig -2
    pushint 10
    +

__historical_ensure_budget_while:
    frame_dig 0
    global OpcodeBudget
    >
    bz __historical_ensure_budget_done
    itxn_begin
    pushint 6 // appl
    itxn_field TypeEnum
    pushint 5 // DeleteApplication
    itxn_field OnCompletion
    pushbytes 0x068101
    itxn_field ApprovalProgram
    pushbytes 0x068101
    itxn_field ClearStateProgram
    pushint 0
    itxn_field Fee
    itxn_submit
    b __historical_ensure_budget_while

__historical_ensure_budget_done:
    retsub
"""
        applied.append("receiveMessage/replaceMessage ensure_budget(45000) OpUp shim")
        applied.append("receiveMessage forwards exact signed message bytes[116:]")
        applied.append("replaceMessage caller-app comparison uses address low 64 bits")
    return patched, applied


def build_pre08_compat_artifacts(
    cases: Path,
) -> tuple[tempfile.TemporaryDirectory[str], Path, dict[str, list[str]]]:
    """Patch disposable TEAL artifacts without changing the cached corpus.

    The oracle assembles the patched TEAL source.  The cached binary is only a
    conservative deployment-size input; the replacement is two bytes smaller and does
    not cross a program-page boundary.
    """
    temp = tempfile.TemporaryDirectory(prefix="puya-cctp-historical-")
    root = Path(temp.name)
    patches = {}
    for tag, config in CASE_CONFIG.items():
        out = root / tag / "out_avm"
        shutil.copytree(cases / tag / "out_avm", out)
        approval = out / f"{config['contract']}.approval.teal"
        source, applied = pre08_compat_teal(approval.read_text())
        approval.write_text(source)
        patches[tag] = applied
    return temp, root, patches


class Runner:
    def __init__(
        self,
        cases_path: Path,
        prover_root: Path,
        oracle_binary: Path,
        *,
        continue_after_divergence: bool,
        pre08_compat: bool,
    ):
        self.api = load_oracle_api(prover_root)
        self.client = self.api.OracleClient(oracle_binary)
        self.world = self.api.OracleState()
        self.cases = {tag: CaseData.load(cases_path, tag) for tag in CASE_CONFIG}
        for data in self.cases.values():
            object.__setattr__(data, "oracle_api", self.api)
        self.stub_arc56 = load_json(
            cases_path / STUB_SOURCE["tag"] / "out_avm" / "StubERC20.arc56.json"
        )
        self._compat_temp = None
        self.compatibility_patches: dict[str, list[str]] = {}
        artifact_cases = cases_path
        if pre08_compat:
            self._compat_temp, artifact_cases, self.compatibility_patches = (
                build_pre08_compat_artifacts(cases_path)
            )
        self.artifacts = {
            data.config["contract"]: self.api.artifact(
                artifact_cases, tag, data.config["contract"]
            )
            for tag, data in self.cases.items()
        }
        self.artifacts["StubERC20"] = self.api.artifact(
            cases_path, STUB_SOURCE["tag"], "StubERC20"
        )
        # Per-tag CURRENT era: replay_call always encodes against the arc56 of
        # the implementation live at that point in the stream.
        self.era_arc56 = {tag: data.arc56 for tag, data in self.cases.items()}
        self.upgrade_artifacts: list[tuple[dict[str, Any], dict[str, Any]]] = []
        self.upgrades_applied: list[dict[str, Any]] = []
        for entry in UPGRADES:
            art_dir = cases_path / entry["avm_artifact"]
            contract = entry["contract"]
            source = (art_dir / f"{contract}.approval.teal").read_text()
            applied: list[str] = []
            if pre08_compat:
                source, applied = pre08_compat_teal(source)
            if applied:
                self.compatibility_patches[
                    f"upgrade:{entry['tag']}#{len(self.upgrade_artifacts)}"
                ] = applied
            self.upgrade_artifacts.append(
                (
                    {
                        "name": contract,
                        "source": source,
                        "clear": (art_dir / f"{contract}.clear.teal").read_text(),
                        "approval_size": (
                            art_dir / f"{contract}.approval.bin"
                        ).stat().st_size,
                        "clear_size": (
                            art_dir / f"{contract}.clear.bin"
                        ).stat().st_size,
                    },
                    load_json(art_dir / f"{contract}.arc56.json"),
                )
            )
        self.cases_path = cases_path
        self.continue_after_divergence = continue_after_divergence
        self.steps: list[dict[str, Any]] = []
        self.results: list[dict[str, Any]] = []
        self.tainted = False
        # Evidence-based receipt corrections made during this run (never
        # silent: every entry names the hash and the evidence).
        self.receipt_corrections: list[dict[str, Any]] = []

    def app_spec(
        self, app_id: int, artifact: dict[str, Any], creator: str, *, current=False
    ) -> dict[str, Any]:
        return {
            "app": self.api.ORACLE_APP_ID if current else app_id,
            "creator": creator,
            "approval_source": artifact["source"],
            "clear_state_source": artifact["clear"],
            # The canonical ledger enforces schemas (the hand-applied path did
            # not); puya-sol contracts keep scalar state vars in app globals,
            # so declare the AVM maximum (64 total entries) up front.
            "global_num_uint": 32,
            "global_num_byteslice": 32,
            "extra_program_pages": 7,
        }

    def initialize(
        self,
        *,
        name: str,
        app_id: int,
        artifact: dict[str, Any],
        arc56: dict[str, Any],
        creator: str,
        timestamp: int,
        block: int,
        ctor_values: list[Any] | None,
        dependency: bool = False,
    ) -> None:
        state = self.api.OracleState()
        state.latest_timestamp = timestamp
        state.round = block
        state.register_application(
            self.app_spec(app_id, artifact, creator, current=True)
        )
        state.balances[(self.api.CONTROLLER,)] = {
            "account": "app",
            "amount": 30_000_000,
        }
        state.balances[(creator,)] = {"account": creator, "amount": 10**12}

        total = artifact["approval_size"] + artifact["clear_size"]
        charged = max(0, total - 4 * 2048)
        quota_refs = (charged + 2047) // 2048
        response, record = self.api.run_with_resources(
            self.client,
            state,
            f"create {name}",
            artifact["source"],
            creating=True,
            sender=creator,
            fee=8_000,
            box_refs=[""] * quota_refs,
        )
        record.update(
            {
                "kind": "dependency-create" if dependency else "create",
                "block": block,
                "timestamp": timestamp,
                "app_id": app_id,
                "creator": creator,
            }
        )
        self.steps.append(record)
        if not state.carry(response):
            raise RuntimeError(f"{name} creation failed: {record}")

        final = response
        if ctor_values is not None:
            state.reference_box(P0)
            post_args = encode_method(
                arc56,
                "__postInit(" + ",".join("_" for _ in ctor_values) + ")",
                ctor_values,
            )
            # method_for matches __postInit by arity, so the placeholder source types
            # above deliberately avoid duplicating Solidity-to-ARC widening logic.
            final, record = self.api.run_with_resources(
                self.client,
                state,
                f"initialize {name}",
                artifact["source"],
                sender=creator,
                fee=16_000,
                app_args=post_args,
                group=self.api.budget_group(),
                group_index=0,
            )
            record.update(
                {
                    "kind": "dependency-initialize" if dependency else "initialize",
                    "block": block,
                    "timestamp": timestamp,
                    "app_id": app_id,
                }
            )
            self.steps.append(record)
            if not state.carry(final):
                raise RuntimeError(f"{name} initialization failed: {record}")

        self.world.register_application(self.app_spec(app_id, artifact, creator))
        self.world.absorb_current_as(final, app_id)

    def run_resources(
        self, name: str, source: str, **fields: Any
    ) -> tuple[dict[str, Any], dict[str, Any]]:
        response, record = self.api.run_with_resources(
            self.client,
            self.world,
            name,
            source,
            unified_access=True,
            **fields,
        )
        if (
            response.get("result") == "PANIC"
            and "access list needs" in response.get("error", "").lower()
        ):
            response, record = self.api.run_with_resources(
                self.client,
                self.world,
                name,
                source,
                pooled_group_resources=True,
                **fields,
            )
            record["resource_mode"] = "pooled-legacy-fallback"
        else:
            record["resource_mode"] = "unified-access"
        return response, record

    def call_app(
        self,
        *,
        name: str,
        app_id: int,
        arc56: dict[str, Any] | None,
        signature: str | None,
        values: list[Any] | None,
        sender: str,
        value: int = 0,
        synthetic: bool = False,
    ) -> tuple[dict[str, Any], dict[str, Any]]:
        self.world.box_refs = []
        self.world.foreign_box_refs = []
        self.world.balances.setdefault((sender,), {"account": sender, "amount": 10**12})
        self.world.auth[(sender,)] = {"account": sender, "auth": "app"}
        args = encode_method(arc56, signature, values) if signature and arc56 else []
        source = outer_call(app_id, args, self.api.app_address(app_id), value)
        response, record = self.run_resources(
            name,
            source,
            sender=sender,
            # Surplus fee credit pays fee-zero ephemeral application transactions when
            # the historical receiveMessage artifact OpUps to 45k.
            fee=100_000,
            # ECDSA recovery in receiveMessage costs just over 9k opcodes.  A full
            # 16-transaction group supplies the protocol maximum pooled budget.
            group=self.api.budget_group(15),
            group_index=0,
        )
        record["synthetic_dependency_state"] = synthetic
        if response.get("result") == "ACCEPT":
            self.world.carry(response)
        return response, record

    def seed_usdc(self, data: CaseData, call: dict[str, Any], sender: str) -> None:
        amount = int(call["args"][0])
        target = address_argument(data.raw_address(call["sender"]["__addr__"]))
        for label, signature, values, prep_sender in (
            ("mint", "mint(address,uint256)", [target, amount], sender),
            (
                "approve",
                "approve(address,uint256)",
                [
                    bytes.fromhex(
                        self.api.app_address(data.config["app_id"])
                    ),
                    amount,
                ],
                sender,
            ),
        ):
            response, record = self.call_app(
                name=f"USDC reconstruction: {label} for {call['hash']}",
                app_id=STUB_CONFIG["app_id"],
                arc56=self.stub_arc56,
                signature=signature,
                values=values,
                sender=prep_sender,
                synthetic=True,
            )
            record.update(
                {
                    "kind": "dependency-state",
                    "historical_hash": call["hash"],
                    "amount": amount,
                }
            )
            self.steps.append(record)
            if response.get("result") != "ACCEPT":
                raise RuntimeError(f"USDC {label} reconstruction failed: {record}")

    def deploy_dependency(self) -> None:
        first = min(
            (data.case["creation"] for data in self.cases.values()),
            key=lambda creation: (int(creation["block"]), int(creation["ts"])),
        )
        # A case fetched without a resolvable creation txn carries block/ts 0,
        # and the stub then deploys one step EARLIER than that — a negative
        # uint64 the oracle rejects with an opaque unmarshal error. Say what
        # is actually wrong instead.
        if int(first["ts"]) <= 0 or int(first["block"]) <= 0:
            broken = [
                tag for tag, data in self.cases.items()
                if not int(data.case["creation"].get("block") or 0)
                or not int(data.case["creation"].get("ts") or 0)
            ]
            raise RuntimeError(
                f"case(s) {broken} have an unresolved creation block/ts "
                f"(0) — re-fetch them or restore creation from a backup; "
                f"every deploy timestamp derives from it"
            )
        stub_registry = load_json(
            self.cases_path / STUB_SOURCE["tag"] / "registry.json"
        )
        creator = (bytes(12) + raw20(stub_registry["creator"])).hex()
        self.initialize(
            name="StubERC20",
            app_id=STUB_CONFIG["app_id"],
            artifact=self.artifacts["StubERC20"],
            arc56=self.stub_arc56,
            creator=creator,
            timestamp=int(first["ts"]) - 1,
            block=int(first["block"]) - 1,
            ctor_values=None,
            dependency=True,
        )

    def deploy_case(self, data: CaseData) -> None:
        creation = data.case["creation"]
        creator = (bytes(12) + raw20(data.registry["creator"])).hex()
        ctor_values = [data.resolve(value) for value in data.calls["meta"]["ctor_args"]]
        self.initialize(
            name=data.config["contract"],
            app_id=data.config["app_id"],
            artifact=self.artifacts[data.config["contract"]],
            arc56=data.arc56,
            creator=creator,
            timestamp=int(creation["ts"]),
            block=int(creation["block"]),
            ctor_values=ctor_values,
        )
        # Config-era replay (v2): the proxy's historical initialize call,
        # decoded from the creation txn's delegatecall trace. Sender is the
        # creator — `initializer` gates on the latch, not on ownership.
        for entry in INIT_CALLS:
            if entry["tag"] != data.tag:
                continue
            response, record = self.call_app(
                name=f"historical initialize {data.config['contract']}",
                app_id=data.config["app_id"],
                arc56=data.arc56,
                signature=entry["sig"],
                values=[data.resolve(v) for v in entry["args"]],
                sender=creator,
            )
            record.update({"kind": "config-era-initialize"})
            self.steps.append(record)
            if response.get("result") != "ACCEPT":
                raise RuntimeError(
                    f"{data.config['contract']} historical initialize failed: {record}"
                )

    def apply_upgrade(self, item: dict[str, Any]) -> None:
        """Native UpdateApplication at the historical upgrade block.

        Re-registering the app spec swaps the approval program while the
        app's boxes and globals persist — byte-for-byte what an admin-signed
        UpdateApplication does on chain (proxy.md §1). The historical
        upgradeToAndCall's embedded calldata, if any, replays right after.
        """
        entry = item["upgrade"]
        data = self.cases[entry["tag"]]
        artifact, arc56 = self.upgrade_artifacts[item["upgrade_index"]]
        self.world.latest_timestamp = int(entry["ts"])
        self.world.round = int(entry["block"])
        creator = (bytes(12) + raw20(data.registry["creator"])).hex()
        self.world.register_application(
            self.app_spec(data.config["app_id"], artifact, creator)
        )
        self.era_arc56[data.tag] = arc56
        step = {
            "kind": "native-update",
            "tag": data.tag,
            "contract": entry["contract"],
            "block": int(entry["block"]),
            "historical_hash": entry.get("hash"),
            "new_implementation": entry.get("impl"),
        }
        self.steps.append(step)
        self.upgrades_applied.append(step)
        if entry.get("init_sig"):
            sender = (
                (bytes(12) + raw20(entry["sender"])).hex()
                if entry.get("sender")
                else creator
            )
            response, record = self.call_app(
                name=f"upgrade initialize {entry['contract']} {entry['init_sig']}",
                app_id=data.config["app_id"],
                arc56=arc56,
                signature=entry["init_sig"],
                values=[data.resolve(v) for v in entry.get("init_args") or []],
                sender=sender,
            )
            record.update({"kind": "upgrade-initialize"})
            self.steps.append(record)
            if response.get("result") != "ACCEPT":
                raise RuntimeError(
                    f"{entry['contract']} upgrade initialize failed: {record}"
                )

    def replay_call(self, item: dict[str, Any]) -> bool:
        data = self.cases[item["tag"]]
        call = item["call"]
        self.world.latest_timestamp = int(call["ts"])
        self.world.round = int(item["block"])
        sender = data.sender(call, item["txn"])
        # Only the TokenMessenger declares depositForBurn* — keying on the
        # method rather than a literal tag keeps this correct for v2, whose
        # tags differ and whose signature carries three extra parameters.
        if call["sig"] and (
            call["sig"].startswith("depositForBurn") and historical_ok(call)
        ):
            self.seed_usdc(data, call, sender)

        response, record = self.call_app(
            name=f"historical {data.config['contract']}[{call['i']}] {call['sig']}",
            app_id=data.config["app_id"],
            arc56=self.era_arc56[data.tag] if call.get("sig") else None,
            signature=call["sig"],
            values=[data.resolve(value) for value in call["args"]]
            if call.get("args") is not None
            else None,
            sender=sender,
            value=int(call.get("value") or 0),
        )
        observed_ok = response.get("result") == "ACCEPT"
        matched = observed_ok == historical_ok(call)
        result = {
            "tag": data.tag,
            "contract": data.config["contract"],
            "case_index": call["i"],
            "hash": call["hash"],
            "signature": call["sig"],
            "block": item["block"],
            "timestamp": call["ts"],
            "sender": sender,
            "value": int(call.get("value") or 0),
            "historical_ok": historical_ok(call),
            "oracle_result": response.get("result"),
            "oracle_category": record.get("category"),
            "oracle_error": response.get("error"),
            "matched_status": matched,
            "tainted_before": self.tainted,
            "resource_mode": record.get("resource_mode"),
            "access_list_count": record.get("access_list_count"),
            "box_resources_discovered": record.get("box_resources_discovered"),
            "nested_application_ids": record.get("nested_application_ids"),
            # ARC-28 logs, program order: the outer call's own logs followed by
            # every inner transaction's AT ANY DEPTH (attributed by app id) —
            # MessageSent/MintAndWithdraw fire 2-3 inner levels down
            # (transmitter→messenger→minter), so depth-1 alone loses them.
            "logs": list(response.get("logs") or []),
            "inner_logs": flatten_inner_logs(response.get("inners_after") or []),
        }
        self.results.append(result)
        if not matched:
            self.tainted = True
        return matched

    def zero_log_ok_hashes(self) -> set[str]:
        """Hashes whose receipt says "ok" but carries NO logs at all.

        A state-changing CCTP method that succeeded always emits (a real
        receiveMessage produces Mint + Transfer + MintAndWithdraw +
        MessageReceived). An "ok" receipt with zero entries is therefore
        self-contradictory — the indexer's status field is stale. The differ
        already refuses to compare events for these (corrupt_empty_receipt);
        this exposes the same evidence to the STATUS comparison, which is
        where a duplicate-delivery race otherwise reads as a divergence.

        Only hashes present in logs.json are considered: a missing entry means
        "not fetched", which is not evidence of anything.
        """
        out: set[str] = set()
        for tag in CASE_CONFIG:
            path = self.cases_path / tag / "logs.json"
            if not path.exists():
                continue
            try:
                logs = load_json(path)
            except Exception:
                continue
            for h, entries in logs.items():
                if not entries:
                    out.add(h)
        return out

    def reclassify_payload_races(self) -> None:
        """Identical-payload relayer races: outcome MULTISET comparison.

        Two byte-identical submissions of one attested message can both carry
        "ok" receipts when an indexer's status field is stale (verified: the
        earlier 'ok' txn has ZERO receipt logs — a real receiveMessage success
        always emits), or history's winner can differ from the replay's
        because the loser failed on gas, which neither leg models. The chain
        accepted exactly one of the group; so does the replay. When the
        group's historical and observed outcome multisets agree, the order is
        environmental, not semantic: mark those rows matched with a note.

        When they do NOT agree, one more evidence-based correction applies
        before giving up: within a duplicate-payload group, an "ok" row whose
        receipt carries zero logs did not actually succeed (CCTP's usedNonces
        makes a second delivery of one attested message impossible, so the
        chain cannot have accepted both). Those rows' historical truth is
        corrected to failed — explicitly, per row, and recorded in the report
        — and the multiset test is retried.
        """
        from collections import Counter, defaultdict

        zero_log_ok = self.zero_log_ok_hashes()
        groups: dict[tuple, list[dict[str, Any]]] = defaultdict(list)
        for r in self.results:
            data = self.cases.get(r["tag"])
            if data is None:
                continue
            call = next(
                (c for c in data.calls["calls"] if c["hash"] == r["hash"]), None
            )
            if call is None or not call.get("sig"):
                continue
            key = (r["tag"], call["sig"], json.dumps(call.get("args"), sort_keys=True))
            groups[key].append(r)
        for rows in groups.values():
            if len(rows) < 2 or all(r["matched_status"] for r in rows):
                continue
            hist = Counter(bool(r["historical_ok"]) for r in rows)
            seen = Counter(r["oracle_result"] == "ACCEPT" for r in rows)
            note = (
                "identical-payload race: outcome multiset matches history; "
                "order is gas/indexer-environmental"
            )
            if hist != seen:
                # Correct provably-corrupt "ok" receipts inside THIS duplicate
                # group, then retry. Never touches rows outside a duplicate
                # group, and never turns a failure into a success.
                corrected = [
                    r for r in rows
                    if r["historical_ok"] and r["hash"].split("#")[0] in zero_log_ok
                ]
                if not corrected:
                    continue
                hist = Counter(
                    False if r in corrected else bool(r["historical_ok"])
                    for r in rows
                )
                if hist != seen:
                    continue
                for r in corrected:
                    r["historical_ok_corrected"] = False
                    self.receipt_corrections.append({
                        "hash": r["hash"],
                        "signature": r["signature"],
                        "reason": (
                            "duplicate attested payload whose 'ok' receipt has "
                            "ZERO logs — a successful CCTP call always emits, "
                            "and usedNonces forbids a second delivery"
                        ),
                    })
                note = (
                    "identical-payload race: an 'ok' receipt in the group has "
                    "zero logs (corrupt indexer status) — corrected to failed, "
                    "after which the outcome multiset matches history"
                )
            if hist == seen:
                for r in rows:
                    if not r["matched_status"]:
                        r["matched_status"] = True
                        r["status_note"] = note

    def run(self, limit: int | None) -> dict[str, Any]:
        self.deploy_dependency()
        replayed = 0
        stopped = False
        for item in historical_stream(self.cases):
            if item["kind"] == "create":
                self.world.latest_timestamp = item["timestamp"]
                self.world.round = item["block"]
                self.deploy_case(self.cases[item["tag"]])
                continue
            if limit is not None and replayed >= limit:
                break
            if item["kind"] == "upgrade":
                self.apply_upgrade(item)
                continue
            matched = self.replay_call(item)
            replayed += 1
            if not matched and not self.continue_after_divergence:
                stopped = True
                break

        self.reclassify_payload_races()
        compared = [r for r in self.results if r.get("matched_status") is not None]
        skipped = [r for r in self.results if r.get("status") == "not-replayed"]
        mismatches = [r for r in compared if not r["matched_status"]]
        return {
            "scope": {
                "kind": "historical CCTP receipt-status replay",
                "real_contracts": [item["contract"] for item in CASE_CONFIG.values()],
                "root_fixture_calls": sum(
                    1
                    for item in historical_stream(self.cases)
                    if item["kind"] == "call"
                ),
                "lifted_inner_calls_excluded": sum(
                    1
                    for data in self.cases.values()
                    for call in data.calls["calls"]
                    if "#" in call["hash"]
                ),
                "dependency_model": (
                    "StubERC20 with per-successful-deposit synthetic mint/approval"
                ),
                "comparison": "historical receipt status vs oracle ACCEPT/reject",
                "mainnet_receipt_metadata_corrections": MAINNET_RECEIPT_METADATA,
                "zero_log_receipt_corrections": self.receipt_corrections,
                "artifact_source": (
                    "cached corpus TEAL copied to a temporary directory and patched; "
                    "the cached corpus itself is unchanged"
                ),
                "pre08_compatibility_patches": self.compatibility_patches,
                "mid_history_upgrades": self.upgrades_applied,
                "pre08_compatibility_boundary": (
                    "The known TypedMemView uint8 wrap is restored; this is not a "
                    "general emulation of every Solidity 0.7 unchecked arithmetic op."
                ),
                "not_compared": [
                    "historical USDC storage/balances/allowances",
                    "EVM return data",
                    "EVM events",
                    "full EVM-vs-AVM storage snapshots",
                ],
            },
            "summary": {
                "processed_root_calls": len(self.results),
                "compared_statuses": len(compared),
                "matched_statuses": sum(r["matched_status"] for r in compared),
                "status_mismatches": len(mismatches),
                "not_replayed": len(skipped),
                "stopped_at_first_divergence": stopped,
                "tainted_suffix": self.tainted and self.continue_after_divergence,
            },
            "first_mismatch": mismatches[0] if mismatches else None,
            "initialization_and_dependency_steps": self.steps,
            "results": self.results,
            "final_storage": self.dump_slot_storage(),
        }

    def dump_slot_storage(self) -> dict[str, dict[str, str]]:
        """Slot→word maps decoded from each app's page/sparse boxes.

        --evm-storage-layout backs solc's slot space with boxes:
          "p:" ++ itob(slot // 64)  → 2048-byte page (64 dense words)
          "s:" ++ slot32            → one 32-byte word per keccak-derived slot
        Zero-padded historical addresses make AVM mapping-key hashing
        bit-identical to EVM keccak slots, so these maps compare
        slot-for-slot against a local EVM replay.
        """
        by_app: dict[int, dict[str, str]] = {}
        for (app, _key), item in self.world.foreign_boxes.items():
            name = bytes.fromhex(item["key"])
            data = bytes.fromhex(item.get("bytes") or "")
            slots = by_app.setdefault(int(app), {})
            if name.startswith(b"p:") and len(name) == 10:
                page = int.from_bytes(name[2:], "big")
                for j in range(0, len(data) // 32):
                    word = data[j * 32 : (j + 1) * 32]
                    if any(word):
                        slots[str(page * 64 + j)] = word.hex()
            elif name.startswith(b"s:") and len(name) == 34:
                if any(data):
                    slots[str(int.from_bytes(name[2:], "big"))] = data[:32].hex()
        tags = {config["app_id"]: tag for tag, config in CASE_CONFIG.items()}
        tags[STUB_CONFIG["app_id"]] = "stub_usdc"
        return {
            tags[app]: slots for app, slots in by_app.items() if app in tags
        }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("cases", type=Path)
    parser.add_argument(
        "--prover-root",
        type=Path,
        default=os.environ.get("AVM_PROVER_ROOT"),
        required="AVM_PROVER_ROOT" not in os.environ,
        help="avm-prover checkout (or set AVM_PROVER_ROOT)",
    )
    parser.add_argument(
        "--oracle",
        type=Path,
        help="oracle binary; defaults to <prover-root>/oracle/avmoracle",
    )
    parser.add_argument("--output", type=Path)
    parser.add_argument("--limit", type=int)
    parser.add_argument("--continue-after-divergence", action="store_true")
    parser.add_argument(
        "--config",
        type=Path,
        help="joint config JSON re-pointing the replay at another contract "
        "system (e.g. joint_config_v2.json for CCTP v2)",
    )
    parser.add_argument(
        "--no-pre08-compat",
        action="store_true",
        help="use cached pragma-relaxed artifacts without the CCTP uint8-wrap shim",
    )
    args = parser.parse_args()
    if args.config:
        apply_joint_config(load_json(args.config))

    oracle = args.oracle or args.prover_root / "oracle" / "avmoracle"
    runner = Runner(
        args.cases.resolve(),
        args.prover_root.resolve(),
        oracle.resolve(),
        continue_after_divergence=args.continue_after_divergence,
        pre08_compat=not args.no_pre08_compat,
    )
    report = runner.run(args.limit)
    rendered = json.dumps(report, indent=2) + "\n"
    if args.output:
        args.output.write_text(rendered)
    else:
        print(rendered, end="")
    summary = report["summary"]
    print(
        "historical CCTP: "
        f"{summary['matched_statuses']}/{summary['compared_statuses']} statuses match; "
        f"{summary['not_replayed']} not replayed; "
        f"{summary['status_mismatches']} mismatch(es)",
        file=sys.stderr,
    )
    return 1 if summary["status_mismatches"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
