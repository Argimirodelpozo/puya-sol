#!/usr/bin/env python3
"""LIVE EVM oracle — runs in .evmvenv (solcx + py-evm/eth-tester + web3).

Replaces the hand-written oracle.py with a REAL solc+EVM so arbitrary fixtures can be
fuzzed without hand-modeling each function. Reads a JSON batch from stdin, compiles the
fixture with solc, deploys it on an in-process py-evm chain, executes every call, and
writes the results to stdout as JSON.

  stdin : {"fixture": "<abs.sol>", "solc_version": "0.8.26", "evm_version": "paris",
           "contract": "Probe"?, "ctor_args": []?, "calls": [{"sig":"f(uint256)","args":[1]}]}
  stdout: {"contract": "<name>", "results": [{"ok":true,"value":<int|list>} |
           {"ok":false,"revert":true} | {"ok":false,"revert":false,"err":"..."}]}

A clean Solidity revert/require/assert/Panic surfaces as ContractLogicError → revert:true.
Any other exception (e.g. arg-encoding) → revert:false (the diff driver skips those: not a
semantic result). Return values are coerced to ints (big-endian for bytesN) so the driver
can canonicalize to the 256-bit pattern.
"""
import json
import sys


def _as_int(v):
    if isinstance(v, (list, tuple)):
        return [_as_int(x) for x in v]
    if isinstance(v, bool):
        return int(v)
    if isinstance(v, (bytes, bytearray)):
        return list(v)   # list of int byte-values — matches how algosdk decodes byte[]/bytesN
    return v


def _canon_type(e):
    """Canonical ABI type string for an input/output entry: expands tuples to
    (t1,t2,...) (incl. tuple[] / tuple[N]) so signatures match the real selector."""
    t = e["type"]
    if t.startswith("tuple"):
        inner = "(" + ",".join(_canon_type(c) for c in e.get("components", [])) + ")"
        return inner + t[len("tuple"):]      # carry any [] / [N] suffix
    return t


def _fn_sig(e):
    return e["name"] + "(" + ",".join(_canon_type(i) for i in e["inputs"]) + ")"


def _make_w3():
    # EthereumTesterProvider location moved across web3 majors.
    from web3 import Web3
    try:
        from web3 import EthereumTesterProvider  # web3 v6
    except ImportError:
        from web3.providers.eth_tester import EthereumTesterProvider  # web3 v7
    return Web3(EthereumTesterProvider())


def main():
    req = json.load(sys.stdin)
    with open(req["fixture"]) as fh:
        src = fh.read()
    ver = req.get("solc_version", "0.8.26")
    evm_version = req.get("evm_version", "paris")

    import solcx
    try:
        solcx.set_solc_version(ver)
    except Exception:
        solcx.install_solc(ver, show_progress=False)
        solcx.set_solc_version(ver)

    compiled = solcx.compile_standard(
        {
            "language": "Solidity",
            "sources": {"fixture.sol": {"content": src}},
            "settings": {
                "evmVersion": evm_version,
                "outputSelection": {"*": {"*": ["abi", "evm.bytecode.object"]}},
            },
        }
    )
    contracts = compiled["contracts"]["fixture.sol"]
    want = req.get("contract")
    if want and want in contracts:
        name = want
    else:  # pick the contract with the most functions (the main one)
        name = max(contracts, key=lambda n: sum(
            1 for e in contracts[n]["abi"] if e.get("type") == "function"))
    abi = contracts[name]["abi"]
    bytecode = contracts[name]["evm"]["bytecode"]["object"]

    # Introspect mode: return the function table so the driver can auto-generate
    # boundary inputs per param type (no per-function hand-modeling, any fixture).
    if req.get("introspect"):
        fns = []
        for e in abi:
            if e.get("type") != "function":
                continue
            fns.append({
                "sig": _fn_sig(e),
                "inputs": e["inputs"],                  # full entries (type + components)
                "outputs": [o["type"] for o in e["outputs"]],
                "mut": e.get("stateMutability", ""),
            })
        json.dump({"contract": name, "functions": fns}, sys.stdout)
        return

    w3 = _make_w3()
    acct = w3.eth.accounts[0]
    C = w3.eth.contract(abi=abi, bytecode=bytecode)
    txh = C.constructor(*req.get("ctor_args", [])).transact({"from": acct})
    addr = w3.eth.get_transaction_receipt(txh)["contractAddress"]
    inst = w3.eth.contract(address=addr, abi=abi)

    from web3.exceptions import ContractLogicError

    # Stateful mode: persist storage across the call sequence. View/pure calls just `.call()`
    # (read); state-changing calls `.call()` (captures the return against the current state) THEN
    # `.transact()` (commits the same computation) so the next call sees the update.
    stateful = bool(req.get("stateful", False))
    view_sigs = set()
    for e in abi:
        if e.get("type") == "function" and e.get("stateMutability") in ("view", "pure"):
            view_sigs.add(_fn_sig(e))

    def _rebytes(o):  # rebuild driver-tagged args: {"__b__":hex}→bytes, {"__addr__":i}→EVM addr
        if isinstance(o, dict):
            if set(o) == {"__b__"}:
                return bytes.fromhex(o["__b__"])
            if set(o) == {"__addr__"}:
                return "0x" + int(o["__addr__"]).to_bytes(20, "big").hex()   # slot i → 20-byte addr
            return {k: _rebytes(v) for k, v in o.items()}
        if isinstance(o, list):
            return [_rebytes(x) for x in o]
        return o

    results = []
    for call in req["calls"]:
        sig, args = call["sig"], _rebytes(call["args"])
        try:
            try:
                fnf = inst.get_function_by_signature(sig)
            except Exception:
                fnf = inst.functions[sig.split("(", 1)[0]]
            val = fnf(*args).call({"from": acct})
            if stateful and sig not in view_sigs:
                w3.eth.wait_for_transaction_receipt(fnf(*args).transact({"from": acct}))
            results.append({"ok": True, "value": _as_int(val)})
        except ContractLogicError:
            results.append({"ok": False, "revert": True})
        except Exception as e:
            results.append({"ok": False, "revert": False,
                            "err": type(e).__name__ + ": " + str(e)[:100]})

    json.dump({"contract": name, "results": results}, sys.stdout)


if __name__ == "__main__":
    main()
