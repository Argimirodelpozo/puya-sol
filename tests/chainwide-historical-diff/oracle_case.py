#!/usr/bin/env python3
"""AVM leg on avm-prover's canonical oracle — per-contract replay, no LocalNet.

  python3 oracle_case.py <case_dir> ['<json opts>'] [--evm-storage-layout]
                          [--oracle PATH] [--prover-root PATH]

Same inputs as avm_leg.py (cases/<tag>/{case,registry,calls}.json plus the EVM
leg's evm_results.json for the shared clock), the same compile path (the
framework compile avm_leg factored out — PUYA_SOL_EXTRA_ARGS honoured through
compile_sol), and the same avm_results.json shape, so `python3 differ.py
<case_dir>` diffs the result unchanged. The JSON opts are avm_leg's (`skips`,
`time_base`, `deployment_time`, `evm_layout`); a missing clock falls back to
the schedule the EVM leg recorded, then to the history's own instants.

How the LocalNet lane's moving parts map onto the oracle (go-algorand's own
BlockEvaluator over an in-memory ledger, one request per transaction group,
committed state carried between requests by OracleState):

  deploy            one `creating` request (ctor args on the create txn, then
                    `__postInit` in a pooled group); the app is the oracle's
                    fixed app 9001, its escrow folds to «self»;
  sender identity   the xchain LogicSig account A(E) is just a 32-byte sender
                    here (the oracle verifies no signatures), so the compiled
                    router adopts the claimed historical address exactly as on
                    LocalNet — and the creator IS the historical creator;
  block clock       `latest_timestamp` is pinned PER REQUEST from the shared
                    schedule, so a time-gated history replays at its true
                    instants whatever ran before; rounds are a small counter
                    (+2 per call: seal + call, like LocalNet), not history;
  budget/resources  every call runs in a full 16-txn group with 15 EXECUTED
                    helper calls (LocalNet's OpUp helper twins, apps 8001/8002):
                    plain first, OpUp-amplified on a budget error — the two
                    tiers framework.call retries through. Box references are
                    discovered from the oracle's own `invalid Box reference`
                    panics (a failed attempt commits nothing) and packed across
                    the group's txns; every spare reference slot carries an
                    empty ref for I/O budget, because the oracle registers the
                    approval program as the clear program too and so charges a
                    >8 KiB program's bytes twice against the read budget;
  storage           the carried globals/boxes feed the SAME readers avm_leg
                    uses (decode_global_state / read_native_maps /
                    read_slot_storage) via chd_box_source.OracleBoxSource,
                    holder-mismatch root check included;
  dependencies      each recorded dependency is compiled and deployed like the
                    main app in a scratch ledger of its own, then imported as a
                    callable app 9002+ (`absorb_current_as`), so the contract's
                    inner `appl` to bzero(24) ‖ itob(id) reaches it. A recorded
                    ANSWER TAPE is loaded into its stand-in through the same
                    `__load` the LocalNet lane uses; `__seek(start,end)` rides
                    in the replayed call's OWN group (one sibling per active
                    tape) instead of a separate transaction, so a rejected call
                    rolls the cursor back with it.

Not covered — refused loudly rather than approximated: split/delegate code
pages, child programs via box, `new C()` children (CH<n> symbols), and a
stand-in still carrying the pre-selector answer tape (`fetch.py
--refresh-stubs <tag>` regenerates it).
"""
from __future__ import annotations

import base64
import hashlib
import importlib
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path
from types import SimpleNamespace

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parents[0] / "solidity-semantic-tests"))
sys.path.insert(0, str(HERE.parents[0] / "WIP" / "tiny-fuzzing-oracle"))

from eth_abi import decode as evm_abi_decode, encode as evm_abi_encode

from algosdk import account, encoding
from algosdk.atomic_transaction_composer import (AccountTransactionSigner,
                                                 AtomicTransactionComposer)
from algosdk.transaction import SuggestedParams

from avm_leg import (XCHAIN_PLACEHOLDER, XCHAIN_TOY_TEAL, _ctype, _ret,
                     collect_compiler_events, compile_case_contract,
                     compiled_artifact_root, decode_global_state,
                     evm_selector, evm_wire_value, main_compile_args,
                     mode_compile_args, read_native_maps, xchain_compile_args,
                     xchain_template_bytes)
from chd_box_source import OracleBoxSource, slot_map_from_boxes
from chd_common import (arg_content20, build_dep_tape_plans, canon_value,
                        deployment_clock_target, dump_json, is_platform_limit,
                        load_json, probe_clock_target, replay_clock_targets,
                        replay_epoch, symbol, tape_script_chunks)
from framework import Harness
from framework.call import _resolve_method
from framework.deploy import _encode_ctor_args, _load_arc56, _zero_for_type
from event_diff import decode_avm_log_bytes

DEFAULT_PROVER_ROOT = HERE.parents[3] / "new_verifier_experiment" / "avm-prover"

ORACLE_APP = 9001                    # the oracle's fixed id for the app under eval
DEP_APP_BASE = 9002                  # dependency contracts: 9002, 9003, …
HELPER_APP, TARGET_APP = 8001, 8002  # LocalNet's OpUp helper + the bare app it calls
POOL = 15                            # helper calls per group (16 minus the call)
OPUP_DEPTH = 8                       # inner calls per amplified helper (framework.call)
MIN_FEE = 1_000
EXTRA_FEE = 20_000                   # avm_leg's per-call fee headroom
INNER_FEE_HEADROOM = 16_000
MAX_TXN_REFS = 8                     # MaxAppTotalTxnReferences
MAX_TXN_ACCOUNTS = 4                 # MaxAppTxnAccounts
SCHEMA_PAD = 16                      # framework.deploy's spare global cells
MAX_SCHEMA_CELLS = 64                # consensus max global entries per kind
RETRY_CAP = 96                       # discovery attempts per call
MEMO_CAP = 48                        # recently used box names seeded on every call
ROUND0 = 1_000
FUNDING = 10 ** 12                   # money is free here; MBR is never a finding
BATCH = 16                           # read requests per oracle process

TARGET_TEAL = "#pragma version 10\nint 1"
HELPER_TEAL = f"""#pragma version 10
txn NumAppArgs
bz done
txna ApplicationArgs 0
btoi
store 0
loop:
load 0
bz done
itxn_begin
int appl
itxn_field TypeEnum
int {TARGET_APP}
itxn_field ApplicationID
int 0
itxn_field Fee
itxn_submit
load 0
int 1
-
store 0
b loop
done:
int 1"""

INVALID_BOX = re.compile(r"invalid Box reference 0x([0-9a-fA-F]*)")
UNAVAILABLE_ACCOUNT = re.compile(r"unavailable Account ([A-Z2-7]{58})")
IO_BUDGET = re.compile(r"(?:read|write) budget exceeded \((\d+) > (\d+)\)")
RET_MAGIC = bytes.fromhex("151f7c75")


def _itob(n: int) -> str:
    return int(n).to_bytes(8, "big").hex()


def _is_budget_error(err: str) -> bool:
    """framework.call._is_budget_error, on the oracle's error text."""
    m = err.lower()
    return "budget" in m or "opcode" in m or "dynamic cost" in m


def inner_application_calls(txns: list[dict]) -> dict[str, int]:
    """Count submitted calls by existing app ID through the entire inner tree.

    Keep budget-helper IDs visible; callers can distinguish them from the
    recorded dependency IDs. This measures observed calls, not opcode cost or
    a complete trace of work discarded by a failing group.
    """
    counts: dict[str, int] = {}
    for txn in txns:
        app_id = int((txn.get("u64") or {}).get("ApplicationID") or 0)
        if app_id:
            key = str(app_id)
            counts[key] = counts.get(key, 0) + 1
        for key, count in inner_application_calls(txn.get("inner_txns") or []).items():
            counts[key] = counts.get(key, 0) + count
    return counts


def global_schema_for(app_spec, name: str) -> tuple[int, int]:
    """(uints, byte-slices) for an app's global schema.

    The ARC-56 declaration padded to framework.deploy's 16/16 floor, so small
    contracts keep the exact schema the LocalNet lane deploys while a contract
    that declares MORE named globals than that gets what it declares. A flat
    16/16 made every such contract undeployable here (cases/toshi declares
    4 uint / 24 byte-slice and died on "store bytes count 17 exceeds schema
    bytes count 16" inside __postInit).
    """
    declared = app_spec.state.schema.global_state
    uints, byteslices = int(declared.ints), int(declared.bytes)
    if uints > MAX_SCHEMA_CELLS or byteslices > MAX_SCHEMA_CELLS:
        raise RuntimeError(
            f"{name}: ARC-56 global schema {uints} uint / {byteslices} "
            f"byte-slice exceeds the AVM maximum of {MAX_SCHEMA_CELLS} per "
            f"kind — this contract cannot be deployed on any lane")
    return max(uints, SCHEMA_PAD), max(byteslices, SCHEMA_PAD)


def app_address_hex(app_id: int) -> str:
    """go-algorand's application escrow address, as the oracle spells it."""
    return hashlib.new("sha512_256",
                       b"appID" + int(app_id).to_bytes(8, "big")).hexdigest()


def arc4_app_args(abi_method, values) -> list[str]:
    """App args for one ARC-4 method call, ATC-encoded offline (selector +
    ARC4 args, >14-arg tuple packing included)."""
    sk, addr = account.generate_account()
    sp = SuggestedParams(fee=MIN_FEE, first=1, last=1000,
                         gh=base64.b64encode(bytes(32)).decode(), flat_fee=True)
    atc = AtomicTransactionComposer()
    atc.add_method_call(app_id=ORACLE_APP, method=abi_method, sender=addr, sp=sp,
                        signer=AccountTransactionSigner(sk),
                        method_args=list(values))
    return [a.hex() for a in atc.build_group()[0].txn.app_args]


def load_state_adapter(prover_root: Path):
    oracle_dir = prover_root.resolve() / "oracle"
    if not (oracle_dir / "state_adapter.py").exists():
        raise FileNotFoundError(f"not an avm-prover checkout: {prover_root}")
    sys.path.insert(0, str(oracle_dir))
    return importlib.import_module("state_adapter")


class Oracle:
    """The avmoracle binary: one process per (batch of) request(s)."""

    def __init__(self, binary: Path):
        if not binary.exists():
            raise FileNotFoundError(f"oracle binary not found: {binary}")
        self.binary = binary
        self.requests = 0
        self.processes = 0
        self.seconds = 0.0

    def run_batch(self, requests: list[dict]) -> list[dict]:
        t0 = time.time()
        proc = subprocess.run([str(self.binary)], input=json.dumps(requests),
                              capture_output=True, text=True, timeout=900,
                              check=False)
        self.seconds += time.time() - t0
        self.requests += len(requests)
        self.processes += 1
        if proc.returncode:
            raise RuntimeError(f"oracle exited {proc.returncode}: {proc.stderr[:500]}")
        out = json.loads(proc.stdout)
        if not isinstance(out, list):
            out = [out]
        for resp in out:
            err = str(resp.get("error") or "")
            # Harness faults must never read as contract reverts.
            if err.startswith("oracle-internal:") or resp.get("result") == "ASSEMBLE_ERROR":
                raise RuntimeError(f"oracle: {resp.get('result')}: {err[:400]}")
        return out

    def run(self, request: dict) -> dict:
        return self.run_batch([request])[0]


class Identities:
    """avm_leg's sender/argument address model without accounts to fund.

    Every historical sender i is the xchain LogicSig account owned by its
    20-byte address: A(E) = sha512_256("Program" ‖ template-with-E-spliced),
    and the call carries E as the owner claim. The creator is the historical
    creator's zero-extended address (LocalNet uses its dispenser; both fold to
    «C»). Arg-only symbols keep their deterministic content addresses.
    """

    def __init__(self, reg: dict, calls: list, template: bytes):
        self.reg = reg
        self.template = template
        # historical dependency address → the app id its stand-in was given.
        # Assigned before any dependency is deployed so a dependency's OWN
        # constructor arguments can name a later one.
        self.dep_apps: dict[str, int] = {}
        self.creator20 = bytes.fromhex(reg["creator"][2:])
        self.creator32 = bytes(12) + self.creator20
        self.creator_hex = self.creator32.hex()
        idx_owner: dict[int, bytes] = {}
        for addr, i in (reg.get("senders") or {}).items():
            idx_owner[i] = bytes.fromhex(addr[2:])
        for addr, i in (reg.get("args") or {}).items():
            idx_owner.setdefault(i, bytes.fromhex(addr[2:]))
        self.accts: dict[int, SimpleNamespace] = {}
        for i in reg["senders"].values():
            self.accts[i] = self._xchain(idx_owner.get(i) or arg_content20(i))
        # Symbols that only ever appear as arguments still SEND on some
        # histories — materialise them too (avm_leg does the same).
        for c in calls:
            m = (c.get("sender") or {}).get("__addr__")
            if isinstance(m, int) and m not in self.accts:
                self.accts[m] = self._xchain(idx_owner.get(m) or arg_content20(m))

    def _xchain(self, owner20: bytes) -> SimpleNamespace:
        program = self.template.replace(XCHAIN_PLACEHOLDER, owner20)
        address = hashlib.new("sha512_256", b"Program" + program).digest()
        return SimpleNamespace(owner=owner20, address=address)

    def sender(self, marker) -> tuple[str, bytes | None]:
        """(oracle sender hex32, owner claim or None) for a call's sender marker."""
        if isinstance(marker, int) and marker in self.accts:
            acct = self.accts[marker]
            return acct.address.hex(), acct.owner
        return self.creator_hex, None

    def concrete_addr(self, m) -> str:
        if m == "Z":
            return encoding.encode_address(bytes(32))
        if m == "C":
            return encoding.encode_address(self.creator32)
        if isinstance(m, int) and m in self.accts:
            return encoding.encode_address(bytes(12) + self.accts[m].owner)
        if isinstance(m, int):
            return encoding.encode_address(bytes(12) + arg_content20(m))
        return encoding.encode_address(bytes(32))

    def dep_address(self, addr: str) -> str:
        """puya-sol's cross-contract value for a dependency: bzero(24) ‖ itob(id)."""
        app = self.dep_apps.get(str(addr).lower())
        if app is None:
            raise NotImplementedError(
                f"dependency {addr} has no stand-in on this lane — it is not in "
                f"calls.json meta.dep_ctors, so a call reaching it would run "
                f"against an empty account instead of the recorded contract")
        return encoding.encode_address(bytes(24) + app.to_bytes(8, "big"))

    def resolve(self, v):
        if isinstance(v, dict) and set(v) == {"__dep__"}:
            return self.dep_address(v["__dep__"])
        if isinstance(v, dict) and set(v) == {"__addr__"}:
            return self.concrete_addr(v["__addr__"])
        if isinstance(v, dict) and set(v) == {"__b__"}:
            return bytes.fromhex(v["__b__"])
        if isinstance(v, list):
            return [self.resolve(x) for x in v]
        return v

    def inverse_fold_table(self, app_addr32: bytes) -> dict[str, str]:
        """32-byte content hex → registry symbol (avm_leg's `inv`)."""
        inv = {self.creator32.hex(): symbol("C"),
               app_addr32.hex(): symbol("self"),
               # The contract-value form of the app itself (bzero24 ‖ itob id).
               (bytes(24) + ORACLE_APP.to_bytes(8, "big")).hex(): symbol("self"),
               bytes(32).hex(): symbol("Z")}
        for i, a in self.accts.items():
            inv[a.address.hex()] = symbol(i)
            inv[(bytes(12) + a.owner).hex()] = symbol(i)
        for _a, i in self.reg["args"].items():
            inv[(bytes(12) + arg_content20(i)).hex()] = symbol(i)
        for a, i in (self.reg.get("deps") or {}).items():
            app = self.dep_apps.get(a.lower())
            if app is None:
                continue
            # Both value forms of a dependency app, as avm_leg folds them: the
            # contract value the caller passes around, and the ESCROW address a
            # stand-in's `address(this)` answers from assembly.
            inv[(bytes(24) + app.to_bytes(8, "big")).hex()] = symbol(f"D{i}")
            inv[app_address_hex(app)] = symbol(f"D{i}")
        # A Solidity `address` keeps 20 bytes: fold the truncated forms too.
        for k in list(inv):
            inv.setdefault((bytes(12) + bytes.fromhex(k)[-20:]).hex(), inv[k])
        return inv

    def storage_symbols(self) -> dict[str, bytes]:
        """avm_leg's `syms`: symbol → 32-byte AVM content, same construction order."""
        syms = {symbol("C"): self.creator32, symbol("Z"): bytes(32)}
        for i, a in self.accts.items():
            syms[symbol(i)] = bytes(12) + a.owner
        for _ad, i in self.reg["args"].items():
            syms[symbol(i)] = bytes(12) + arg_content20(i)
        for a, i in (self.reg.get("deps") or {}).items():
            app = self.dep_apps.get(a.lower())
            if app is not None:
                syms[symbol(f"D{i}")] = bytes(24) + app.to_bytes(8, "big")
        return syms


class OracleLane:
    """The case's app (9001) and LocalNet's budget-helper twins (8001/8002) in
    one oracle ledger, with the committed state carried between requests."""

    def __init__(self, oracle: Oracle, adapter, approval_teal: str, clear_teal: str,
                 approval_bin: bytes, clear_bin: bytes, creator_hex: str,
                 schema: tuple[int, int] = (SCHEMA_PAD, SCHEMA_PAD)):
        self.oracle, self.adapter = oracle, adapter
        self.global_uints, self.global_bytes = schema
        self.approval, self.clear = approval_teal, clear_teal
        self.approval_bin, self.clear_bin = approval_bin, clear_bin
        total = len(approval_bin) + len(clear_bin)
        page = 2048
        # framework.deploy: pages for the SUM of both programs; v42 charges the
        # bytes above four pages against the create txn's box I/O budget.
        self.extra_pages = max(0, (total - 1) // page)
        if self.extra_pages > 7:
            raise RuntimeError(
                f"program exceeds AVM 16KB cap: approval={len(approval_bin)}B + "
                f"clear={len(clear_bin)}B needs extra_pages={self.extra_pages} (max 7)")
        self.write_budget_refs = (max(0, total - 4 * page) + page - 1) // page
        self.creator = creator_hex
        self.app_addr = adapter.app_address(ORACLE_APP)
        self.state = adapter.OracleState()
        self.state.register_application({
            "app": HELPER_APP, "creator": creator_hex,
            "approval_source": HELPER_TEAL, "clear_state_source": TARGET_TEAL})
        self.state.register_application({
            "app": TARGET_APP, "creator": creator_hex,
            "approval_source": TARGET_TEAL, "clear_state_source": TARGET_TEAL})
        self.fund(creator_hex)
        self.fund(self.app_addr)
        self.round = ROUND0
        self.accounts_found: list[str] = []
        self.memo: list[tuple[int, str]] = []
        # Dependency apps this lane must keep REACHABLE (an inner `appl` needs
        # its callee among the group's application resources) and the owner of
        # every box the dependencies brought with them — the oracle's box panic
        # names the box, never its app.
        self.dep_apps: list[int] = []
        self.box_owner: dict[str, int] = {}
        self.last_accept: dict = {}
        self.stats = {"calls": 0, "attempts": 0, "discoveries": 0,
                      "amplified": 0, "seed_drops": 0}

    # ── ledger plumbing ───────────────────────────────────────────────────
    def fund(self, account_hex: str) -> None:
        self.state.balances.setdefault(
            (account_hex,), {"account": account_hex, "amount": FUNDING})

    def _app_fields(self) -> dict:
        # In whole-group mode the current app's params (schema, pages) come
        # from the request itself on EVERY call, so they must match what
        # create() registered — no locals, like framework.deploy.
        return {"execute_group": True,
                "global_num_uint": self.global_uints,
                "global_num_byteslice": self.global_bytes,
                "local_num_uint": 0, "local_num_byteslice": 0,
                "extra_program_pages": self.extra_pages,
                "clear_source": self.clear}

    def create(self, app_args_hex: list[str], ts: int, *,
               deferred_constructor: bool = False) -> None:
        """framework.deploy's create txn: ctor args as app args, no group."""
        self.state.latest_timestamp = int(ts)
        req = self.state.request(
            self.approval, creating=True, sender=self.creator, fee=8 * MIN_FEE,
            app_args=list(app_args_hex or []),
            box_refs=[""] * self.write_budget_refs, round=self.round,
            **self._app_fields())
        for key in ("accounts", "foreign_assets", "foreign_apps", "foreign_box_refs"):
            req.pop(key, None)
        # The compiled ARC-56 tells us whether the constructor runs later in
        # __postInit. Do not name unused dependencies on that create txn:
        # v42 charges their program-read budget even without executing them.
        # __postInit's pooled group declares and budgets its callees normally.
        if self.dep_apps and not deferred_constructor:
            req["foreign_apps"] = self.dep_apps[:MAX_TXN_REFS - self.write_budget_refs]
        resp = self.oracle.run(req)
        if resp.get("result") != "ACCEPT":
            raise RuntimeError(f"create txn failed: {resp.get('error')}")
        self.state.carry(resp)
        self.last_accept = resp
        # Keep the app registered for later requests (the oracle omits its own
        # app from app_params_after; whole-group calls re-derive it anyway).
        self.state.register_application({
            "app": ORACLE_APP, "creator": self.creator,
            "approval_program": self.approval_bin.hex(),
            "clear_state_program": self.clear_bin.hex(),
            "global_num_uint": self.global_uints,
            "global_num_byteslice": self.global_bytes,
            "extra_program_pages": self.extra_pages})
        self.round += 1

    # ── one transaction group ─────────────────────────────────────────────
    def _siblings(self, sender: str, count: int, amplify: bool) -> list[dict]:
        depth = OPUP_DEPTH if amplify else 0
        out = []
        for k in range(count):
            # arg 1 makes the otherwise identical helper txns distinct (the
            # evaluator rejects duplicate txids inside one group; LocalNet
            # uses random notes for the same reason).
            item = {"type_enum": 6,
                    "u64": {"ApplicationID": HELPER_APP, "Fee": 0},
                    "addr": {"Sender": sender},
                    "app_args": [_itob(depth), _itob(k)]}
            if amplify:
                item["foreign_apps"] = [TARGET_APP]
            out.append(item)
        return out

    def build(self, sender: str, app_args_hex: list[str],
              refs: list[tuple[int, str]], *, ts: int, value: int = 0,
              amplify: bool = False, extra: list[dict] = ()) -> dict | None:
        """The 16-txn group request, or None when the references overflow it.

        Shape mirrors framework.call's pooled retry: helpers FIRST (pooled
        budget accrues before the call runs), the msg.value payment immediately
        before the app call, the app call last. `extra` siblings (dependency
        tape seeks) replace helper slots, so the group stays inside the 16-txn
        limit and their effect commits — or rolls back — with the call itself.

        A reference is (app, box name), app 0 meaning the contract under test.
        Its own boxes fill the call txn's 8 compact slots first; everything
        else rides on the helpers as foreign refs, each distinct owner costing
        one slot on top. Dependency apps are named as resources whether or not
        they own a box, because an inner `appl` needs its callee available.
        Every remaining slot is an empty ref, +2048 bytes of I/O budget each.
        """
        extra = list(extra or ())
        count = POOL - (1 if value else 0) - len(extra)
        if count < 0:
            return None
        helpers = self._siblings(sender, count, amplify)
        sibs = helpers + extra
        if value:
            sibs.append({"type_enum": 1,
                         "u64": {"Amount": int(value), "Fee": MIN_FEE},
                         "addr": {"Sender": sender, "Receiver": self.app_addr}})
        depth = OPUP_DEPTH if amplify else 0
        fee = (MIN_FEE * (len(sibs) + 1) + EXTRA_FEE + MIN_FEE * count * depth
               + INNER_FEE_HEADROOM)
        main_refs: list[str] = []
        pending: list[tuple[int, str]] = []
        # Accounts and boxes share the call txn's 8 reference slots, so every
        # discovered account costs one box slot; the rest spill to the helpers
        # below exactly as an over-long box list already does.
        main_cap = MAX_TXN_REFS - len(self.accounts_found)
        for app, key in refs:
            app = ORACLE_APP if app in (0, ORACLE_APP) else app
            if app == ORACLE_APP and len(main_refs) < main_cap:
                main_refs.append(key)
            else:
                pending.append((app, key))
        # Group a dependency's boxes together: one owner slot then its refs.
        pending.sort(key=lambda ref: ref[0])
        declare = list(self.dep_apps)
        for item in sibs:
            if item["type_enum"] != 6:
                continue
            apps = list(item.get("foreign_apps") or [])
            spare = MAX_TXN_REFS - len(apps)
            box_refs = []
            while pending:
                app, key = pending[0]
                need = (0 if app in apps else 1) + 1
                if need > spare:
                    break
                if app not in apps:
                    apps.append(app)
                    spare -= 1
                box_refs.append({"app": app, "key": key})
                spare -= 1
                pending.pop(0)
            while declare and spare:
                app = declare.pop(0)
                if app in apps:
                    continue
                apps.append(app)
                spare -= 1
            if box_refs:
                item["foreign_box_refs"] = box_refs
            if apps:
                item["foreign_apps"] = apps
            item["box_refs"] = [""] * spare
        if pending or declare:
            return None
        self.state.latest_timestamp = int(ts)
        req = self.state.request(
            self.approval, sender=sender, fee=fee, app_args=list(app_args_hex),
            group=sibs, group_index=len(sibs), round=self.round,
            **self._app_fields())
        for key in ("accounts", "foreign_assets", "foreign_apps", "box_refs",
                    "foreign_box_refs"):
            req.pop(key, None)
        req["box_refs"] = main_refs + [""] * max(
            0, MAX_TXN_REFS - len(main_refs) - len(self.accounts_found))
        # Accounts the call proved it touches (discovered from the oracle's
        # own `unavailable Account` panic, the same way boxes are). A payment
        # to an address the contract computes — FriendTech paying the shares
        # subject and the protocol fee recipient — is unavailable until named,
        # and LocalNet never showed this because algod's simulate populates
        # the resource arrays for us.
        if self.accounts_found:
            req["accounts"] = list(self.accounts_found)
        return req

    def _attribute(self, key: str, found: list[tuple[int, str]]):
        """Which app owns the box the oracle just refused?

        Its panic names the box, never its owner. A box a dependency brought in
        with it is known outright; anything else is tried against the contract
        under test first and then each dependency in turn — one extra attempt
        per candidate, which is what makes a box a dependency CREATES during
        the call (or a name both apps happen to use) still resolve.
        """
        hinted = self.box_owner.get(key)
        candidates = ([hinted] if hinted is not None else []) + [ORACLE_APP]
        candidates += [app for app in self.dep_apps if app != hinted]
        for app in candidates:
            if (app, key) not in found:
                return (app, key)
        return None

    def call(self, sender: str, app_args_hex: list[str], *, ts: int,
             value: int = 0, commit: bool = True,
             extra: list[dict] = ()) -> tuple[bool, dict, dict]:
        """One app call with resource discovery; commits on ACCEPT when asked."""
        self.fund(sender)
        self.stats["calls"] += 1
        # Per call: the addresses this one touches are its own (each FriendTech
        # buy pays a different subject), and the txn holds at most four.
        self.accounts_found = []
        seeds = list(self.memo)
        found: list[tuple[int, str]] = []
        amplify = False
        resp: dict = {}
        attempt = 0
        for attempt in range(1, RETRY_CAP + 1):
            refs = found + [s for s in seeds if s not in found]
            req = self.build(sender, app_args_hex, refs, ts=ts, value=value,
                             amplify=amplify, extra=extra)
            if req is None:
                if seeds:
                    seeds = []
                    continue
                resp = {"result": "PANIC", "error": (
                    f"invalid box reference capacity: {len(refs)} named boxes "
                    f"exceed the 16-txn group's reference slots")}
                break
            self.stats["attempts"] += 1
            resp = self.oracle.run(req)
            if resp.get("result") == "ACCEPT":
                break
            err = str(resp.get("error") or "")
            m = INVALID_BOX.search(err)
            if m:
                ref = self._attribute(m.group(1).lower(), found) if m.group(1) else None
                if ref is not None:
                    found.append(ref)
                    self.stats["discoveries"] += 1
                    continue
                break
            m = UNAVAILABLE_ACCOUNT.search(err)
            if m and encoding.decode_address(m.group(1)).hex() not in self.accounts_found:
                if len(self.accounts_found) >= MAX_TXN_ACCOUNTS:
                    resp = {"result": "PANIC", "error": (
                        f"account reference capacity: this call needs more than "
                        f"{MAX_TXN_ACCOUNTS} accounts ({self.accounts_found} + "
                        f"{m.group(1)})")}
                    break
                # The panic renders the address in base32; the request field
                # carries the raw 32-byte public key as hex, like every other
                # account in this lane.
                self.accounts_found.append(encoding.decode_address(m.group(1)).hex())
                self.stats["account_discoveries"] = (
                    self.stats.get("account_discoveries", 0) + 1)
                continue
            if IO_BUDGET.search(err):
                # Seeded (existing) boxes are charged against the read budget
                # whether or not the call touches them: retry with only the
                # refs this call proved it needs.
                if seeds:
                    seeds = []
                    self.stats["seed_drops"] += 1
                    continue
                break
            if _is_budget_error(err) and not amplify:
                amplify = True
                self.stats["amplified"] += 1
                continue
            break
        ok = resp.get("result") == "ACCEPT"
        if ok:
            for ref in reversed(found):
                if ref in self.memo:
                    self.memo.remove(ref)
                self.memo.insert(0, ref)
            del self.memo[MEMO_CAP:]
            if commit:
                self.state.carry(resp)
                self.last_accept = resp
        return ok, resp, {"attempts": attempt, "refs": len(found), "amplified": amplify}

    def read_many(self, items: list[tuple[str, list[str]]], *, ts: int,
                  extra: list[dict] = ()) -> list[dict]:
        """Uncommitted reads, batched per oracle process; a read that trips a
        resource/budget limit is re-run alone through the discovery loop."""
        out: list[dict] = []
        for start in range(0, len(items), BATCH):
            chunk = items[start:start + BATCH]
            reqs = []
            for sender, app_args in chunk:
                self.fund(sender)
                reqs.append(self.build(sender, app_args, list(self.memo), ts=ts,
                                       extra=extra))
            self.stats["attempts"] += len(reqs)
            for (sender, app_args), resp in zip(chunk, self.oracle.run_batch(reqs)):
                err = str(resp.get("error") or "")
                if (resp.get("result") != "ACCEPT"
                        and (INVALID_BOX.search(err) or IO_BUDGET.search(err)
                             or _is_budget_error(err))):
                    _ok, resp, _info = self.call(sender, app_args, ts=ts,
                                                 commit=False, extra=extra)
                out.append(resp)
        return out

    # ── carried state → storage readers ───────────────────────────────────
    def global_entries(self) -> list[tuple[bytes, int | bytes]]:
        out = []
        for item in self.state.globals:
            key = bytes.fromhex(item["key"])
            if item.get("uint") is not None:
                out.append((key, int(item["uint"])))
            else:
                out.append((key, bytes.fromhex(item.get("bytes") or "")))
        return out

    def box_source(self) -> OracleBoxSource:
        return OracleBoxSource(self.state.boxes)


# ── dependency contracts ─────────────────────────────────────────────────
TAPE_LOAD_SIG = "__load(bytes32[],uint256[],bytes32[])"
TAPE_SEEK_SIG = "__seek(uint256,uint256)"


class DepApp:
    """One recorded dependency, deployed as its own app in the oracle ledger.

    It is compiled and initialised in a scratch ledger where it IS app 9001 —
    the only id the oracle creates — and then imported into the ledger of the
    contract under test under a stable id of its own. puya-sol's cross-contract
    convention does the rest: the address value bzero(24) ‖ itob(id) the main
    contract holds becomes an inner `appl` to exactly that app.
    """

    def __init__(self, addr: str, name: str, app_id: int, lane: OracleLane,
                 app_spec):
        self.addr, self.name, self.app_id = addr.lower(), name, app_id
        self.lane, self.app_spec = lane, app_spec

    def export(self, state) -> None:
        """Publish program, schema and committed state into the main ledger."""
        state.register_application({
            "app": self.app_id, "creator": self.lane.creator,
            "approval_program": self.lane.approval_bin.hex(),
            "clear_state_program": self.lane.clear_bin.hex(),
            "global_num_uint": self.lane.global_uints,
            "global_num_byteslice": self.lane.global_bytes,
            "extra_program_pages": self.lane.extra_pages})
        state.absorb_current_as(self.lane.last_accept, self.app_id)
        escrow = app_address_hex(self.app_id)
        state.balances[(escrow,)] = {"account": escrow, "amount": FUNDING}

    def box_keys(self) -> list[str]:
        return [str(item["key"]).lower() for item in self.lane.state.boxes]

    def method(self, signature: str, arity: int):
        """The dependency's ARC-4 method for a Solidity signature, or None.

        Arity is checked because _resolve_method falls back to the only method
        of that NAME: a stand-in still carrying the pre-selector answer tape has
        a two-argument __load, and silently encoding three arguments into it
        would load garbage instead of failing.
        """
        method = _resolve_method(self.app_spec, signature)
        return method if method is not None and len(method.args) == arity else None

    def seek_txn(self, sender: str, start: int, end: int) -> dict:
        """A `__seek(start,end)` sibling for one replayed call's own group."""
        return {"type_enum": 6,
                "u64": {"ApplicationID": self.app_id, "Fee": 0},
                "addr": {"Sender": sender},
                "app_args": arc4_app_args(self.method(TAPE_SEEK_SIG, 2),
                                          [int(start), int(end)])}


def _dep_lane(oracle: Oracle, adapter, h, sol: Path, name: str | None,
              compile_args: list[str], ids: "Identities", ctor_markers: list,
              ts: int, case: dict | None = None) -> tuple[OracleLane, str, object]:
    """Compile one dependency source and run its create + __postInit."""
    artifacts = (compile_case_contract(h, sol.parent, case, compile_args)
                 if case is not None else h.compile(sol, extra_args=compile_args))
    picked = artifacts.last_deployable(name)
    if picked is None:
        raise RuntimeError(f"no deployable contract compiled from {sol}")
    artifact = artifacts.by_contract[picked]
    app_spec = _load_arc56(artifact["arc56"])
    directory = artifact["approval_teal"].parent
    lane = OracleLane(
        oracle, adapter, artifact["approval_teal"].read_text(),
        artifact["clear_teal"].read_text(),
        (directory / f"{picked}.approval.bin").read_bytes(),
        (directory / f"{picked}.clear.bin").read_bytes(), ids.creator_hex,
        schema=global_schema_for(app_spec, f"dependency {picked}"))
    ctor_values = [ids.resolve(m) for m in (ctor_markers or [])] or None
    evm_abi = (case["abi"] if case is not None and
               any(compile_args[i:i + 2] == ["--contract-abi", "evm"]
                   for i in range(len(compile_args))) else None)
    create_args = creation_app_args(app_spec, artifact, ctor_values, evm_abi=evm_abi)
    post_args = postinit_app_args(app_spec, ctor_values)
    lane.create(create_args, ts=ts, deferred_constructor=post_args is not None)
    if post_args is not None:
        ok, resp, _info = lane.call(ids.creator_hex, post_args, ts=ts)
        if not ok:
            raise RuntimeError(
                f"{picked} __postInit failed: {str(resp.get('error'))[:200]}")
    return lane, picked, app_spec


def deploy_dependencies(oracle: Oracle, adapter, h, case_dir: Path, meta: dict,
                        mode_args, xchain_args, ids: "Identities",
                        ts: int) -> list[DepApp]:
    """Every recorded dependency, children-first (the EVM leg's own order).

    avm_leg's LocalNet loop without a chain, including its build-flag rule: a
    dependency that must ROUTE a call is compiled with its CALLER's wire ABI,
    while a TAPE-DRIVEN stand-in keeps the ARC-4 profile — the only one that
    still exposes __load/__seek — and answers from its fallback either way.
    Unlike that loop this one REFUSES a dependency it cannot build: a missing
    stand-in silently turns every call into it against an empty account.
    """
    specs = meta.get("dep_ctors") or []
    if not specs:
        return []
    tape_path = case_dir / "dep_tape.json"
    taped = {a.lower() for a in
             (((load_json(tape_path) or {}).get("tapes") or {})
              if tape_path.exists() else {})}
    for spec in specs:
        ids.dep_apps.setdefault(spec["addr"].lower(),
                                DEP_APP_BASE + len(ids.dep_apps))
    deps: list[DepApp] = []
    seen: set[str] = set()
    for spec in specs:
        addr = spec["addr"].lower()
        if addr in seen:
            continue
        seen.add(addr)
        directory = case_dir / spec["dir"]
        compile_args = (list(mode_args or []) if addr in taped
                        else list(mode_args or []) + ["--contract-abi", "evm"]
                        + list(xchain_args))
        try:
            lane, picked, app_spec = _dep_lane(
                oracle, adapter, h, directory / "prepared.sol", spec.get("name"),
                compile_args, ids, spec.get("args"), ts,
                case=load_json(directory / "case.json"))
        except Exception as exc:
            fallback = directory / "stub_fallback.sol"
            if not fallback.exists():
                raise RuntimeError(
                    f"dependency {spec.get('name')} ({addr}) could not be "
                    f"deployed on the oracle lane: {str(exc)[:300]}") from exc
            lane, picked, app_spec = _dep_lane(
                oracle, adapter, h, fallback, None, compile_args, ids, None, ts)
            print(f"[avm] dep {spec.get('name')}: generic stand-in deployed "
                  f"instead ({str(exc)[:100]})")
        dep = DepApp(addr, picked, ids.dep_apps[addr], lane, app_spec)
        deps.append(dep)
        print(f"[avm] dep {picked} app_id={dep.app_id} @ {addr[:10]}…")
    return deps


def dep_address_map(reg: dict, calls: list, ids: "Identities",
                    deps: list[DepApp]) -> dict[bytes, bytes]:
    """avm_leg's `_m20`: historical 20-byte address → this leg's 32-byte word.

    A recorded answer is ABI-encoded, so an address inside it has to be
    translated the same way a call argument is, or a `msg.sender == owner`
    guard fed from the tape can never pass.
    """
    mapping: dict[bytes, bytes] = {}
    senders = {(c.get("sender") or {}).get("__addr__") for c in calls}
    for addr, i in (reg.get("args") or {}).items():
        acct = ids.accts.get(i) if i in senders else None
        mapping[bytes.fromhex(addr[2:])] = (
            bytes(12) + acct.owner if acct is not None
            else bytes(12) + arg_content20(i))
    for addr, i in (reg.get("senders") or {}).items():
        if i in ids.accts:
            mapping[bytes.fromhex(addr[2:])] = bytes(12) + ids.accts[i].owner
    if reg.get("creator"):
        mapping[bytes.fromhex(reg["creator"][2:])] = ids.creator32
    for dep in deps:
        mapping[bytes.fromhex(dep.addr[2:])] = (
            bytes(24) + dep.app_id.to_bytes(8, "big"))
    return mapping


def tape_skips(calls: list, meta: dict, ext_skips: set) -> set:
    """Transactions whose recorded answers this run will never need.

    A stand-in holds its tape in one puya-sol dynamic array, and an array is
    one box: 32 KiB, about a thousand words. morpho_alloc records 1629 words
    for a single dependency and 197 of its 200 transactions are skipped, so
    loading the whole tape overflowed the box before a single call replayed.
    Answers are addressed per transaction (`__seek(start,end)`), so dropping a
    skipped transaction's answers only renumbers what stays — every replayed
    transaction still reads exactly the entries it recorded. A parameterized
    probe replays a recorded read, so ITS transaction is never dropped even
    when the replay skipped it.
    """
    keep = {int(probe["source_txn"]) for probe in (meta.get("probes") or [])
            if probe.get("source_txn") is not None}
    skipped = {c["i"] for c in calls if c.get("skip")} | {int(i) for i in ext_skips}
    return skipped - keep


def load_dep_tapes(case_dir: Path, reg: dict, calls: list, meta: dict,
                   ext_skips: set, ids: "Identities", deps: list[DepApp],
                   ts: int) -> dict:
    """Load each recorded answer tape into its stand-in; return the seek plan.

    TWO-PHASE, exactly as on LocalNet: every stand-in exists by now, so the
    answers can be translated into this leg's address space before they are
    written.
    """
    plans = build_dep_tape_plans(case_dir, tape_skips(calls, meta, ext_skips),
                                 dep_address_map(reg, calls, ids, deps),
                                 calls=calls)
    seek: dict[str, tuple[DepApp, dict]] = {}
    for addr, plan in plans.items():
        dep = next((d for d in deps if d.addr == addr), None)
        if dep is None or not plan["answers"]:
            continue
        load = dep.method(TAPE_LOAD_SIG, 3)
        if load is None or dep.method(TAPE_SEEK_SIG, 2) is None:
            raise NotImplementedError(
                f"dependency stand-in {dep.name} ({addr}) has no "
                f"{TAPE_LOAD_SIG}/{TAPE_SEEK_SIG} — it predates the "
                f"selector-gated, transaction-bounded answer tape both legs "
                f"now use, and serving its recorded answers ungated would "
                f"replay something the EVM leg never did. Regenerate the "
                f"stand-ins with `python3 fetch.py --refresh-stubs "
                f"{case_dir.name}` (avm_leg.py requires the same tape).")
        for words, lens, selectors in tape_script_chunks(plan["answers"],
                                                         plan["selectors"]):
            ok, resp, _info = dep.lane.call(
                ids.creator_hex, arc4_app_args(load, [words, lens, selectors]),
                ts=ts)
            if not ok:
                error = str(resp.get("error") or "")
                if "box size too large" in error:
                    words_total = sum((len(a) + 31) // 32 for a in plan["answers"])
                    raise NotImplementedError(
                        f"dependency {dep.name} ({addr}) records "
                        f"{len(plan['answers'])} answers / {words_total} words, "
                        f"and a stand-in keeps them in ONE dynamic array — "
                        f"which is one 32 KiB box on the AVM (a ~1000-word "
                        f"ceiling, the same on LocalNet). Replay a shorter "
                        f"window, or split the tape store: {error[:120]}")
                raise RuntimeError(
                    f"dependency {dep.name} tape load failed: {error[:200]}")
        print(f"[avm] dep tape loaded: {len(plan['answers'])} answer(s) "
              f"@ {addr[:10]}…")
        seek[addr] = (dep, plan["bounds"])
    return seek


def dep_seek_txns(seek: dict, active: set, index: int, sender: str) -> tuple[list, set]:
    """The `__seek` siblings for one replayed transaction, and the new active
    set — avm_leg's rule: arm the tapes this transaction bounds, and CLEAR the
    ones the previous transaction armed so a stale range cannot serve."""
    upcoming = {addr for addr, (_dep, bounds) in seek.items() if index in bounds}
    txns = []
    for addr in sorted(active | upcoming):
        dep, bounds = seek[addr]
        start, end = bounds.get(index, (0, 0))
        txns.append(dep.seek_txn(sender, start, end))
    return txns, upcoming


def verify_xchain_template(oracle: Oracle, template: bytes) -> None:
    """The hand assembly must hash to what the canonical assembler produces —
    otherwise A(E) (and the compile-cache key) would silently differ from the
    LocalNet lane's."""
    resp = oracle.run({"source": XCHAIN_TOY_TEAL, "version": 9})
    expect = hashlib.new("sha512_256", b"Program" + template).hexdigest()
    if resp.get("program_hash") != expect:
        raise RuntimeError(
            f"xchain template assembly mismatch: oracle {resp.get('program_hash')} "
            f"vs hand-assembled {expect}")


def refuse_unsupported(meta: dict, opts: dict, case_dir: Path) -> None:
    problems = []
    if os.environ.get("CHD_ORACLE_NO_DEPS") and (
            meta.get("dep_ctors") or (case_dir / "dep_tape.json").exists()):
        # Escape hatch for a sweep that wants the pre-dependency behaviour
        # back (dependency replay costs a compile and a ledger per stand-in).
        problems.append("dependency contracts (CHD_ORACLE_NO_DEPS is set)")
    if opts.get("split_config"):
        problems.append("--split-config code pages")
    if opts.get("force_delegate"):
        problems.append("--force-delegate pages")
    if opts.get("child_box"):
        problems.append("--child-programs-via-box")
    if problems:
        raise NotImplementedError(
            "oracle lane does not replay: " + "; ".join(problems)
            + " — use avm_leg.py (LocalNet) for this case")


def postinit_app_args(app_spec, ctor_values) -> list[str] | None:
    """framework.deploy._call_postinit's argument preparation, ATC-encoded
    offline (selector + ARC4 args, >14-arg tuple packing included)."""
    postinit_spec = next((m for m in app_spec.methods if m.name == "__postInit"), None)
    if postinit_spec is None:
        return None
    abi_method = postinit_spec.to_abi_method()
    args = list(ctor_values or [])
    if len(args) > len(abi_method.args):
        raise RuntimeError(
            f"__postInit argument count mismatch: expected at most "
            f"{len(abi_method.args)}, got {len(args)}")
    while len(args) < len(abi_method.args):
        args.append(_zero_for_type(str(abi_method.args[len(args)].type)))
    for i, spec in enumerate(abi_method.args[: len(args)]):
        t = str(spec.type)
        if not (isinstance(args[i], int) and not isinstance(args[i], bool)):
            continue
        if t.startswith("byte[") and t != "byte[]":
            n = int(t[5:-1])
            args[i] = list((args[i] % (1 << (8 * n))).to_bytes(n, "big"))
        elif t == "address":
            args[i] = (args[i] % (1 << 256)).to_bytes(32, "big")
    sk, addr = account.generate_account()
    sp = SuggestedParams(fee=MIN_FEE, first=1, last=1000,
                         gh=base64.b64encode(bytes(32)).decode(), flat_fee=True)
    atc = AtomicTransactionComposer()
    atc.add_method_call(app_id=ORACLE_APP, method=abi_method, sender=addr, sp=sp,
                        signer=AccountTransactionSigner(sk),
                        method_args=args[: len(abi_method.args)])
    return [a.hex() for a in atc.build_group()[0].txn.app_args]


def creation_app_args(app_spec, artifact, values, *, evm_abi=None) -> list[str]:
    """Immediate EVM constructors take one tuple; deferred ones use ARC-4.

    Use solc's constructor ABI for the tuple and the compiled lifecycle method
    to distinguish immediate execution from __postInit. The ARC-4 framework's
    per-value fallback cannot encode an immediate EVM constructor with two args.
    """
    values = list(values or [])
    deferred = any(method.name == "__postInit" for method in app_spec.methods)
    if evm_abi is not None and not deferred:
        ctor = next((item for item in evm_abi if item.get("type") == "constructor"), {})
        inputs = ctor.get("inputs") or []
        if len(values) != len(inputs):
            raise ValueError(f"constructor argument count mismatch: expected {len(inputs)}, got {len(values)}")
        return ([evm_abi_encode([_ctype(i) for i in inputs],
                 [evm_wire_value(v, i) for v, i in zip(values, inputs)]).hex()]
                if inputs else [])
    return [a.hex() for a in _encode_ctor_args(values, app_spec, artifact)] if values else []


def _opt(argv: list[str], flag: str):
    if flag in argv:
        i = argv.index(flag)
        value = argv[i + 1]
        del argv[i:i + 2]
        return value
    return None


def main(argv=None) -> None:
    argv = list(sys.argv[1:] if argv is None else argv)
    oracle_opt = _opt(argv, "--oracle")
    prover_opt = _opt(argv, "--prover-root")
    evm_layout_flag = "--evm-storage-layout" in argv
    if evm_layout_flag:
        argv.remove("--evm-storage-layout")
    if not argv:
        sys.exit(__doc__)
    case_dir = Path(argv[0]).resolve()
    opts = json.loads(argv[1]) if len(argv) > 1 else {}
    if evm_layout_flag:
        opts["evm_layout"] = True

    prover_root = Path(prover_opt or os.environ.get("AVM_PROVER_ROOT")
                       or DEFAULT_PROVER_ROOT).resolve()
    oracle_bin = Path(oracle_opt or os.environ.get("AVM_ORACLE_BIN")
                      or prover_root / "oracle" / "avmoracle").resolve()
    adapter = load_state_adapter(prover_root)
    oracle = Oracle(oracle_bin)

    case = load_json(case_dir / "case.json")
    reg = load_json(case_dir / "registry.json")
    cj = load_json(case_dir / "calls.json")
    meta, calls = cj["meta"], cj["calls"]
    ext_skips = set(int(k) for k in (opts.get("skips") or []))
    refuse_unsupported(meta, opts, case_dir)

    mut = {}
    for e in case["abi"]:
        if e.get("type") == "function":
            sig = e["name"] + "(" + ",".join(_ctype(i) for i in e["inputs"]) + ")"
            mut[sig] = e.get("stateMutability", "")

    template = xchain_template_bytes()
    verify_xchain_template(oracle, template)
    ids = Identities(reg, calls, template)

    # ── the shared clock: the EVM leg's schedule, else the history's own ──
    evm_path = case_dir / "evm_results.json"
    evm = load_json(evm_path) if evm_path.exists() else {}
    time_base = int(opts.get("time_base") or evm.get("time_base") or replay_epoch(calls))
    creation_ts = int((case.get("creation") or {}).get("ts") or 0)
    deployment_time = int(opts.get("deployment_time") or evm.get("deployment_time")
                          or deployment_clock_target(creation_ts, calls, time_base))
    clock_by_index = replay_clock_targets(calls, time_base)
    print(f"[avm] oracle: clock base={time_base} deploy={deployment_time} "
          f"(historical epoch {replay_epoch(calls)})")

    # ── compile exactly as avm_leg does ───────────────────────────────────
    h = Harness(None, case_dir / "out_avm")
    mode_args = mode_compile_args(opts)
    xchain_args = xchain_compile_args(template)
    main_args = main_compile_args(case_dir, opts, mode_args, xchain_args)

    # ── dependencies FIRST: the contract's own constructor calls them ─────
    deps = deploy_dependencies(oracle, adapter, h, case_dir, meta, mode_args,
                               xchain_args, ids, deployment_time)
    dep_seek = load_dep_tapes(case_dir, reg, calls, meta, ext_skips, ids, deps,
                              deployment_time)

    artifacts = compile_case_contract(h, case_dir, case, main_args)
    if any("__Helper" in n for n in artifacts.by_contract):
        raise NotImplementedError("split helper artifacts are not replayed by the oracle lane")
    delegate_doc = compiled_artifact_root(artifacts) / "delegate_helpers.json"
    if delegate_doc.exists() and (load_json(delegate_doc) or {}).get("delegates"):
        raise NotImplementedError("delegate code pages are not replayed by the oracle lane")
    main_artifact = artifacts.by_contract[case["name"]]
    compiler_events = collect_compiler_events(artifacts, case["name"])
    arc56 = load_json(main_artifact["arc56"])
    app_spec = _load_arc56(main_artifact["arc56"])
    approval_teal = main_artifact["approval_teal"].read_text()
    clear_teal = main_artifact["clear_teal"].read_text()
    name = case["name"]
    approval_bin = (main_artifact["approval_teal"].parent / f"{name}.approval.bin").read_bytes()
    clear_bin = (main_artifact["clear_teal"].parent / f"{name}.clear.bin").read_bytes()

    lane = OracleLane(oracle, adapter, approval_teal, clear_teal, approval_bin,
                      clear_bin, ids.creator_hex,
                      schema=global_schema_for(app_spec, name))
    for dep in deps:
        dep.export(lane.state)
        lane.dep_apps.append(dep.app_id)
        # Boxes a dependency arrives with are attributable outright when the
        # oracle later refuses one by name.
        for key in dep.box_keys():
            lane.box_owner[key] = dep.app_id

    # ── deploy: create txn + __postInit, at the deployment instant ────────
    ctor_values = [ids.resolve(m) for m in meta["ctor_args"]] or None
    create_args = creation_app_args(app_spec, main_artifact, ctor_values, evm_abi=case["abi"])
    # PROXY-RUNTIME replay (framework.deploy's skip_postinit): an implementation
    # behind a proxy never ran its constructor against proxy storage — the
    # replay's own `initialize(...)` call does that work — so the deferred
    # constructor stays unexecuted here. It must BE deferred: a constructor that
    # wrote storage during AppCreate cannot be modelled this way.
    proxy_runtime = bool((case.get("proxy") or {}).get("initializer"))
    deferred_constructor = any(m.name == "__postInit" for m in app_spec.methods)
    if proxy_runtime and not deferred_constructor:
        raise RuntimeError(
            "proxy-runtime replay requires a deferred constructor; this "
            "implementation executed constructor storage during AppCreate and "
            "cannot be modelled safely")
    lane.create(create_args, ts=deployment_time, deferred_constructor=deferred_constructor)
    post_args = None if proxy_runtime else postinit_app_args(app_spec, ctor_values)
    if post_args is not None:
        ok, resp, info = lane.call(ids.creator_hex, post_args, ts=deployment_time)
        if not ok:
            raise RuntimeError(f"__postInit failed: {str(resp.get('error'))[:300]}")
        lane.round += 1
    print(f"[avm] oracle: deployed {name} app_id={ORACLE_APP} "
          f"({len(approval_bin)}B approval, {lane.extra_pages} extra pages"
          + (", deferred ctor left for the proxy initializer)" if proxy_runtime else ")"))

    # ── folding ───────────────────────────────────────────────────────────
    app_addr32 = bytes.fromhex(lane.app_addr)
    inv = ids.inverse_fold_table(app_addr32)

    def fold(v):
        if v is None:
            return None
        if isinstance(v, str) and v.startswith("0x"):
            hx = v[2:].lower()
        elif isinstance(v, (list, tuple, bytes, bytearray)):
            hx = bytes(v).hex()
        elif isinstance(v, str):
            try:
                hx = encoding.decode_address(v).hex()
            except Exception:
                return f"?{v}"
        else:
            return f"?{v}"
        if len(hx) == 40:
            hx = bytes(12).hex() + hx
        return inv.get(hx, f"?0x{hx}")

    ev_types = {e["name"]: e["inputs"] for e in case["abi"] if e.get("type") == "event"}
    abi_events = [e for e in case["abi"] if e.get("type") == "event"]

    def fold_events(raw_logs):
        got = decode_avm_log_bytes(raw_logs, abi_events, compiler_events)
        out = []
        for lg in got:
            ins = lg.get("inputs") or ev_types.get(lg["name"], [])
            args = [canon_value(v, i2.get("type", "uint256"), fold, i2.get("components"))
                    for v, i2 in zip(lg["args"], ins)]
            out.append({"name": lg["name"], "args": args})
        return out

    def evm_app_args(sig: str, args: list, claim: bytes | None) -> list[str]:
        """avm_leg._call_evm's wire form: [selector, calldata body, owner claim?]."""
        fn = meta["fns"].get(sig) or {"inputs": [], "outputs": []}
        inputs = fn.get("inputs") or []
        values = [evm_wire_value(value, spec) for value, spec in zip(args, inputs)]
        body = evm_abi_encode([_ctype(spec) for spec in inputs], values)
        out = [evm_selector(sig).hex(), body.hex()]
        if claim is not None:
            out.append(claim.hex())
        return out

    def decode_return(resp: dict, sig: str):
        """The 151f7c75 payload → Result-like object, or an error string."""
        logs = [bytes.fromhex(l) for l in resp.get("logs") or []]
        payload = next((l[4:] for l in reversed(logs) if l.startswith(RET_MAGIC)), None)
        if payload is None:
            return None, logs, f"EVM entry {sig} returned no structured payload"
        outputs = (meta["fns"].get(sig) or {}).get("outputs") or []
        decoded = (list(evm_abi_decode([_ctype(spec) for spec in outputs], payload))
                   if outputs else [])
        value = decoded[0] if len(decoded) == 1 else tuple(decoded) if decoded else None
        return SimpleNamespace(abi_return=value, logs=logs), logs, None

    def read_values(sig: str, args: list, outputs: list, ts: int):
        """A getter/probe answered like avm_leg's expect_revert simulate path."""
        ok, resp, _info = lane.call(ids.creator_hex, evm_app_args(sig, args, None),
                                    ts=ts, commit=False)
        return finish_read(ok, resp, sig, outputs)

    def finish_read(ok: bool, resp: dict, sig: str, outputs: list):
        if not ok:
            return False, f"REVERT:{str(resp.get('error') or '')[:60]}"
        result, _logs, error = decode_return(resp, sig)
        if error:
            return False, f"ERROR:{error[:60]}"
        vs = result.abi_return
        vs = list(vs) if len(outputs) > 1 else [vs]
        return True, [canon_value(v, o["type"], fold, o.get("components"))
                      for v, o in zip(vs, outputs)]

    # ── replay ────────────────────────────────────────────────────────────
    results, snapshots, platform_limits = {}, {}, {}
    snapshot_at = set(meta["snapshot_at"])
    block_ts, block_no = {}, {}
    active_dep_tapes: set = set()
    current_ts = deployment_time
    for c in calls:
        i = c["i"]
        if not c.get("skip") and i not in ext_skips:
            target = clock_by_index.get(i)
            if target:
                current_ts = max(current_ts, int(target))
            block_ts[str(i)] = current_ts
            # Seal + call, as LocalNet's clock jump and the txn's own round.
            lane.round += 2
            block_no[str(i)] = lane.round
            sig, args = c["sig"], [ids.resolve(a) for a in c["args"]]
            is_view = mut.get(sig, "") in ("view", "pure")
            sender_hex, claim = ids.sender((c.get("sender") or {}).get("__addr__"))
            value = int(c.get("value") or 0)
            # Selector-bounded answer tapes: armed inside this call's OWN group,
            # so a rejected call rolls the cursor back with it instead of
            # leaving the next transaction reading from a consumed range.
            seeks, active_dep_tapes = dep_seek_txns(
                dep_seek, active_dep_tapes, i, ids.creator_hex)
            try:
                ok, resp, info = lane.call(
                    sender_hex, evm_app_args(sig, args, claim), ts=current_ts,
                    value=value, commit=not is_view, extra=seeks)
                if not ok:
                    reason = str(resp.get("error") or "")[:160 if is_view else 200]
                    results[i] = {"ok": False, "revert": reason}
                    if is_platform_limit(reason):
                        platform_limits[i] = reason
                else:
                    result, logs, error = decode_return(resp, sig)
                    if error:
                        raise RuntimeError(error)
                    results[i] = {"ok": True, "ret": _ret(result, meta, sig, fold),
                                  "logs": [] if is_view else (fold_events(logs) or [])}
                results[i]["inner_apps"] = inner_application_calls(
                    resp.get("inners_after") or [])
            except NotImplementedError:
                raise
            except Exception as e:
                reason = str(e)[:200]
                results[i] = {"ok": False, "revert": reason}
                if is_platform_limit(reason):
                    platform_limits[i] = reason
        if i in snapshot_at:
            getters = meta["getters"]
            items = [(ids.creator_hex, evm_app_args(g["sig"], [], None)) for g in getters]
            snap = {}
            for g, resp in zip(getters, lane.read_many(items, ts=current_ts)):
                try:
                    ok, value = finish_read(resp.get("result") == "ACCEPT", resp,
                                            g["sig"], g["outputs"])
                    snap[g["sig"]] = value
                except Exception as e:
                    snap[g["sig"]] = f"ERROR:{str(e)[:60]}"
            snapshots[str(i)] = snap

    # ── post-replay probes at the shared instant ──────────────────────────
    probe_time = probe_clock_target(clock_by_index)
    probe_ts = max(current_ts, probe_time) if probe_time else current_ts
    lane.round += 1
    probes = meta.get("probes") or []
    probe_results = {}
    items = []
    for probe in probes:
        try:
            items.append((ids.creator_hex, evm_app_args(
                probe["sig"], [ids.resolve(v) for v in probe.get("args") or []], None)))
        except Exception as exc:
            items.append(None)
            probe_results[str(len(items) - 1)] = {"ok": False, "revert": str(exc)[:160]}
    live = [(k, item) for k, item in enumerate(items) if item is not None]
    if dep_seek:
        # A parameterized probe replays a recorded read, so its dependencies
        # must answer from the same range that transaction used. One request
        # each (the seek differs per probe), instead of the batched read.
        responses = []
        for k, item in live:
            source = probes[k].get("source_txn")
            if source is None:
                responses.append(lane.read_many([item], ts=probe_ts)[0])
                continue
            seeks, active_dep_tapes = dep_seek_txns(
                dep_seek, active_dep_tapes, int(source), ids.creator_hex)
            _ok, resp, _info = lane.call(item[0], item[1], ts=probe_ts,
                                         commit=False, extra=seeks)
            responses.append(resp)
    else:
        responses = lane.read_many([item for _k, item in live], ts=probe_ts)
    for (k, _item), resp in zip(live, responses):
        probe = probes[k]
        try:
            ok, value = finish_read(resp.get("result") == "ACCEPT", resp,
                                    probe["sig"], probe["outputs"])
            probe_results[str(k)] = ({"ok": True, "ret": value} if ok
                                     else {"ok": False, "revert": value[len("REVERT:"):][:160]})
        except Exception as exc:
            probe_results[str(k)] = {"ok": False, "revert": str(exc)[:160]}

    # ── storage: the carried state through avm_leg's readers ──────────────
    syms = ids.storage_symbols()
    slot_layout = load_json(case_dir / "storage_layout.json")
    box_values = lane.box_source()
    if opts.get("evm_layout"):
        from chd_slot_reader import read_slot_storage
        storage = read_slot_storage(
            slot_map_from_boxes(box_values), slot_layout, syms, fold, calls,
            meta.get("fns") or {}, snapshots, meta.get("getters") or [])
    else:
        storage = decode_global_state(lane.global_entries(), arc56, fold)
        app_id_symbols = {ORACLE_APP: symbol("self")}
        dep_index = {a.lower(): i for a, i in (reg.get("deps") or {}).items()}
        for dep in deps:
            if dep.addr in dep_index:
                app_id_symbols[dep.app_id] = symbol(f"D{dep_index[dep.addr]}")
        maps = read_native_maps(
            box_values, arc56, slot_layout, syms, fold, calls,
            meta.get("fns") or {}, app_id_symbols, snapshots,
            meta.get("getters") or [])
        storage["raw_slots"] = maps.pop("__raw_slots__", {})
        storage["coverage"] = maps.pop("__coverage__", {})
        storage["maps"] = maps

    dump_json(case_dir / "avm_results.json",
              {"results": {str(k): v for k, v in results.items()},
               "snapshots": snapshots,
               "probes": probe_results,
               "storage": storage,
               "platform_limits": {str(k): v for k, v in platform_limits.items()},
               "block_ts": block_ts,
               "block_no": block_no,
               "probe_time": probe_time,
               "app_id": ORACLE_APP,
               "backend": "oracle",
               "oracle": {"binary": str(oracle_bin),
                          "requests": oracle.requests,
                          "processes": oracle.processes,
                          "seconds": round(oracle.seconds, 1),
                          "deps": {dep.addr: dep.app_id for dep in deps},
                          "dep_tapes": sorted(dep_seek),
                          **lane.stats}})
    n = len(results)
    n_ok = sum(1 for r in results.values() if r["ok"])
    print(f"[avm] oracle: replayed {n} txns ({n_ok} ok, {n - n_ok} reverted, "
          f"{len(platform_limits)} platform-limit) — {oracle.requests} oracle "
          f"requests in {oracle.processes} processes, {oracle.seconds:.0f}s; "
          f"{lane.stats['discoveries']} box refs discovered, "
          f"{lane.stats['amplified']} amplified call(s), "
          f"{len(box_values)} boxes at end")


if __name__ == "__main__":
    main()
