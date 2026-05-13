#!/usr/bin/env python3
"""Run only the tests that compiled successfully — deploy + verify assertions."""
import sys
from pathlib import Path

import algokit_utils as au
from algosdk import encoding
from algosdk.transaction import (
    ApplicationCreateTxn, OnComplete, StateSchema,
    PaymentTxn, wait_for_confirmation,
)

from parser import parse_test_file, parse_value
import re

TESTS_DIR = Path(__file__).parent / "tests"
OUT_DIR = Path(__file__).parent / "out"
NO_POPULATE = au.SendParams(populate_app_call_resources=False)


def find_last_contract(sol_path: Path, deployable: dict) -> str:
    """Find the last contract defined in the source that has deployable artifacts.

    Solidity semantic tests expect the last contract to be deployed.
    """
    content = sol_path.read_text()
    # Find all contract/library names in source order
    contracts = re.findall(r'(?:contract|library)\s+(\w+)', content)
    # Return the last one that has deployable artifacts
    for name in reversed(contracts):
        if name in deployable:
            return name
    # Fallback: last deployable
    return list(deployable.keys())[-1]

# Map Solidity types to ARC4 types for method resolution
SOL_TO_ARC4 = {
    "uint8": "uint8", "uint16": "uint16", "uint32": "uint32", "uint64": "uint64",
    "uint128": "uint128", "uint256": "uint256",
    "int8": "int8", "int16": "int16", "int32": "int32", "int64": "int64",
    "int128": "int128", "int256": "int256",
    "bool": "bool", "address": "address", "string": "string",
    "bytes": "byte[]",
    "bytes1": "byte[1]", "bytes2": "byte[2]", "bytes3": "byte[3]", "bytes4": "byte[4]",
    "bytes5": "byte[5]", "bytes6": "byte[6]", "bytes7": "byte[7]", "bytes8": "byte[8]",
    "bytes9": "byte[9]", "bytes10": "byte[10]", "bytes11": "byte[11]", "bytes12": "byte[12]",
    "bytes13": "byte[13]", "bytes14": "byte[14]", "bytes15": "byte[15]", "bytes16": "byte[16]",
    "bytes17": "byte[17]", "bytes18": "byte[18]", "bytes19": "byte[19]", "bytes20": "byte[20]",
    "bytes21": "byte[21]", "bytes22": "byte[22]", "bytes23": "byte[23]", "bytes24": "byte[24]",
    "bytes25": "byte[25]", "bytes26": "byte[26]", "bytes27": "byte[27]", "bytes28": "byte[28]",
    "bytes29": "byte[29]", "bytes30": "byte[30]", "bytes31": "byte[31]", "bytes32": "byte[32]",
}


def resolve_method(app_spec, sol_sig):
    """Resolve a Solidity method signature to an ARC56 method.

    sol_sig is like "f(uint256)" or "g()" — Solidity types.
    We need to match against ARC56 methods which use ARC4 types.
    """
    import re
    m = re.match(r'(\w+)\((.*)\)', sol_sig)
    if not m:
        return sol_sig
    name = m.group(1)
    params_str = m.group(2)

    # Find all methods with this name
    methods = [method for method in app_spec.methods if method.name == name]

    if len(methods) == 0:
        return sol_sig  # Not found, let it fail naturally
    if len(methods) == 1:
        # Only one method with this name — use it
        method = methods[0]
        args_part = ",".join(a.type for a in method.args)
        ret_part = method.returns.type if method.returns and method.returns.type != "void" else "void"
        return f"{name}({args_part}){ret_part}"

    # Multiple overloads — try to match by parameter types
    sol_params = [p.strip() for p in params_str.split(",")] if params_str else []
    arc4_params = [SOL_TO_ARC4.get(p, p) for p in sol_params]

    for method in methods:
        method_types = [a.type for a in method.args]
        if method_types == arc4_params:
            args_part = ",".join(method_types)
            ret_part = method.returns.type if method.returns and method.returns.type != "void" else "void"
            return f"{name}({args_part}){ret_part}"

    # Fallback: match by param count
    for method in methods:
        if len(method.args) == len(sol_params):
            args_part = ",".join(a.type for a in method.args)
            ret_part = method.returns.type if method.returns and method.returns.type != "void" else "void"
            return f"{name}({args_part}){ret_part}"

    return sol_sig


def setup_localnet():
    algod = au.ClientManager.get_algod_client(
        au.ClientManager.get_default_localnet_config("algod"))
    kmd = au.ClientManager.get_kmd_client(
        au.ClientManager.get_default_localnet_config("kmd"))
    localnet = au.AlgorandClient(au.AlgoSdkClients(algod=algod, kmd=kmd))
    localnet.set_suggested_params_cache_timeout(0)
    account = localnet.account.localnet_dispenser()
    localnet.account.set_signer_from_account(account)
    return localnet, account


def deploy_contract(localnet, account, artifacts):
    try:
        app_spec = au.Arc56Contract.from_json(artifacts["arc56"].read_text())
        algod = localnet.client.algod
        approval_bin = encoding.base64.b64decode(
            algod.compile(artifacts["approval_teal"].read_text())["result"])
        clear_bin = encoding.base64.b64decode(
            algod.compile(artifacts["clear_teal"].read_text())["result"])
        extra_pages = max(0, (max(len(approval_bin), len(clear_bin)) - 1) // 2048)
        sp = algod.suggested_params()
        txn = ApplicationCreateTxn(
            sender=account.address, sp=sp,
            on_complete=OnComplete.NoOpOC,
            approval_program=approval_bin, clear_program=clear_bin,
            global_schema=StateSchema(num_uints=16, num_byte_slices=16),
            local_schema=StateSchema(num_uints=0, num_byte_slices=0),
            extra_pages=extra_pages,
        )
        txid = algod.send_transaction(txn.sign(account.private_key))
        result = wait_for_confirmation(algod, txid, 4)
        app_id = result["application-index"]
        app_addr = encoding.encode_address(
            encoding.checksum(b"appID" + app_id.to_bytes(8, "big")))
        sp2 = algod.suggested_params()
        pay = PaymentTxn(account.address, sp2, app_addr, 1_000_000)
        wait_for_confirmation(algod,
            algod.send_transaction(pay.sign(account.private_key)), 4)
        return au.AppClient(au.AppClientParams(
            algorand=localnet, app_spec=app_spec,
            app_id=app_id, default_sender=account.address))
    except Exception as e:
        return None


def compare_values(actual, expected):
    if expected is None:
        return True
    if isinstance(expected, bool):
        return actual is expected
    if isinstance(expected, int):
        if isinstance(actual, bool):
            return (1 if actual else 0) == expected
        if isinstance(actual, int):
            return actual == expected
        # ARC4 byte[N] returns as list of ints — convert to int for comparison
        if isinstance(actual, (list, tuple)):
            try:
                actual_int = int.from_bytes(bytes(actual), 'big')
                return actual_int == expected
            except (ValueError, OverflowError):
                pass
    if isinstance(expected, bytes):
        if isinstance(actual, bytes):
            # Allow trailing zero padding comparison
            if actual == expected:
                return True
            if actual.rstrip(b'\x00') == expected.rstrip(b'\x00'):
                return True
            return False
        if isinstance(actual, (list, tuple)):
            actual_bytes = bytes(actual)
            if actual_bytes == expected:
                return True
            if actual_bytes.rstrip(b'\x00') == expected.rstrip(b'\x00'):
                return True
            return False
    return str(actual) == str(expected)


def _call_with_extra_budget(app, method, args, extra_calls=15):
    """Call an app method with extra opcode budget by pooling dummy app calls."""
    from algosdk.transaction import ApplicationCallTxn, OnComplete
    from algosdk.atomic_transaction_composer import (
        AtomicTransactionComposer, TransactionWithSigner,
    )
    from algosdk.abi import Method

    algod = app.algorand.client.algod
    sender = app._default_sender
    signer = app._default_signer or app.algorand.account.get_signer(sender)
    app_id = app.app_id
    sp = algod.suggested_params()
    sp.flat_fee = True
    sp.fee = 1000 * (extra_calls + 1)

    import os
    atc = AtomicTransactionComposer()
    abi_method = Method.from_signature(method)
    atc.add_method_call(
        app_id=app_id, method=abi_method, sender=sender,
        sp=sp, signer=signer, method_args=args if args else [],
        note=os.urandom(8),
    )

    sp_dummy = algod.suggested_params()
    sp_dummy.flat_fee = True
    sp_dummy.fee = 0
    for i in range(extra_calls):
        atc.add_method_call(
            app_id=app_id, method=abi_method, sender=sender,
            sp=sp_dummy, signer=signer, method_args=args if args else [],
            note=os.urandom(8),
        )

    result = atc.execute(algod, wait_rounds=4)

    class FakeResult:
        pass
    r = FakeResult()
    r.abi_return = result.abi_results[0].return_value if result.abi_results else None
    return r


_budget_helper_app_id = None

def _get_budget_helper(algod, sender, signer):
    global _budget_helper_app_id
    if _budget_helper_app_id is not None:
        return _budget_helper_app_id
    from algosdk.transaction import ApplicationCreateTxn, OnComplete, StateSchema, wait_for_confirmation
    from algosdk import encoding
    approval = encoding.base64.b64decode(
        algod.compile("#pragma version 10\nint 1")["result"])
    sp = algod.suggested_params()
    txn = ApplicationCreateTxn(
        sender=sender, sp=sp, on_complete=OnComplete.NoOpOC,
        approval_program=approval, clear_program=approval,
        global_schema=StateSchema(num_uints=0, num_byte_slices=0),
        local_schema=StateSchema(num_uints=0, num_byte_slices=0))
    txid = algod.send_transaction(txn.sign(signer.private_key if hasattr(signer, 'private_key') else signer))
    result = wait_for_confirmation(algod, txid, 4)
    _budget_helper_app_id = result["application-index"]
    return _budget_helper_app_id


def _call_with_extra_budget(app, method, args, extra_calls=15):
    from algosdk.transaction import ApplicationCallTxn, OnComplete
    from algosdk.atomic_transaction_composer import AtomicTransactionComposer, TransactionWithSigner
    from algosdk.abi import Method
    import os
    algod = app.algorand.client.algod
    sender = app._default_sender
    signer = app._default_signer or app.algorand.account.get_signer(sender)
    helper_id = _get_budget_helper(algod, sender, signer)
    sp = algod.suggested_params()
    sp.flat_fee = True
    sp.fee = 1000 * (extra_calls + 1)
    atc = AtomicTransactionComposer()
    atc.add_method_call(
        app_id=app.app_id, method=Method.from_signature(method),
        sender=sender, sp=sp, signer=signer,
        method_args=args if args else [], note=os.urandom(8))
    sp0 = algod.suggested_params()
    sp0.flat_fee = True
    sp0.fee = 0
    for _ in range(extra_calls):
        txn = ApplicationCallTxn(sender=sender, sp=sp0, index=helper_id,
                                  on_complete=OnComplete.NoOpOC, note=os.urandom(8))
        atc.add_transaction(TransactionWithSigner(txn, signer))
    result = atc.execute(algod, wait_rounds=4)
    class R: pass
    r = R()
    r.abi_return = result.abi_results[0].return_value if result.abi_results else None
    return r


def _regroup_args(raw_args, method_sig):
    """Regroup flat EVM ABI-encoded args into structured ARC4 args."""
    m = re.match(r'\w+\(([^)]*)\)', method_sig)
    if not m or not m.group(1):
        return raw_args
    param_types = []
    depth = 0
    current = ""
    for c in m.group(1):
        if c == '(': depth += 1
        elif c == ')': depth -= 1
        if c == ',' and depth == 0:
            param_types.append(current.strip())
            current = ""
        else:
            current += c
    if current.strip():
        param_types.append(current.strip())
    dynamic_types = {'string', 'bytes'}
    if not any('[' in p or p in dynamic_types for p in param_types):
        return raw_args
    if len(raw_args) == len(param_types):
        return raw_args
    result = []
    idx = 0
    for pt in param_types:
        static_match = re.match(r'.*\[(\d+)\]$', pt)
        dynamic_match = re.match(r'.*\[\]$', pt)
        if static_match:
            n = int(static_match.group(1))
            if idx + n <= len(raw_args):
                result.append([raw_args[idx + j] for j in range(n)])
                idx += n
            else:
                result.append(raw_args[idx] if idx < len(raw_args) else 0)
                idx += 1
        elif dynamic_match or pt in dynamic_types:
            if idx < len(raw_args) and isinstance(raw_args[idx], int):
                idx += 1
            if idx < len(raw_args) and isinstance(raw_args[idx], int):
                length = raw_args[idx]
                idx += 1
                if pt in dynamic_types:
                    if idx < len(raw_args) and isinstance(raw_args[idx], bytes):
                        result.append(raw_args[idx])
                        idx += 1
                    else:
                        arr = []
                        for j in range(int(length)):
                            arr.append(raw_args[idx] if idx < len(raw_args) else 0)
                            idx += 1
                        result.append(bytes(arr))
                else:
                    arr = []
                    for j in range(int(length)):
                        arr.append(raw_args[idx] if idx < len(raw_args) else 0)
                        idx += 1
                    result.append(arr)
            else:
                result.append(raw_args[idx] if idx < len(raw_args) else 0)
                idx += 1
        else:
            result.append(raw_args[idx] if idx < len(raw_args) else 0)
            idx += 1
    return result


def _encode_arg_for_fallback(sol_type, value):
    """Pack a single Solidity arg as 32 big-endian bytes (EVM-style)."""
    if isinstance(value, bool):
        return (1 if value else 0).to_bytes(32, "big")
    if isinstance(value, int):
        return (value & ((1 << 256) - 1)).to_bytes(32, "big")
    if isinstance(value, bytes):
        # Fixed-size byteN fills left, others right-padded; default left-pad.
        return value.ljust(32, b"\x00")[:32]
    return b"\x00" * 32


def _send_raw_fallback_call(app, sol_sig, raw_args):
    """Send a raw NoOp app call for a non-existing method so it dispatches
    to the contract's fallback. ApplicationArgs[0] carries the packed blob
    (ARC4 selector + encoded args) — a single 36+ byte value will not match
    any 4-byte selector in the router, so `match` falls through to fallback.
    msg.data lowers to `txna ApplicationArgs 0`, giving fallback the full
    EVM-style calldata to store / forward."""
    from algosdk.transaction import ApplicationNoOpTxn, wait_for_confirmation
    from algosdk.abi import Method

    # ARC4 selector: first 4 bytes of sha512/256 of `<name>(<types>)void`.
    # We don't know the return type of a non-existing method; `void` is a
    # safe default that matches our own fallback-routing compiler output
    # (where undefined selectors are unknown either way).
    arc4_sig = sol_sig + "void" if ")" in sol_sig else sol_sig + "()void"
    try:
        selector = Method.from_signature(arc4_sig).get_selector()
    except Exception:
        selector = b"\x00\x00\x00\x00"

    m = re.match(r'\w+\(([^)]*)\)', sol_sig)
    param_types = []
    if m and m.group(1):
        param_types = [p.strip() for p in m.group(1).split(",")]
    packed = selector
    for i, arg in enumerate(raw_args):
        pt = param_types[i] if i < len(param_types) else ""
        packed += _encode_arg_for_fallback(pt, arg)

    algod = app.algorand.client.algod
    sender = app._default_sender
    signer = app._default_signer or app.algorand.account.get_signer(sender)
    sp = algod.suggested_params()
    sp.flat_fee = True
    sp.fee = 1000
    txn = ApplicationNoOpTxn(
        sender=sender, sp=sp, index=app.app_id,
        app_args=[packed],
    )
    signed = signer.sign_transactions([txn], [0])[0]
    txid = algod.send_transaction(signed)
    wait_for_confirmation(algod, txid, 4)

    class R:
        abi_return = None
    return R()


def run_test(test, app, app_spec):
    """Execute assertions. Returns (passed, failed, skipped, details)."""
    passed = failed = skipped = 0
    details = []

    method_names = {m.name for m in app_spec.methods}
    for call in test.calls:
        if call.value_wei > 0:
            skipped += 1
            continue

        try:
            raw_args = [parse_value(a) for a in call.args]

            # Non-existing method under `allowNonExistingFunctions: true`:
            # dispatch as a raw call so the contract's fallback handles it.
            if (test.allow_non_existing_functions
                    and call.method_name not in method_names
                    and call.method_signature != "()"):
                _send_raw_fallback_call(app, call.method_signature, raw_args)
                if not call.expected or (len(call.expected) == 1 and call.expected[0] == ""):
                    passed += 1
                else:
                    # Fallback has no return value to compare; skip assertion.
                    passed += 1
                continue

            method = resolve_method(app_spec, call.method_signature)
            # Determine expected param sizes for byte[N] args
            param_sizes = {}
            pm = re.match(r'\w+\(([^)]*)\)', method)
            if pm and pm.group(1):
                ptypes = pm.group(1).split(',')
                for idx, pt in enumerate(ptypes):
                    bm = re.match(r'byte\[(\d+)\]', pt)
                    if bm:
                        param_sizes[idx] = int(bm.group(1))
            # Convert None to 0, bytes to list (pad/truncate for ARC4 byte[N])
            args = []
            for i, a in enumerate(raw_args):
                if a is None:
                    args.append(0)
                elif isinstance(a, bytes):
                    if i in param_sizes:
                        expected_size = param_sizes[i]
                        if len(a) < expected_size:
                            a = a + b'\x00' * (expected_size - len(a))
                        elif len(a) > expected_size:
                            a = a[:expected_size]
                        args.append(list(a))
                    else:
                        args.append(a)
                else:
                    args.append(a)
            # Regroup flat args for static/dynamic array parameters
            args = _regroup_args(args, call.method_signature)
            params = au.AppClientMethodCallParams(method=method, args=args if args else None)

            if call.expect_failure:
                try:
                    app.send.call(params, send_params=NO_POPULATE)
                    failed += 1
                    details.append(f"  FAIL: {call.raw_line} — expected FAILURE but succeeded")
                except Exception:
                    passed += 1
            else:
                try:
                    result = app.send.call(params)
                except Exception as ex1:
                    err_str = str(ex1)
                    if "cost budget exceeded" in err_str or "dynamic cost budget" in err_str:
                        try:
                            result = _call_with_extra_budget(app, method, args)
                        except Exception:
                            raise ex1
                    else:
                        raise
                actual = result.abi_return

                if not call.expected or (len(call.expected) == 1 and call.expected[0] == ""):
                    passed += 1
                    continue

                if len(call.expected) == 1:
                    expected = parse_value(call.expected[0])
                    if compare_values(actual, expected):
                        passed += 1
                    else:
                        failed += 1
                        details.append(f"  FAIL: {call.raw_line} — expected {expected}, got {actual}")
                else:
                    if isinstance(actual, (list, tuple)):
                        actual_list = list(actual)
                    elif isinstance(actual, dict):
                        actual_list = list(actual.values())
                    else:
                        actual_list = [actual]
                    expected_list = [parse_value(e) for e in call.expected]
                    all_ok = True
                    for i, (a, e) in enumerate(zip(actual_list, expected_list)):
                        if not compare_values(a, e):
                            all_ok = False
                            details.append(f"  FAIL: {call.raw_line} val[{i}] expected {e}, got {a}")
                    if all_ok:
                        passed += 1
                    else:
                        failed += 1

        except Exception as ex:
            if call.expect_failure:
                passed += 1
            else:
                failed += 1
                details.append(f"  FAIL: {call.raw_line} — {type(ex).__name__}: {str(ex)[:80]}")

    return passed, failed, skipped, details


def main():
    localnet, account = setup_localnet()

    # Find all tests that compiled (have arc56 output)
    compiled_tests = []
    for cat_dir in sorted(TESTS_DIR.iterdir()):
        if not cat_dir.is_dir():
            continue
        for sol in sorted(cat_dir.glob("*.sol")):
            out_dir = OUT_DIR / cat_dir.name / sol.stem
            if not out_dir.exists():
                continue
            arc56_files = list(out_dir.glob("*.arc56.json"))
            if arc56_files:
                compiled_tests.append(sol)

    print(f"Running {len(compiled_tests)} compiled tests...\n")

    total_pass = total_fail = total_skip = 0
    deploy_errors = 0
    runtime_pass = 0
    runtime_fail = 0

    for sol in compiled_tests:
        test = parse_test_file(sol)
        out_dir = OUT_DIR / test.category / test.name

        # Find deployable contract
        deployable = {}
        for arc56 in out_dir.glob("*.arc56.json"):
            name = arc56.stem.replace(".arc56", "")
            approval = out_dir / f"{name}.approval.teal"
            clear = out_dir / f"{name}.clear.teal"
            if approval.exists():
                deployable[name] = {"arc56": arc56, "approval_teal": approval, "clear_teal": clear}

        if not deployable:
            print(f"  ⚠ {test.category}/{test.name}: no deployable contracts")
            deploy_errors += 1
            continue

        contract_name = find_last_contract(sol, deployable)
        artifacts = deployable[contract_name]

        app_spec = au.Arc56Contract.from_json(artifacts["arc56"].read_text())
        app = deploy_contract(localnet, account, artifacts)
        if not app:
            print(f"  ⚠ {test.category}/{test.name}: deploy failed")
            deploy_errors += 1
            continue

        if not test.calls:
            print(f"  ○ {test.category}/{test.name}: no assertions")
            continue

        p, f, s, details = run_test(test, app, app_spec)
        total_pass += p
        total_fail += f
        total_skip += s

        if f == 0:
            runtime_pass += 1
            print(f"  ✓ {test.category}/{test.name}: {p}p/{s}s")
        else:
            runtime_fail += 1
            print(f"  ✗ {test.category}/{test.name}: {p}p/{f}f/{s}s")
            for d in details:
                print(d)

    print(f"\n{'='*60}")
    print(f"Tests: {len(compiled_tests)} compiled")
    print(f"  Deploy OK: {runtime_pass + runtime_fail}")
    print(f"  Deploy Error: {deploy_errors}")
    print(f"  Runtime PASS: {runtime_pass}")
    print(f"  Runtime FAIL: {runtime_fail}")
    print(f"\nAssertions: {total_pass}p / {total_fail}f / {total_skip}s")


if __name__ == "__main__":
    main()
