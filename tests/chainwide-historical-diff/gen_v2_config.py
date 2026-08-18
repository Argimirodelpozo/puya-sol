#!/usr/bin/env python3
"""Generate joint_config_v2.json for the CCTP v2 joint replay.

  ../WIP/tiny-fuzzing-oracle/.evmvenv/bin/python gen_v2_config.py

Harvests the CONFIG ERA from each contract's creation transaction — the calls
`txlist` can never show, because they are internal calls of the deploy txn
(the morpho lesson: a factory-deployed contract's config era lives in its
creation trace). Two shapes appear:
  * proxy delegatecall initialize(...)  → the implementation's initializer
  * direct calls into the new contract  → transferOwnership / setTokenController

Both decode against the implementation ABI into plain values; addresses become
{"__dep__": "0x..."} markers so each leg resolves them its own way.

App ids are the low 64 bits of the PROXY addresses — v2 messages embed the
proxy addresses in signed bytes, exactly like v1.
"""

from __future__ import annotations

import json
from pathlib import Path

from eth_abi import decode
from eth_utils import keccak

HERE = Path(__file__).parent
CASES = HERE / "cases"

PROXY_ADMIN = {
    "4f1ef286": "upgradeToAndCall(address,bytes)",
    "8f283970": "changeAdmin(address)",
    "3659cfe6": "upgradeTo(address)",
}

V2 = {
    "cctp2_transmitter": {
        "contract": "MessageTransmitterV2",
        "address": "0x81d40f21f12a8f0e3252bccb954d722d4c464b64",
    },
    "cctp2_messenger": {
        "contract": "TokenMessengerV2",
        "address": "0x28b5a0e9c621a5badaa536219b3a228c8168cf5d",
    },
    "cctp2_minter": {
        "contract": "TokenMinterV2",
        "address": "0xfd78ee919681417d192449715b2594ab58f5d002",
    },
}


def mark(abi_type: str, value):
    if abi_type == "address":
        return {"__dep__": value.lower()}
    if abi_type == "address[]":
        return [{"__dep__": v.lower()} for v in value]
    if abi_type.endswith("[]"):
        return [mark(abi_type[:-2], v) for v in value]
    if abi_type.startswith("bytes"):
        return {"__b__": value.hex()}
    return value


def creation_calls(address: str) -> list[bytes]:
    """Calldata of every config call made in the contract's creation txn."""
    import urllib.request

    def get(url):
        req = urllib.request.Request(url, headers={"User-Agent": "chd/1.0"})
        with urllib.request.urlopen(req, timeout=40) as r:
            return json.load(r)

    a = get(f"https://eth.blockscout.com/api/v2/addresses/{address}")
    h = a.get("creation_tx_hash") or a.get("creation_transaction_hash")
    if not h:
        return []
    out = []
    for entry in get(f"https://eth.blockscout.com/api/v2/transactions/{h}/raw-trace"):
        act = entry.get("action", {})
        inp = act.get("input") or ""
        if (act.get("to") or "").lower() != address.lower() or not inp:
            continue
        if (act.get("from") or "").lower() == address.lower():
            continue  # self-call, not config
        out.append(bytes.fromhex(inp.removeprefix("0x")))
    return out


def decode_call(abi: list, calldata: bytes):
    """(sig, decoded values) for a calldata blob, or None if no ABI match."""
    for item in abi:
        if item.get("type") != "function":
            continue
        types = [i["type"] for i in item.get("inputs", [])]
        sig = item["name"] + "(" + ",".join(types) + ")"
        if keccak(text=sig)[:4] == calldata[:4]:
            return sig, decode(types, calldata[4:]), types
    return None


def main() -> int:
    config = {"cases": {}, "init_calls": []}
    for tag, spec in V2.items():
        addr = spec["address"]
        config["cases"][tag] = {
            "contract": spec["contract"],
            "address": addr,
            "app_id": int(addr[-16:], 16),
        }
        abi = json.loads((CASES / tag / "case.json").read_text())["abi"]
        blobs = []
        init_path = CASES / tag / "init_calldata.hex"
        if init_path.exists():
            blobs.append(
                bytes.fromhex(init_path.read_text().strip().removeprefix("0x"))
            )
        blobs.extend(creation_calls(addr))
        seen = set()
        for blob in blobs:
            if blob in seen:
                continue
            seen.add(blob)
            decoded = decode_call(abi, blob)
            if decoded is None:
                # Proxy-ADMIN methods (upgradeToAndCall, changeAdmin) are not
                # implementation methods: the replay deploys implementations
                # directly, so the proxy's own admin surface has no analogue.
                label = PROXY_ADMIN.get(blob[:4].hex(), "unknown")
                print(f"[{tag}] skipped proxy-admin call {blob[:4].hex()} ({label})")
                continue
            sig, values, types = decoded
            config["init_calls"].append(
                {
                    "tag": tag,
                    "sig": sig,
                    "args": [mark(t, v) for t, v in zip(types, values)],
                }
            )
            print(f"[{tag}] {sig}")
            for t, v in zip(types, values):
                print(f"    {t} = {v}")
    config["stub"] = {
        "contract": "StubERC20",
        "app_id": 0x2E9EB0CE3606EB48,
        "address": "0xa0b86991c6218b36c1d19d4a2e9eb0ce3606eb48",
        "arc56": "cctp_minter/out_avm/StubERC20.arc56.json",
        "artifact_tag": "cctp_minter",
        "source": "cctp_minter/deps/argdep_a0b86991/prepared.sol",
    }
    out = HERE / "joint_config_v2.json"
    out.write_text(json.dumps(config, indent=1))
    print("wrote", out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
