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

from algosdk.abi import ABIType, Method
from algosdk.encoding import encode_address


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
    encoded = [Method.from_signature(signature).get_selector().hex()]
    for spec, value in zip(method["args"], values):
        if spec["type"] == "address" and isinstance(value, (bytes, bytearray)):
            value = encode_address(bytes(value))
        encoded.append(ABIType.from_string(spec["type"]).encode(value).hex())
    return encoded


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
        minter = self.cases["cctp_minter"]
        self.stub_arc56 = load_json(minter.path / "out_avm" / "StubERC20.arc56.json")
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
            cases_path, "cctp_minter", "StubERC20"
        )
        self.continue_after_divergence = continue_after_divergence
        self.steps: list[dict[str, Any]] = []
        self.results: list[dict[str, Any]] = []
        self.tainted = False

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
                        self.api.app_address(CASE_CONFIG["cctp_messenger"]["app_id"])
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
        minter = self.cases["cctp_minter"]
        creator = (bytes(12) + raw20(minter.registry["creator"])).hex()
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

    def replay_call(self, item: dict[str, Any]) -> bool:
        data = self.cases[item["tag"]]
        call = item["call"]
        self.world.latest_timestamp = int(call["ts"])
        self.world.round = int(item["block"])
        sender = data.sender(call, item["txn"])
        if (
            data.tag == "cctp_messenger"
            and call["sig"].startswith("depositForBurn(")
            and historical_ok(call)
        ):
            self.seed_usdc(data, call, sender)

        response, record = self.call_app(
            name=f"historical {data.config['contract']}[{call['i']}] {call['sig']}",
            app_id=data.config["app_id"],
            arc56=data.arc56 if call.get("sig") else None,
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
        }
        self.results.append(result)
        if not matched:
            self.tainted = True
        return matched

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
            matched = self.replay_call(item)
            replayed += 1
            if not matched and not self.continue_after_divergence:
                stopped = True
                break

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
                "artifact_source": (
                    "cached corpus TEAL copied to a temporary directory and patched; "
                    "the cached corpus itself is unchanged"
                ),
                "pre08_compatibility_patches": self.compatibility_patches,
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
        "--no-pre08-compat",
        action="store_true",
        help="use cached pragma-relaxed artifacts without the CCTP uint8-wrap shim",
    )
    args = parser.parse_args()

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
