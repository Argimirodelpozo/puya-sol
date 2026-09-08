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

JOINT_ARTIFACT_DIR = "out_avm_joint"

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


def read_artifact(directory: Path, contract: str) -> dict[str, Any]:
    """The oracle artifact shape, without its adapter's hardcoded out_avm path."""
    return {
        "name": contract,
        "source": (directory / f"{contract}.approval.teal").read_text(),
        "clear": (directory / f"{contract}.clear.teal").read_text(),
        "approval_size": (directory / f"{contract}.approval.bin").stat().st_size,
        "clear_size": (directory / f"{contract}.clear.bin").stat().st_size,
    }


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
        )

    @property
    def arc56(self) -> dict[str, Any]:
        # Pure history construction and the EVM leg do not need AVM artifacts.
        return load_json(self.path / JOINT_ARTIFACT_DIR / f"{self.config['contract']}.arc56.json")

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


def count_opup_txns(txns: list[dict[str, Any]]) -> tuple[int, int]:
    """(opup inner txns, total inner txns) at any depth.

    An OpUp escalation is an ephemeral application CREATE whose completion is
    DeleteApplication (ApplicationID 0, OnCompletion 5) — puya's ensure_budget
    and the v1 replay shim both use it. Counting them is the only budget
    signal this driver can collect for free: the oracle's apply path reports
    no opcode figures, so a replay that merely passes says nothing about cost.
    Each one is a real transaction with a real fee, which makes the count the
    metric a deployment actually pays.

    Note what it measures: the OpUp loop tops up to its TARGET, so the count
    reflects the budget REQUESTED, not consumed. It is a cost and regression
    signal at a fixed target, not a measurement of headroom — for that, bisect
    the target itself (see budget_probe.py / CCTP_ENSURE_BUDGET).
    """
    opup = total = 0
    for txn in txns or []:
        u64 = txn.get("u64") or {}
        total += 1
        if int(u64.get("ApplicationID") or 0) == 0 and \
                int(u64.get("OnCompletion") or 0) == 5:
            opup += 1
        sub_opup, sub_total = count_opup_txns(txn.get("inner_txns") or [])
        opup += sub_opup
        total += sub_total
    return opup, total


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


def _app_account_funding() -> int:
    """microAlgos each replayed application account is created with.

    Funding is environmental, like the OpUp fee credit: on chain the deployer
    tops the app account up as its box storage grows. Every box a contract
    creates raises the account's minimum balance (2500 + 400 * (name + size)
    microAlgos), and CCTP writes one `usedNonces` box per received message
    plus balance boxes in the stub; the prover enforces that requirement, so a
    fixed 30 ALGO seed ran out after ~1000 receiveMessage calls (2026-09-07:
    1971 `balance 30000000 below min 30028000` panics from index 1070 of a
    6029-call window, none of them a compiler divergence). Seed enough for
    any window; lower it (CCTP_APP_FUNDING) only to probe MBR behaviour.
    """
    return int(os.environ.get("CCTP_APP_FUNDING", str(10**12)))


def _progress_every() -> int:
    """Root calls between stderr progress lines (a deep window runs for hours
    with no other output; 0 disables)."""
    return int(os.environ.get("CCTP_PROGRESS_EVERY", "250")) or 1 << 62


def _ensure_budget_target() -> int:
    """OpUp target for the v1 receive/replace shim.

    Parametrised so the replay can MEASURE what these entry points actually
    need instead of only proving they fit under a generous ceiling: lowering
    it until calls start failing bounds the real requirement. Default is the
    historical 45000, so ordinary runs are unchanged.
    """
    return int(os.environ.get("CCTP_ENSURE_BUDGET", "45000"))


# ── TEAL-shape compatibility shims ───────────────────────────────────────────
# A shim is a set of anchored SITES.  Each site's regex CONTAINS the anchor it
# belongs to (the router label, the assert, the compare), so a match is
# adjacency by construction; the regex must match exactly once, and after the
# rewrite a marker line unique to that site must sit within `window` lines of
# the re-located anchor.  Anything else raises ShimError naming the shim and
# the site.  A shim that lands somewhere else, twice, or not at all is never
# silent: the 08-13 → rev-2 re-derivation hit every one of those (router
# label suffixes, intc_N / intc N / pushint spellings, a `dig 36` stack temp
# that became a frame slot), and the shape-regex that replaced the literals
# then fired on CCTP v2's receiveMessage router and claimed shims it had not
# applied.  Which shims an artifact gets is keyed by CONTRACT NAME
# (EXPECTED_SHIMS), never by what happens to match.

TMV_INDEX_ASSERT = (
    "assert // TypedMemView/index - Attempted to index more than 32 bytes"
)
SHIM_MARK = "// shim:"
V1_TRANSMITTER_CONTRACT = "MessageTransmitter"

HISTORICAL_ENSURE_BUDGET_TEAL = """

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


class ShimError(ValueError):
    """A compatibility shim's anchor is missing, ambiguous, or has moved."""


@dataclass(frozen=True)
class ShimSite:
    what: str
    pattern: re.Pattern[str]  # contains the anchor; must match exactly once
    replacement: str
    anchor: re.Pattern[str]  # re-locates the anchor line after the rewrite
    marker: str  # unique text the rewrite leaves at the site
    window: int = 12  # max |marker line - anchor line|


@dataclass(frozen=True)
class Shim:
    name: str  # reported in scope.pre08_compatibility_patches
    sites: tuple[ShimSite, ...]
    epilogue: str = ""  # appended once after every site applied


def _const(value: int) -> str:
    """Any rendering of an integer constant across compiler revisions."""
    return (
        rf"(?:intc_\d+ // {value}|intc \d+ // {value}|pushint {value}(?: // {value})?)"
    )


def _label(name: str) -> re.Pattern[str]:
    """A label DEFINITION (the router's `match` line lists labels without ':')."""
    return re.compile(rf"^{re.escape(name)}@\d+:$", re.M)


def _tmv_wrap_shim() -> Shim:
    """Restore the intentional uint8 wrap used by CCTP's Solidity-0.7 TMV.

    The cached corpus was fetched with ``--relax-pre08`` because puya-sol accepts
    Solidity 0.8 syntax.  Merely changing the pragma also changes arithmetic: the
    historical TypedMemView deliberately computes ``uint8(32 * 8) == 0`` to ask
    ``leftMask`` for all 256 bits, while 0.8 reverts.  The site is the overflow
    assert right after ``_bytes * 8`` in TypedMemView.index; the rewrite drops
    it and reduces mod 256 instead.
    """
    marker = f"pushint 256 {SHIM_MARK} TypedMemView.index uint8 wrap"
    return Shim(
        name="TypedMemView.index uint8(32 * 8) wrap restored with unchecked",
        sites=(
            ShimSite(
                what="uint8 overflow assert after `_bytes * 8` in TypedMemView.index",
                pattern=re.compile(
                    r"(TypedMemView\.index_after_if_else@\d+:\n"
                    r"(?:    frame_dig \d+\n)?"
                    rf"    {re.escape(TMV_INDEX_ASSERT)}\n"
                    r"    frame_dig -1\n"
                    rf"    {_const(8)}\n"
                    r"    \*\n)"
                    r"    dup\n"
                    rf"    {_const(255)}\n"
                    r"    <=\n"
                    r"    assert // overflow\n"
                    r"(    frame_dig -3\n)"
                ),
                replacement=r"\1" + f"    {marker}\n    %\n" + r"\2",
                anchor=re.compile(rf"^    {re.escape(TMV_INDEX_ASSERT)}$", re.M),
                marker=marker,
                window=6,
            ),
        ),
    )


def _opup_shim() -> Shim:
    """OpUp escalation at the receiveMessage/replaceMessage router entries.

    v1 ships without ``--ensure-budget``; receiveMessage's ECDSA recovery and
    TMV parsing need 12-14k opcodes, above the 11,200 a fully pooled group
    supplies (README "Opcode budget"), so the harness buys budget the way a
    real deployment would.  Sites: the two router labels; the call is
    inserted immediately after the label, ahead of the argument decode.
    Lines the other route shim already inserted (all carry SHIM_MARK) may sit
    between the label and the decode.
    """
    target = _ensure_budget_target()

    def site(route: str) -> ShimSite:
        marker = f"callsub __historical_ensure_budget {SHIM_MARK} OpUp {route} route"
        return ShimSite(
            what=f"{route} router entry (label + `txna ApplicationArgs 1` decode)",
            pattern=re.compile(
                rf"(^main_{route}_route@\d+:\n(?:    .*{re.escape(SHIM_MARK)}.*\n)*)"
                r"(    txna ApplicationArgs 1\n)",
                re.M,
            ),
            replacement=(
                r"\1"
                + f"    pushint {target} {SHIM_MARK} OpUp {route} target\n"
                + f"    pushint 0 {SHIM_MARK} OpUp {route} fee\n"
                + f"    {marker}\n"
                + r"\2"
            ),
            anchor=_label(f"main_{route}_route"),
            marker=marker,
            window=8,
        )

    return Shim(
        name=f"receiveMessage/replaceMessage ensure_budget({target}) OpUp shim",
        sites=(site("receiveMessage"), site("replaceMessage")),
        epilogue=HISTORICAL_ENSURE_BUDGET_TEAL,
    )


def _body_forward_shim() -> Shim:
    """Forward the exact signed message body to handleReceiveMessage.

    Site 1 captures the raw ``message`` argument (ARC-4 length prefix
    stripped) into scratch 255 at the receiveMessage router entry.  Site 2 is
    the ``Message._messageBody`` + ``TypedMemView.clone`` pair that feeds the
    handleReceiveMessage inner call right after ``assert // Nonce already
    used`` (unique to receiveMessage): it is replaced by ``bytes[116:]`` of the
    captured message — 116 is CCTP's fixed header size.  Stack shape is
    unchanged (view stays, body bytes on top).
    """
    capture = f"store 255 {SHIM_MARK} capture signed message bytes"
    forwarded = f"{SHIM_MARK} message body forwarded from the captured signed bytes"
    return Shim(
        name="receiveMessage forwards exact signed message bytes[116:]",
        sites=(
            ShimSite(
                what="capture of the raw message argument at the receiveMessage router entry",
                pattern=re.compile(
                    rf"(^main_receiveMessage_route@\d+:\n(?:    .*{re.escape(SHIM_MARK)}.*\n)*)"
                    r"(    txna ApplicationArgs 1\n)",
                    re.M,
                ),
                replacement=(
                    r"\1"
                    + f"    txna ApplicationArgs 1 {SHIM_MARK} capture message\n"
                    + f"    extract 2 0 {SHIM_MARK} capture message\n"
                    + f"    {capture}\n"
                    + r"\2"
                ),
                anchor=_label("main_receiveMessage_route"),
                marker=capture,
                window=8,
            ),
            ShimSite(
                what=(
                    "Message._messageBody + TypedMemView.clone feeding the "
                    "handleReceiveMessage inner call after `assert // Nonce already used`"
                ),
                pattern=re.compile(
                    r"(    assert // Nonce already used\n(?:    [^\n]*\n){1,24}?)"
                    r"    dup\n"
                    r"    callsub Message\._messageBody\n"
                    r"    callsub TypedMemView\.clone\n"
                    r"(    itxn_begin\n)"
                ),
                replacement=(
                    r"\1"
                    + "    load 255\n"
                    + "    extract 116 0\n"
                    + f"    {forwarded}\n"
                    + r"\2"
                ),
                anchor=re.compile(r"^    assert // Nonce already used$", re.M),
                marker=forwarded,
                window=30,
            ),
        ),
    )


def _caller_compare_shim() -> Shim:
    """Compare a calling application to the low 64 bits of the sender word.

    replaceMessage requires ``msg.sender == bytes32ToAddress(_sender)``; when
    TokenMessenger.replaceDepositForBurn is the caller, ``_sender`` holds the
    messenger's historical 32-byte EVM address while the compiler's
    caller-app branch builds ``bytes24 ++ itob(CallerApplicationID)``.  The
    app id IS the address's low 64 bits by the joint lane's address model, so
    compare those.  Site: the ``||`` short-circuit's caller-app branch,
    between ``bnz replaceMessage_bool_true`` and ``bz replaceMessage_bool_false``;
    the sender word may be a stack temp (``dig N``) or a frame slot.
    """
    marker = (
        f"extract_uint64 {SHIM_MARK} caller app == low 64 bits of the historical sender"
    )
    return Shim(
        name="replaceMessage caller-app comparison uses address low 64 bits",
        sites=(
            ShimSite(
                what=(
                    "caller-application branch of `msg.sender == bytes32ToAddress(_sender)` "
                    "inside replaceMessage"
                ),
                pattern=re.compile(
                    r"(    bnz replaceMessage_bool_true@\d+\n"
                    r"    global CallerApplicationID\n"
                    r"    bz replaceMessage_bool_false@\d+\n)"
                    r"    pushint 24\n"
                    r"    bzero\n"
                    r"    global CallerApplicationID\n"
                    r"    itob\n"
                    r"    concat\n"
                    r"    ((?:frame_)?dig \d+)\n"
                    r"    ==\n"
                    r"(    bz replaceMessage_bool_false@\d+\n)"
                ),
                replacement=(
                    r"\1"
                    + "    global CallerApplicationID\n"
                    + r"    \2" + "\n"
                    + "    pushint 24\n"
                    + f"    {marker}\n"
                    + "    ==\n"
                    + r"\3"
                ),
                anchor=re.compile(r"^    bnz replaceMessage_bool_true@\d+$", re.M),
                marker=marker,
                window=12,
            ),
        ),
    )


_SHIM_BUILDERS = {
    "tmv": _tmv_wrap_shim,
    "opup": _opup_shim,
    "body": _body_forward_shim,
    "caller": _caller_compare_shim,
}
# Which shims each known contract MUST receive.  A contract listed here whose
# TEAL lacks one of its anchors fails loudly instead of replaying unshimmed.
EXPECTED_SHIMS: dict[str, tuple[str, ...]] = {
    V1_TRANSMITTER_CONTRACT: ("tmv", "opup", "body", "caller"),
    "TokenMessenger": ("tmv",),
    "TokenMinter": (),
    # v2 compiles its own budget in (build_v2_avm.py --ensure-budget), has no
    # replaceMessage, and its receiveMessage needs neither body forwarding nor
    # the caller compare — only the pragma-relaxation wrap applies.
    "MessageTransmitterV2": ("tmv",),
    "TokenMessengerV2": ("tmv",),
    "TokenMinterV2": (),
}


def shim_set_for(source: str, contract: str | None) -> tuple[str, ...]:
    """Shim keys for an artifact: by contract name, else inferred from shape.

    The inference exists for contracts this table does not know (synthetic
    selftests, upgrade eras): TypedMemView present → wrap; a replaceMessage
    router → the v1 MessageTransmitter set.
    """
    if contract in EXPECTED_SHIMS:
        return EXPECTED_SHIMS[contract]
    keys: list[str] = []
    if TMV_INDEX_ASSERT in source:
        keys.append("tmv")
    if _label("main_replaceMessage_route").search(source):
        keys += ["opup", "body", "caller"]
    return tuple(keys)


def _apply_site(source: str, shim: Shim, site: ShimSite) -> tuple[str, int]:
    """Rewrite one site; returns (patched, marker line).  Loud on any drift."""
    where = f"shim [{shim.name} :: {site.what}]"
    patched, count = site.pattern.subn(site.replacement, source)
    if count == 0:
        raise ShimError(
            f"{where}: anchor not found — this compiler revision changed the TEAL "
            "shape at that site; re-derive the site's pattern (grep the anchor in "
            "the artifact) instead of replaying an unshimmed artifact"
        )
    if count > 1:
        raise ShimError(f"{where}: anchor matched {count} times; refusing an ambiguous rewrite")
    anchors = [m.start() for m in site.anchor.finditer(patched)]
    if len(anchors) != 1:
        raise ShimError(
            f"{where}: expected exactly one anchor after the rewrite, found {len(anchors)}"
        )
    markers = [m.start() for m in re.finditer(re.escape(site.marker), patched)]
    if len(markers) != 1:
        raise ShimError(
            f"{where}: rewrite marker present {len(markers)} times, expected exactly once"
        )
    anchor_line = patched.count("\n", 0, anchors[0]) + 1
    marker_line = patched.count("\n", 0, markers[0]) + 1
    if abs(marker_line - anchor_line) > site.window:
        raise ShimError(
            f"{where}: rewrite landed {abs(marker_line - anchor_line)} lines from its "
            f"anchor (limit {site.window}) — the anchor moved"
        )
    return patched, marker_line


def apply_compat_shims(
    source: str, contract: str | None = None
) -> tuple[str, list[str], dict[str, list[dict[str, Any]]]]:
    """Apply the artifact's shim set; (patched, applied names, per-site audit).

    Every site of every expected shim must apply exactly once next to its
    anchor (ShimError otherwise), so the returned `applied` list is a record
    of rewrites that happened, not of intentions.
    """
    if "__historical_ensure_budget" in source:
        raise ValueError("historical ensure-budget shim already exists")
    if SHIM_MARK in source:
        raise ValueError("compatibility shims already applied")
    patched = source
    applied: list[str] = []
    placed: list[tuple[str, ShimSite]] = []
    for key in shim_set_for(source, contract):
        shim = _SHIM_BUILDERS[key]()
        for site in shim.sites:
            patched, _line = _apply_site(patched, shim, site)
            placed.append((shim.name, site))
        if shim.epilogue:
            patched += shim.epilogue
        applied.append(shim.name)
    # Audit lines are located in the FINAL text: a later shim's insertion
    # shifts every site recorded before it.
    sites: dict[str, list[dict[str, Any]]] = {}
    for name, site in placed:
        line = patched.count("\n", 0, patched.index(site.marker)) + 1
        sites.setdefault(name, []).append({"site": site.what, "line": line})
    return patched, applied, sites


def pre08_compat_teal(
    source: str, contract: str | None = None
) -> tuple[str, list[str]]:
    """CCTP-specific compatibility shims for a pragma-relaxed 0.7.6 artifact.

    Keep this narrow and reported rather than pretending every pre-0.8
    arithmetic expression has been reconstructed: the four shims (TMV uint8
    wrap, OpUp budget, exact message-body forwarding, low-64-bit caller
    compare) are the whole set, each anchored on the TEAL site it belongs to.
    """
    patched, applied, _sites = apply_compat_shims(source, contract)
    return patched, applied


def build_pre08_compat_artifacts(
    cases: Path,
) -> tuple[
    tempfile.TemporaryDirectory[str],
    Path,
    dict[str, list[str]],
    dict[str, dict[str, list[dict[str, Any]]]],
]:
    """Patch disposable TEAL artifacts without changing the cached corpus.

    The oracle assembles the patched TEAL source. The cached binary supplies
    the unpatched program's deployment-size estimate.
    """
    temp = tempfile.TemporaryDirectory(prefix="puya-cctp-historical-")
    root = Path(temp.name)
    patches = {}
    sites = {}
    for tag, config in CASE_CONFIG.items():
        out = root / tag / JOINT_ARTIFACT_DIR
        shutil.copytree(cases / tag / JOINT_ARTIFACT_DIR, out)
        approval = out / f"{config['contract']}.approval.teal"
        source, applied, applied_sites = apply_compat_shims(
            approval.read_text(), config["contract"]
        )
        approval.write_text(source)
        patches[tag] = applied
        sites[tag] = applied_sites
    return temp, root, patches, sites


# ── registered-artifact validation ──────────────────────────────────────────
# Joint ARC-4/slot artifacts have a separate directory from per-contract EVM
# artifacts. Never fall back to out_avm: sharing it previously let a different
# profile overwrite the joint method list or make the slot comparison empty.
# Check every registered artifact up front, including explicit upgrade paths.

ARTIFACT_SUFFIXES = ("approval.teal", "clear.teal", "approval.bin", "clear.bin", "arc56.json")


class ArtifactError(RuntimeError):
    """A registered artifact cannot serve the joint lane."""


def artifact_fix_command(cases: Path, tag: str) -> str:
    multifile = False
    try:
        multifile = bool(load_json(cases / tag / "case.json").get("multifile"))
    except Exception:
        pass
    if multifile:
        return f"python3 build_v2_avm.py {cases}"
    return f"python3 refresh_cctp_artifacts.py {tag} --cases {cases}"


def check_joint_artifact(
    cases: Path,
    tag: str,
    contract: str,
    *,
    signatures: list[str] | tuple[str, ...] = (),
    postinit_arity: int | None = None,
    artifact_dir: str | None = None,
    fix: str | None = None,
) -> dict[str, Any]:
    """Validate one registered artifact; ArtifactError carries the fix command.

    Checks, in order: the five artifact files exist; the ARC-56 is an ARC-4
    profile compile (an EVM-profile one exposes `__postInit` only); every
    signature the replay will encode resolves to exactly one ARC-56 method
    (name + arity, as method_for does), including `__postInit` at the
    constructor's arity; storage is EVM-slot backed (no named ARC-56 state).
    """
    out = cases / artifact_dir if artifact_dir is not None else cases / tag / JOINT_ARTIFACT_DIR
    fix = fix or artifact_fix_command(cases, tag)
    missing = [
        f"{contract}.{suffix}"
        for suffix in ARTIFACT_SUFFIXES
        if not (out / f"{contract}.{suffix}").exists()
    ]
    if missing:
        raise ArtifactError(f"{tag}/{contract}: {out} is missing {missing}. Fix: {fix}")
    arc56_path = out / f"{contract}.arc56.json"
    arc56 = load_json(arc56_path)
    names = [method["name"] for method in arc56.get("methods", [])]
    if set(names) <= {"__postInit"}:
        raise ArtifactError(
            f"{tag}/{contract}: {arc56_path} exposes only {names} — an EVM-profile "
            "(--contract-abi evm) compile. The joint lane requires its separate "
            "ARC-4/slot artifacts, not a copy of the per-contract out_avm directory. "
            f"Fix: {fix}"
        )
    unresolved = []
    for signature in signatures:
        try:
            method_for(arc56, signature)
        except ValueError as error:
            unresolved.append(f"{signature}: {error}")
    if postinit_arity is not None:
        signature = "__postInit(" + ",".join("_" for _ in range(postinit_arity)) + ")"
        try:
            method_for(arc56, signature)
        except ValueError as error:
            unresolved.append(f"{signature} [constructor arity {postinit_arity}]: {error}")
    if unresolved:
        raise ArtifactError(
            f"{tag}/{contract}: {len(unresolved)} signature(s) the replay must encode are "
            f"not in the ARC-56 method list ({len(names)} methods): "
            + "; ".join(unresolved[:6])
            + f". Fix: {fix}"
        )
    state = arc56.get("state") or {}
    named = sorted(
        {
            key
            for section in ("keys", "maps")
            for scope in (state.get(section) or {}).values()
            for key in scope
            if not key.startswith("__")
        }
    )
    if named:
        raise ArtifactError(
            f"{tag}/{contract}: compiled WITHOUT --evm-storage-layout (the ARC-56 "
            f"declares named state {named[:8]}); the joint storage lane decodes EVM slot "
            "pages ('p:'/'s:' boxes) and would compare nothing against the EVM leg. "
            f"Fix: {fix}"
        )
    return {
        "artifact": str(arc56_path),
        "methods": len(names),
        "signatures_checked": len(signatures) + (postinit_arity is not None),
        "storage_model": "evm-slots",
        "approval_bytes": (out / f"{contract}.approval.bin").stat().st_size,
    }


def validate_joint_artifacts(
    cases: Path,
    case_data: dict[str, "CaseData"] | None = None,
    tags: set[str] | None = None,
) -> dict[str, dict[str, Any]]:
    """Check every artifact the joint lane registers: cases, stub, upgrade eras.

    `tags` narrows the check (refresh_cctp_artifacts.py validates what it just
    compiled); the stub is checked whenever its source tag is in scope.
    """
    if case_data is None:
        case_data = {
            tag: CaseData.load(cases, tag)
            for tag in CASE_CONFIG
            if tags is None or tag in tags
        }
    report: dict[str, dict[str, Any]] = {}
    for tag, data in case_data.items():
        if tags is not None and tag not in tags:
            continue
        signatures = sorted(
            {
                call["sig"]
                for call in data.calls["calls"]
                if call.get("sig") and "#" not in call["hash"]
            }
        )
        signatures += [entry["sig"] for entry in INIT_CALLS if entry["tag"] == tag]
        report[tag] = check_joint_artifact(
            cases,
            tag,
            data.config["contract"],
            signatures=signatures,
            postinit_arity=len(data.calls["meta"]["ctor_args"]),
        )
    stub_tag = STUB_SOURCE["tag"]
    if tags is None or stub_tag in tags:
        report[f"stub:{stub_tag}"] = check_joint_artifact(
            cases,
            stub_tag,
            STUB_CONFIG["contract"],
            signatures=["mint(address,uint256)", "approve(address,uint256)"],
            fix=f"python3 refresh_cctp_artifacts.py {stub_tag} --cases {cases}",
        )
    for index, entry in enumerate(UPGRADES):
        if tags is not None and entry["tag"] not in tags:
            continue
        report[f"upgrade:{entry['tag']}#{index}"] = check_joint_artifact(
            cases,
            entry["tag"],
            entry["contract"],
            signatures=[entry["init_sig"]] if entry.get("init_sig") else [],
            artifact_dir=entry["avm_artifact"],
            fix=f"re-run gen_upgrades.py {entry['tag']} (README: Mid-history upgrades)",
        )
    return report

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
        # Fail before the prover is touched if any registered artifact is an
        # EVM-profile or named-cell compile (check_joint_artifact says how to fix).
        self.artifact_check = validate_joint_artifacts(cases_path, self.cases)
        self.stub_arc56 = load_json(
            cases_path / STUB_SOURCE["tag"] / JOINT_ARTIFACT_DIR / "StubERC20.arc56.json"
        )
        self._compat_temp = None
        self.compatibility_patches: dict[str, list[str]] = {}
        self.compatibility_sites: dict[str, dict[str, list[dict[str, Any]]]] = {}
        artifact_cases = cases_path
        if pre08_compat:
            (
                self._compat_temp,
                artifact_cases,
                self.compatibility_patches,
                self.compatibility_sites,
            ) = build_pre08_compat_artifacts(cases_path)
        self.artifacts = {
            data.config["contract"]: read_artifact(
                artifact_cases / tag / JOINT_ARTIFACT_DIR, data.config["contract"]
            )
            for tag, data in self.cases.items()
        }
        self.artifacts["StubERC20"] = read_artifact(
            cases_path / STUB_SOURCE["tag"] / JOINT_ARTIFACT_DIR, "StubERC20"
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
            if pre08_compat:
                source, applied, applied_sites = apply_compat_shims(source, contract)
                if applied:
                    label = f"upgrade:{entry['tag']}#{len(self.upgrade_artifacts)}"
                    self.compatibility_patches[label] = applied
                    self.compatibility_sites[label] = applied_sites
            self.upgrade_artifacts.append(
                (
                    {**read_artifact(art_dir, contract), "source": source},
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
            "amount": _app_account_funding(),
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
            final, record = self._run_with_read_budget(
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

    def _run_with_read_budget(
        self, state: Any, name: str, source: str, **fields: Any
    ) -> tuple[dict[str, Any], dict[str, Any]]:
        """run_with_resources, re-provisioning box READ budget on demand.

        The prover enforces the chain's box I/O budget (2048 bytes per box
        reference, consensus v41+): reading pre-existing boxes larger than the
        referenced budget rejects with ``read budget exceeded (N > M)``. The
        resource discovery loop only adds *named* references, so a call that
        reads big boxes (puya-sol page/holder boxes) needs empty budget refs;
        size them from the prover's own message and retry once.
        """
        response, record = self.api.run_with_resources(
            self.client, state, name, source, **fields
        )
        error = str(response.get("error") or "")
        match = re.search(r"read budget exceeded \((\d+) > (\d+)\)", error)
        if response.get("result") == "PANIC" and match:
            need = (int(match.group(1)) + 2047) // 2048 + 1
            # Distinct dummy names: a reference to an absent box is legal and
            # adds I/O budget, while the unified access list DEDUPS repeated
            # empty names (four "" refs collapsed to one 2048-byte budget).
            # Extend the STATE's tracked references — an explicit box_refs
            # field would replace them in OracleState.request and hide the
            # named boxes the discovery loop adds on later attempts.
            extra = [("ff" * 7) + f"{index:02x}" for index in range(need)]
            if "box_refs" in fields:
                fields["box_refs"] = list(fields["box_refs"]) + extra
            else:
                for key in extra:
                    if key not in state.box_refs:
                        state.box_refs.append(key)
            response, record = self.api.run_with_resources(
                self.client, state, name, source, **fields
            )
            record["read_budget_refs"] = need
        return response, record

    def run_resources(
        self, name: str, source: str, **fields: Any
    ) -> tuple[dict[str, Any], dict[str, Any]]:
        response, record = self._run_with_read_budget(
            self.world,
            name,
            source,
            unified_access=True,
            **fields,
        )
        if (
            response.get("result") == "PANIC"
            and ("access list needs" in response.get("error", "").lower()
                 or "read budget exceeded" in response.get("error", ""))
        ):
            response, record = self._run_with_read_budget(
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
        opup, inner_total = count_opup_txns(response.get("inners_after") or [])
        result["opup_inner_txns"] = opup
        result["inner_txns"] = inner_total
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
            if replayed % _progress_every() == 0:
                matched_so_far = sum(1 for r in self.results if r["matched_status"])
                print(
                    f"progress: {replayed} root calls replayed, "
                    f"{matched_so_far} matched so far (block {item['block']})",
                    file=sys.stderr,
                    flush=True,
                )
            if not matched and not self.continue_after_divergence:
                stopped = True
                break

        self.reclassify_payload_races()
        root_calls = sum(
            1 for item in historical_stream(self.cases) if item["kind"] == "call"
        )
        compared = [r for r in self.results if r.get("matched_status") is not None]
        skipped = [r for r in self.results if r.get("status") == "not-replayed"]
        mismatches = [r for r in compared if not r["matched_status"]]
        return {
            "scope": {
                "kind": "historical CCTP receipt-status replay",
                "real_contracts": [item["contract"] for item in CASE_CONFIG.values()],
                "root_fixture_calls": root_calls,
                "lifted_inner_calls_excluded": sum(
                    1
                    for data in self.cases.values()
                    for call in data.calls["calls"]
                    if "#" in call["hash"]
                ),
                "dependency_model": (
                    "StubERC20 with per-successful-deposit synthetic mint/approval"
                ),
                "app_account_funding_microalgo": _app_account_funding(),
                "comparison": "historical receipt status vs oracle ACCEPT/reject",
                "mainnet_receipt_metadata_corrections": MAINNET_RECEIPT_METADATA,
                "zero_log_receipt_corrections": self.receipt_corrections,
                "artifact_source": (
                    "cached corpus TEAL copied to a temporary directory and patched; "
                    "the cached corpus itself is unchanged"
                ),
                "pre08_compatibility_patches": self.compatibility_patches,
                "pre08_compatibility_sites": self.compatibility_sites,
                "artifact_check": self.artifact_check,
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
                # Root calls the loop never reached (a --limit, or a stop at
                # the first divergence).  Zero on a complete replay.
                "root_calls_not_reached": root_calls - len(self.results),
                "stopped_at_first_divergence": stopped,
                "tainted_suffix": self.tainted and self.continue_after_divergence,
            },
            "first_mismatch": mismatches[0] if mismatches else None,
            "initialization_and_dependency_steps": self.steps,
            "results": self.results,
            "final_storage": self.dump_slot_storage(),
            "budget_profile": self.budget_profile(),
        }

    def budget_profile(self) -> dict[str, Any]:
        """Per-method OpUp cost, so budget is visible in every run.

        The replay hands each call the protocol maximum pooled budget plus
        OpUp escalation, which means a green run cannot by itself show that a
        method is affordable. This surfaces what each entry point had to buy:
        methods needing zero OpUp run inside pooled budget, and the rest carry
        an inner transaction (and fee) per 700 opcodes bought.
        """
        from collections import defaultdict

        per_sig: dict[str, list[int]] = defaultdict(list)
        for r in self.results:
            if r.get("opup_inner_txns") is None or not r.get("signature"):
                continue
            per_sig[r["signature"]].append(int(r["opup_inner_txns"]))
        profile = {}
        for sig, counts in sorted(per_sig.items()):
            counts.sort()
            profile[sig] = {
                "calls": len(counts),
                "opup_max": counts[-1],
                "opup_median": counts[len(counts) // 2],
                "opup_total": sum(counts),
                # Each OpUp is a fee-paying inner txn at the 1000 microAlgo
                # minimum; this is the floor a real deployment pays for budget.
                "min_fee_microalgo_max_call": counts[-1] * 1000,
            }
        return {
            "ensure_budget_target": _ensure_budget_target(),
            "pooled_group_budget": 16 * 700,
            "note": (
                "opup counts reflect the budget REQUESTED at this target, not "
                "consumed; bisect CCTP_ENSURE_BUDGET to measure the requirement"
            ),
            "per_method": profile,
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
        help="avm-prover checkout (or set AVM_PROVER_ROOT)",
    )
    parser.add_argument(
        "--check-artifacts",
        action="store_true",
        help="only validate the registered joint artifacts (ARC-4 profile, full "
        "method list, --evm-storage-layout) and exit; no prover needed",
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
    if args.check_artifacts:
        try:
            checked = validate_joint_artifacts(args.cases.resolve())
        except ArtifactError as error:
            print(f"artifact check FAILED: {error}", file=sys.stderr)
            return 1
        for label, info in checked.items():
            print(
                f"artifact ok: {label}: {info['methods']} ARC-4 methods, "
                f"{info['signatures_checked']} signature(s) resolve, "
                f"{info['storage_model']}, {info['approval_bytes']} B approval"
            )
        return 0
    if args.prover_root is None:
        parser.error("--prover-root (or AVM_PROVER_ROOT) is required to replay")

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
    # A stop at the first divergence is reported even when that row was later
    # reclassified as an identical-payload race: "853/853" with 3,976 root
    # calls never reached is not a full replay.
    incomplete = ""
    if summary["root_calls_not_reached"]:
        why = (
            "stopped at the first divergence"
            if summary["stopped_at_first_divergence"]
            else "--limit"
        )
        incomplete = (
            f"; INCOMPLETE: {summary['root_calls_not_reached']} of "
            f"{report['scope']['root_fixture_calls']} root calls not reached ({why}"
            + ("; rerun with --continue-after-divergence" if summary["stopped_at_first_divergence"] else "")
            + ")"
        )
    print(
        "historical CCTP: "
        f"{summary['matched_statuses']}/{summary['compared_statuses']} statuses match; "
        f"{summary['not_replayed']} not replayed; "
        f"{summary['status_mismatches']} mismatch(es)" + incomplete,
        file=sys.stderr,
    )
    return 1 if summary["status_mismatches"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
