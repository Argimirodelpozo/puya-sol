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


def _canon_log_val(v, sol_type):
    """Canonicalise one decoded EVM event arg to the shape the AVM-side decoder
    produces: ints as ints, bool→0/1, bytesN/bytes→list of byte-values, address→
    32-byte content hex (12 zero + 20 addr, matching _addr_content on the AVM side),
    string→str. Non-scalar (tuple/array) values pass through _as_int recursively."""
    if isinstance(v, bool):
        return int(v)
    if sol_type == "address" and isinstance(v, str) and v.startswith(("0x", "0X")):
        return "0x" + v[2:].rjust(64, "0").lower()   # 20-byte → 32-byte content hex
    if isinstance(v, (bytes, bytearray)):
        return list(v)
    if isinstance(v, (list, tuple)):
        return [_as_int(x) for x in v]
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
    ver = req.get("solc_version", "0.8.26")
    evm_version = req.get("evm_version", "paris")

    # Multi-file: the driver passes {"sources": {relpath: content}} so `import`s
    # resolve between them. Single-file: read the one fixture. The entry contract
    # is picked below by "most functions" (or req["contract"]).
    if req.get("sources"):
        std_sources = {p: {"content": c} for p, c in req["sources"].items()}
    else:
        with open(req["fixture"]) as fh:
            std_sources = {"fixture.sol": {"content": fh.read()}}

    import solcx
    try:
        solcx.set_solc_version(ver)
    except Exception:
        solcx.install_solc(ver, show_progress=False)
        solcx.set_solc_version(ver)

    compiled = solcx.compile_standard(
        {
            "language": "Solidity",
            "sources": std_sources,
            "settings": {
                "evmVersion": evm_version,
                "outputSelection": {"*": {"*": ["abi", "evm.bytecode.object"]}},
            },
        }
    )
    # Flatten contracts across ALL source files (multi-file: the entry contract can
    # live in any of them). Later files win a name clash — irrelevant, names are unique.
    contracts = {}
    src_of = {}
    for src_path, by_name in compiled["contracts"].items():
        for cname, cdata in by_name.items():
            contracts[cname] = cdata
            src_of[cname] = src_path
    want = req.get("contract")
    if want and want in contracts:
        name = want
    else:  # pick the DEPLOYABLE contract with the most functions (skip abstract/
           # interface/library — they have no bytecode)
        deployable = {n: c for n, c in contracts.items()
                      if c.get("evm", {}).get("bytecode", {}).get("object")}
        pool = deployable or contracts
        name = max(pool, key=lambda n: sum(
            1 for e in pool[n]["abi"] if e.get("type") == "function"))
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
        from eth_utils import event_abi_to_log_topic as _evt_topic
        def _topic0(e):
            try:
                return "0x" + _evt_topic(e).hex()      # keccak256(canonical event sig)
            except Exception:
                return None
        evs = [{"name": e["name"], "inputs": e["inputs"],
                "anonymous": e.get("anonymous", False),
                "topic0": _topic0(e)}                   # for AVM raw-log3 (asm log) matching
               for e in abi if e.get("type") == "event"]
        errs = [{"name": e["name"], "inputs": e["inputs"]}
                for e in abi if e.get("type") == "error"]
        json.dump({"contract": name, "functions": fns, "events": evs, "errors": errs},
                  sys.stdout)
        return

    w3 = _make_w3()
    acct = w3.eth.accounts[0]
    C = w3.eth.contract(abi=abi, bytecode=bytecode)
    txh = C.constructor(*req.get("ctor_args", [])).transact({"from": acct})
    addr = w3.eth.get_transaction_receipt(txh)["contractAddress"]
    inst = w3.eth.contract(address=addr, abi=abi)

    from web3.exceptions import ContractLogicError
    import ast
    from eth_abi import encode as _abi_encode

    def _revert_payload(e):
        """Normalise a SOLIDITY revert → raw payload hex ('0x...'), or None if the
        exception is NOT a revert. A revert carries either structured .data (web3
        ContractLogicError — Panic/custom) or an 'execution reverted' message
        (eth_tester TransactionFailed, whose args[0] holds an Error(string) already
        decoded to its text, a bytes-repr for custom errors, or empty). VM HALTS
        (out-of-gas, invalid jump, net-metered SSTORE, stack errors) are NOT
        Solidity reverts and are gas-model divergences we can't compare — return
        None so the differ skips them. Error(string) messages are re-encoded to the
        canonical 0x08c379a0 payload so the driver decodes all kinds uniformly."""
        d = getattr(e, "data", None)
        if isinstance(d, (bytes, bytearray)):
            return "0x" + bytes(d).hex()
        if isinstance(d, str) and d.startswith("0x") and d != "0x":
            return d
        txt = str(e.args[0]) if getattr(e, "args", None) else str(e)
        if "execution reverted" not in txt:
            return None                                  # non-revert error — skip
        rest = txt.split("execution reverted", 1)[1].lstrip(": ").strip()
        if not rest:
            return "0x"
        if rest[:2] in ("b'", 'b"'):                     # custom error → raw bytes-repr
            try:
                return "0x" + bytes(ast.literal_eval(rest)).hex()
            except Exception:
                return None
        # eth-tester prefixes VM HALTS with "execution reverted" too, and their
        # diagnostic text is format-identical to a Solidity Error(string) message.
        # A halt is a gas/VM-model divergence (AVM has an opcode budget, not gas),
        # not a comparable revert — deny the finite set of py-evm halt reasons.
        _VM_HALT = ("out of gas", "invalid jump", "net-metered", "invalid opcode",
                    "invalid instruction", "stack underflow", "stack overflow",
                    "insufficient stack", "max code size", "write protection",
                    "insufficient gas", "gas uint64 overflow")
        low = rest.lower()
        if any(h in low for h in _VM_HALT):
            return None
        try:                                             # eth-tester-decoded Error(string) message
            return "0x08c379a0" + _abi_encode(["string"], [rest]).hex()
        except Exception:
            return None

    # Stateful mode: persist storage across the call sequence. View/pure calls just `.call()`
    # (read); state-changing calls `.call()` (captures the return against the current state) THEN
    # `.transact()` (commits the same computation) so the next call sees the update.
    stateful = bool(req.get("stateful", False))
    want_logs = bool(req.get("logs", False))
    view_sigs = set()
    for e in abi:
        if e.get("type") == "function" and e.get("stateMutability") in ("view", "pure"):
            view_sigs.add(_fn_sig(e))

    # Build topic[0] → (event_name, [(name, type, indexed)]) for log decoding.
    from eth_utils import event_abi_to_log_topic
    from web3._utils.events import get_event_data
    topic_map = {}
    if want_logs:
        for e in abi:
            if e.get("type") != "event" or e.get("anonymous"):
                continue
            try:
                topic_map["0x" + event_abi_to_log_topic(e).hex()] = e
            except Exception:
                pass

    def _decode_logs(receipt):
        """Receipt logs → [{name, args:[values in ABI definition order]}], canonicalised."""
        out = []
        for lg in receipt.get("logs", []):
            topics = lg.get("topics", [])
            if not topics:
                continue
            t0 = topics[0]
            t0 = "0x" + (t0.hex() if hasattr(t0, "hex") else str(t0).removeprefix("0x"))
            ev = topic_map.get(t0.lower()) or topic_map.get(t0)
            if not ev:
                continue
            try:
                data = get_event_data(w3.codec, ev, lg)["args"]
            except Exception:
                continue
            vals = []
            for inp in ev["inputs"]:
                v = data.get(inp["name"])
                vals.append(_canon_log_val(v, inp["type"]))
            out.append({"name": ev["name"], "args": vals})
        return out

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
            logs = None
            if stateful and sig not in view_sigs:
                rcpt = w3.eth.wait_for_transaction_receipt(fnf(*args).transact({"from": acct}))
                if want_logs:
                    logs = _decode_logs(rcpt)
            res = {"ok": True, "value": _as_int(val)}
            if logs is not None:
                res["logs"] = logs
            results.append(res)
        except Exception as e:
            # Both ContractLogicError (Panic) AND eth_tester TransactionFailed
            # (require-string / custom errors / plain revert) are reverts — the old
            # code only caught the former, silently SKIPPING every other revert kind.
            rd = _revert_payload(e)
            if rd is not None:
                results.append({"ok": False, "revert": True, "revert_data": rd})
            else:
                results.append({"ok": False, "revert": False,
                                "err": type(e).__name__ + ": " + str(e)[:100]})

    # The tx sender (msg.sender / deployer). Contracts that store or return it
    # (owner = msg.sender) would otherwise "diverge" — the EVM and AVM callers are
    # different accounts by construction. The driver maps both to a sentinel.
    caller = "0x" + acct[2:].rjust(64, "0").lower()
    self_addr = "0x" + addr[2:].rjust(64, "0").lower()   # address(this) — differs per chain
    json.dump({"contract": name, "results": results,
               "caller": caller, "self": self_addr}, sys.stdout)


if __name__ == "__main__":
    main()
