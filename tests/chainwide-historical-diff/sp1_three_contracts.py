#!/usr/bin/env python3
"""Two real external hops: synthetic application -> SP1 gateway -> SP1 verifier.

Run `evm <historical_case> <new_output_dir>` with the EVM venv, then
`avm <output_dir>` with the oracle's Python environment, then `diff <output_dir>`.
Only local ledgers are used. Verified sources are copied without body rewrites;
there is no dependency fallback or answer tape. All application calls are
synthetic, even where their proof bytes came from a historical transaction.
"""
from __future__ import annotations

import argparse
import hashlib
import shutil
from pathlib import Path

from eth_abi import decode, encode
from eth_utils import keccak

from chd_common import dump_json, is_platform_limit, load_json

HERE = Path(__file__).resolve().parent
SIGNATURE = "verifyProof(bytes32,bytes,bytes)"
SUBMIT = "submitProof(bytes,bytes)"
GATEWAY_ID, VERIFIER_ID = 9003, 9002
EPOCH = 1_780_000_000


def calldata(signature: str, types: list[str], values=()) -> bytes:
    return keccak(text=signature)[:4] + encode(types, values)


def proof_rows(case: dict) -> tuple[bytes, list[dict]]:
    rows, keys = [], set()
    selector = "0x" + keccak(text=SIGNATURE)[:4].hex()
    for txn in case["txns"]:
        if not txn["input"].startswith(selector):
            continue
        key, values, proof = decode(["bytes32", "bytes", "bytes"],
                                    bytes.fromhex(txn["input"][10:]))
        if proof[:4].hex() != "4388a21c" or not txn["hist_ok"]:
            raise ValueError("this fixture requires successful SP1 v6.1.0 proofs")
        keys.add(key)
        rows.append({"kind": "historical-proof-in-synthetic-application",
                     "source_transaction": txn["hash"], "public_values": values.hex(),
                     "proof": proof.hex(), "expected_ok": True})
    if len(keys) != 1 or not rows:
        raise ValueError("expected a nonempty window for exactly one program key")
    last = rows[-1]
    values, proof = bytes.fromhex(last["public_values"]), bytes.fromhex(last["proof"])
    controls = [
        ("changed-public-values", bytes([values[0] ^ 1]) + values[1:], proof, False),
        ("unknown-verifier-selector", values, bytes.fromhex("ffffffff") + proof[4:], False),
        ("truncated-proof", values, proof[:-1], False),
        ("valid-proof-after-rejections", values, proof, True),
    ]
    for kind, val, seal, ok in controls:
        rows.append({**last, "kind": kind, "public_values": val.hex(),
                     "proof": seal.hex(), "expected_ok": ok})
    return next(iter(keys)), rows


def compile_evm(directory: Path, case: dict) -> dict:
    import solcx

    solcx.set_solc_version("0.8.20")
    manifest = case.get("multifile")
    sources = ({rel: {"content": (directory / "src" / rel).read_text()}
                for rel in manifest["files"]} if manifest else
               {"Main.sol": {"content": (directory / "prepared.sol").read_text()}})
    settings = {"outputSelection": {"*": {"*": ["abi", "evm.bytecode", "storageLayout"]}}}
    if manifest:
        settings["remappings"] = manifest["remappings"]
    compiled = solcx.compile_standard({"language": "Solidity", "sources": sources,
                                       "settings": settings})
    matches = [contracts[case["name"]] for contracts in compiled["contracts"].values()
               if case["name"] in contracts]
    if len(matches) != 1:
        raise ValueError(f"ambiguous or missing solc contract: {case['name']}")
    artifact = matches[0]
    # Persist solc facts; AVM compilation and constructor encoding use this ABI.
    dump_json(directory / "case.json", {**case, "abi": artifact["abi"]})
    dump_json(directory / "storage_layout.json", artifact["storageLayout"])
    return artifact


def evm_tree(computation, roles: dict[bytes, str]) -> list[dict]:
    out = []
    for child in computation.children:
        children = evm_tree(child, roles)
        role = roles.get(bytes(child.msg.storage_address))
        if role:
            out.append({"contract": role, "children": children})
        else:
            out.extend(children)
    return out


def avm_tree(txns: list[dict]) -> list[dict]:
    roles = {GATEWAY_ID: "gateway", VERIFIER_ID: "verifier"}
    out = []
    for txn in txns:
        children = avm_tree(txn.get("inner_txns") or [])
        role = roles.get((txn.get("u64") or {}).get("ApplicationID"))
        if role:
            out.append({"contract": role, "children": children})
        else:
            out.extend(children)
    return out


def has_two_hops(tree: list[dict]) -> bool:
    return any(node["contract"] == "gateway" and
               any(child["contract"] == "verifier" for child in node["children"])
               for node in tree)


def run_evm(source: Path, output: Path) -> None:
    from eth_keys import keys
    from eth_tester import PyEVMBackend

    case = load_json(source / "case.json")
    key, rows = proof_rows(case)
    verifier_addr = "0xb69f2584cbcff99a58c4e7002e8b89af54a6f4e2"
    verifier_dir = next(source / item["dir"] for item in case["ctor_deps"]
                        if item["addr"] == verifier_addr)
    output.mkdir(parents=True, exist_ok=False)
    (output / "gateway").mkdir()
    shutil.copytree(source / "src", output / "gateway/src")
    shutil.copy2(source / "prepared.sol", output / "gateway/prepared.sol")
    shutil.copytree(verifier_dir, output / "verifier",
                    ignore=shutil.ignore_patterns("stub_fallback.sol"))
    (output / "application").mkdir()
    shutil.copy2(HERE / "fixtures/ProofApplication.sol", output / "application/prepared.sol")
    units = {
        "verifier": compile_evm(output / "verifier", load_json(verifier_dir / "case.json")),
        "gateway": compile_evm(output / "gateway", case),
        "application": compile_evm(output / "application", {"name": "ProofApplication"}),
    }
    private_key = keys.PrivateKey(bytes.fromhex("11" * 32))  # local fixture key only
    creator = private_key.public_key.to_canonical_address()
    backend = PyEVMBackend(
        genesis_parameters={"timestamp": EPOCH, "gas_limit": 60_000_000},
        genesis_state={creator: {"balance": 10**24, "nonce": 0, "code": b"", "storage": {}}})
    chain = backend.chain

    def execute(target: bytes, data: bytes):
        vm = chain.get_vm()
        txn = vm.create_unsigned_transaction(
            nonce=vm.state.get_nonce(creator), gas_price=10**10, gas=15_000_000,
            to=target, value=0, data=data).as_signed_transaction(private_key)
        _, _, computation = chain.apply_transaction(txn)
        chain.mine_block()
        return computation

    def deploy(role: str, args: list):
        artifact = units[role]
        ctor = next((e for e in artifact["abi"] if e["type"] == "constructor"), {})
        data = bytes.fromhex(artifact["evm"]["bytecode"]["object"])
        data += encode([i["type"] for i in ctor.get("inputs", [])], args)
        computation = execute(b"", data)
        if computation.is_error:
            raise RuntimeError(f"{role} constructor failed: {computation.error}")
        return bytes(computation.msg.storage_address)

    verifier = deploy("verifier", [])
    gateway = deploy("gateway", [creator])
    setup = execute(gateway, calldata("addRoute(address)", ["address"], [verifier]))
    if setup.is_error:
        raise RuntimeError(f"gateway route setup failed: {setup.error}")
    application = deploy("application", [gateway, key])
    roles = {verifier: "verifier", gateway: "gateway", application: "application"}
    dump_json(output / "campaign.json", {
        "scope": "synthetic application with historical proof inputs; three real contracts",
        "source_case": str(source), "creator": "0x" + creator.hex(),
        "program_key": key.hex(), "rows": rows, "solc": "0.8.20",
        "addresses": {role: "0x" + address.hex() for address, role in roles.items()},
    })
    results = []
    for row in rows:
        computation = execute(application, calldata(SUBMIT, ["bytes", "bytes"],
            [bytes.fromhex(row["public_values"]), bytes.fromhex(row["proof"])]))
        state = execute(application, calldata("state()", []))
        route = execute(application, calldata("routeStatus(bytes4)", ["bytes4"],
                                               [bytes.fromhex("4388a21c")]))
        if state.is_error or route.is_error:
            raise RuntimeError("state/route inspection failed on EVM")
        count, digest = decode(["uint256", "bytes32"], state.output)
        route_addr, frozen = decode(["address", "bool"], route.output)
        results.append({"ok": not computation.is_error, "return": computation.output.hex(),
                        "error": str(computation.error) if computation.is_error else None,
                        "platform_limit": computation.is_error and type(computation.error).__name__ == "OutOfGas",
                        "state": [count, digest.hex()], "route": [roles[bytes.fromhex(route_addr[2:])], frozen],
                        "inner_tree": evm_tree(computation, roles)})
    dump_json(output / "evm.json", {"results": results})
    print(f"[evm] deployed all three contracts; {len(results)} application calls; "
          f"{sum(r['ok'] for r in results)} successful", flush=True)


def run_avm(output: Path, prover_root: Path | None, oracle_path: Path | None) -> None:
    from oracle_case import (DEFAULT_PROVER_ROOT, DepApp, Harness, Identities, Oracle,
        RET_MAGIC, _dep_lane, load_state_adapter, mode_compile_args, xchain_compile_args,
        xchain_template_bytes, verify_xchain_template)

    campaign = load_json(output / "campaign.json")
    prover_root = prover_root or DEFAULT_PROVER_ROOT
    oracle = Oracle(oracle_path or prover_root / "oracle/avmoracle")
    adapter = load_state_adapter(prover_root)
    template = xchain_template_bytes()
    verify_xchain_template(oracle, template)
    ids = Identities({"creator": campaign["creator"], "senders": {}, "args": {}}, [], template)
    ids.dep_apps = {"gateway": GATEWAY_ID, "verifier": VERIFIER_ID}
    harness = Harness(None, output / "out_avm")
    flags = list(mode_compile_args({}) or []) + ["--contract-abi", "evm"] + xchain_compile_args(template)

    def deploy(role: str, args: list):
        directory = output / role
        case = load_json(directory / "case.json")
        lane, name, spec = _dep_lane(oracle, adapter, harness, directory / "prepared.sol",
                                     case["name"], flags, ids, args, EPOCH, case=case)
        print(f"[avm] compiled and created {role}: {name} ({len(lane.approval_bin)}B)", flush=True)
        return lane, name, spec

    def attach(lane, dependency):
        dependency.export(lane.state)
        lane.dep_apps.append(dependency.app_id)
        for name in dependency.box_keys():
            lane.box_owner[name] = dependency.app_id

    verifier_lane, name, spec = deploy("verifier", [])
    verifier = DepApp("verifier", name, VERIFIER_ID, verifier_lane, spec)
    gateway_lane, name, spec = deploy("gateway", [{"__addr__": "C"}])
    attach(gateway_lane, verifier)

    def call(lane, data: bytes, *, commit=True):
        lane.round += 1
        ok, response, info = lane.call(ids.creator_hex, [data[:4].hex(), data[4:].hex()],
                                       ts=EPOCH, commit=commit)
        return ok, response, info

    verifier_value = bytes(12) + VERIFIER_ID.to_bytes(8, "big")
    ok, response, _ = call(gateway_lane, calldata("addRoute(address)", ["address"], [verifier_value]))
    if not ok:
        raise RuntimeError(f"gateway route setup failed: {response.get('error')}")
    gateway = DepApp("gateway", name, GATEWAY_ID, gateway_lane, spec)
    lane, _, _ = deploy("application", [{"__dep__": "gateway"}, {"__b__": campaign["program_key"]}])
    attach(lane, verifier)
    attach(lane, gateway)

    def payload(response):
        logs = [bytes.fromhex(value) for value in response.get("logs", [])]
        return next(value[4:] for value in reversed(logs) if value.startswith(RET_MAGIC))

    results = []
    for row in campaign["rows"]:
        ok, response, info = call(lane, calldata(SUBMIT, ["bytes", "bytes"],
            [bytes.fromhex(row["public_values"]), bytes.fromhex(row["proof"])]))
        read_ok, state, _ = call(lane, calldata("state()", []), commit=False)
        route_ok, route, _ = call(lane, calldata("routeStatus(bytes4)", ["bytes4"],
                                               [bytes.fromhex("4388a21c")]), commit=False)
        if not read_ok or not route_ok:
            raise RuntimeError(f"AVM state/route inspection failed: {state.get('error')} {route.get('error')}")
        count, digest = decode(["uint256", "bytes32"], payload(state))
        route_addr, frozen = decode(["address", "bool"], payload(route))
        results.append({"ok": ok, "return": payload(response).hex() if ok else "",
                        "error": response.get("error"), "state": [count, digest.hex()],
                        "platform_limit": is_platform_limit(response.get("error") or ""),
                        "route": ["verifier" if int(route_addr, 16) == VERIFIER_ID else route_addr, frozen],
                        "inner_tree": avm_tree(response.get("inners_after") or []),
                        "amplified": info["amplified"]})
    dump_json(output / "avm.json", {"results": results, "oracle": str(oracle.binary),
        "apps": {"application": 9001, "gateway": GATEWAY_ID, "verifier": VERIFIER_ID},
        "oracle_stats": lane.stats})
    print(f"[avm] {len(results)} application calls; {sum(r['ok'] for r in results)} successful", flush=True)


def compare(output: Path) -> dict:
    campaign = load_json(output / "campaign.json")
    evm, avm = (load_json(output / f"{leg}.json")["results"] for leg in ("evm", "avm"))
    findings = []
    if len(evm) != len(campaign["rows"]) or len(avm) != len(evm):
        raise ValueError("incomplete three-contract replay")
    count, digest = 0, bytes(32).hex()
    for index, (row, e, a) in enumerate(zip(campaign["rows"], evm, avm)):
        if row["expected_ok"]:
            count += 1
            digest = hashlib.sha256(bytes.fromhex(row["public_values"])).hexdigest()
        for leg, result in (("evm", e), ("avm", a)):
            expected = {"ok": row["expected_ok"], "state": [count, digest],
                        "route": ["verifier", False], "platform_limit": False}
            if row["expected_ok"]:
                expected["return"] = digest
                expected["two_hops"] = True
            observed = {**result, "two_hops": has_two_hops(result["inner_tree"])}
            for field, value in expected.items():
                if observed[field] != value:
                    findings.append({"index": index, "kind": row["kind"], "leg": leg,
                                     "field": field, "expected": value, "actual": observed[field]})
    report = {"scope": campaign["scope"], "calls": len(evm), "expected_successes": count,
              "expected_rejections": len(evm) - count,
              "evm_successes": sum(r["ok"] for r in evm),
              "avm_successes": sum(r["ok"] for r in avm),
              "evm_two_hop_successes": sum(r["ok"] and has_two_hops(r["inner_tree"]) for r in evm),
              "avm_two_hop_successes": sum(r["ok"] and has_two_hops(r["inner_tree"]) for r in avm),
              "findings": findings}
    dump_json(output / "report.json", report)
    print(f"[diff] {len(evm)} calls, {count} expected successes; {len(findings)} findings", flush=True)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    evm = commands.add_parser("evm")
    evm.add_argument("source", type=Path)
    evm.add_argument("output", type=Path)
    avm = commands.add_parser("avm")
    avm.add_argument("output", type=Path)
    avm.add_argument("--prover-root", type=Path)
    avm.add_argument("--oracle", type=Path)
    diff = commands.add_parser("diff")
    diff.add_argument("output", type=Path)
    args = parser.parse_args()
    if args.command == "evm":
        run_evm(args.source.resolve(), args.output.resolve())
    elif args.command == "avm":
        run_avm(args.output.resolve(), args.prover_root, args.oracle)
    elif compare(args.output)["findings"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
