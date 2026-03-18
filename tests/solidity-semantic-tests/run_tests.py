#!/usr/bin/env python3
"""Run Solidity semantic tests against puya-sol compiled contracts on AVM localnet.

Usage:
    python run_tests.py [--category smoke] [--limit 10] [--verbose]

Phases:
  1. Parse .sol test files from tests/<category>/
  2. Compile each with puya-sol
  3. Deploy to localnet
  4. Execute assertion calls and compare results

Results are printed as PASS/FAIL/SKIP/COMPILE_ERROR/DEPLOY_ERROR.
"""
import argparse
import json
import os
import subprocess
import sys
import traceback
from pathlib import Path

import algokit_utils as au
from algosdk import encoding
from algosdk.transaction import (
    ApplicationCreateTxn, OnComplete, StateSchema,
    PaymentTxn, wait_for_confirmation,
)

import re
from parser import parse_test_file, parse_value, SemanticTest


def find_last_contract(sol_path, deployable):
    """Find the last contract defined in the source that has deployable artifacts."""
    content = sol_path.read_text()
    contracts = re.findall(r'(?:contract|library)\s+(\w+)', content)
    for name in reversed(contracts):
        if name in deployable:
            return name
    return list(deployable.keys())[-1]

ROOT = Path(__file__).parent.parent.parent
COMPILER = ROOT / "build" / "puya-sol"
PUYA = ROOT.parent / "puya" / ".venv" / "bin" / "puya"
TESTS_DIR = Path(__file__).parent / "tests"
OUT_DIR = Path(__file__).parent / "out"

NO_POPULATE = au.SendParams(populate_app_call_resources=False)


def setup_localnet():
    """Connect to localnet and return (localnet, account)."""
    algod = au.ClientManager.get_algod_client(
        au.ClientManager.get_default_localnet_config("algod")
    )
    kmd = au.ClientManager.get_kmd_client(
        au.ClientManager.get_default_localnet_config("kmd")
    )
    localnet = au.AlgorandClient(au.AlgoSdkClients(algod=algod, kmd=kmd))
    localnet.set_suggested_params_cache_timeout(0)
    account = localnet.account.localnet_dispenser()
    localnet.account.set_signer_from_account(account)
    return localnet, account


def compile_test(sol_path: Path, out_dir: Path) -> dict | None:
    """Compile a .sol file. Returns dict of contract artifacts or None on failure."""
    out_dir.mkdir(parents=True, exist_ok=True)
    result = subprocess.run(
        [str(COMPILER), "--source", str(sol_path),
         "--output-dir", str(out_dir),
         "--puya-path", str(PUYA)],
        capture_output=True, text=True, timeout=120,
    )
    if result.returncode != 0:
        return None

    contracts = {}
    for arc56 in out_dir.glob("*.arc56.json"):
        name = arc56.stem.replace(".arc56", "")
        contracts[name] = {
            "arc56": arc56,
            "approval_teal": out_dir / f"{name}.approval.teal",
            "clear_teal": out_dir / f"{name}.clear.teal",
        }
    return contracts


def deploy_contract(localnet, account, artifacts) -> au.AppClient | None:
    """Deploy a compiled contract. Returns AppClient or None."""
    try:
        app_spec = au.Arc56Contract.from_json(artifacts["arc56"].read_text())
        algod = localnet.client.algod

        approval_bin = encoding.base64.b64decode(
            algod.compile(artifacts["approval_teal"].read_text())["result"]
        )
        clear_bin = encoding.base64.b64decode(
            algod.compile(artifacts["clear_teal"].read_text())["result"]
        )

        max_size = max(len(approval_bin), len(clear_bin))
        extra_pages = max(0, (max_size - 1) // 2048)

        sp = algod.suggested_params()
        txn = ApplicationCreateTxn(
            sender=account.address, sp=sp,
            on_complete=OnComplete.NoOpOC,
            approval_program=approval_bin, clear_program=clear_bin,
            global_schema=StateSchema(num_uints=16, num_byte_slices=16),
            local_schema=StateSchema(num_uints=0, num_byte_slices=0),
            extra_pages=extra_pages,
        )
        signed = txn.sign(account.private_key)
        txid = algod.send_transaction(signed)
        result = wait_for_confirmation(algod, txid, 4)
        app_id = result["application-index"]

        # Fund
        app_addr = encoding.encode_address(
            encoding.checksum(b"appID" + app_id.to_bytes(8, "big"))
        )
        sp2 = algod.suggested_params()
        pay = PaymentTxn(account.address, sp2, app_addr, 1_000_000)
        txid2 = algod.send_transaction(pay.sign(account.private_key))
        wait_for_confirmation(algod, txid2, 4)

        return au.AppClient(
            au.AppClientParams(
                algorand=localnet, app_spec=app_spec,
                app_id=app_id, default_sender=account.address,
            )
        )
    except Exception:
        return None


SOL_TO_ARC4 = {
    "uint8": "uint8", "uint16": "uint16", "uint32": "uint32", "uint64": "uint64",
    "uint128": "uint128", "uint256": "uint256",
    "int8": "int8", "int16": "int16", "int32": "int32", "int64": "int64",
    "int128": "int128", "int256": "int256",
    "bool": "bool", "address": "address", "string": "string",
    "bytes": "byte[]",
}
for i in range(1, 33):
    SOL_TO_ARC4[f"bytes{i}"] = f"byte[{i}]"


def resolve_method(app_spec, sol_sig):
    """Resolve a Solidity method signature to an ARC56 method."""
    m = re.match(r'(\w+)\((.*)\)', sol_sig)
    if not m:
        return sol_sig
    name, params_str = m.group(1), m.group(2)
    methods = [method for method in app_spec.methods if method.name == name]
    if len(methods) == 0:
        return sol_sig
    if len(methods) == 1:
        method = methods[0]
        args_part = ",".join(a.type for a in method.args)
        ret_part = method.returns.type if method.returns and method.returns.type != "void" else "void"
        return f"{name}({args_part}){ret_part}"
    sol_params = [p.strip() for p in params_str.split(",")] if params_str else []
    arc4_params = [SOL_TO_ARC4.get(p, p) for p in sol_params]
    for method in methods:
        method_types = [a.type for a in method.args]
        if method_types == arc4_params:
            args_part = ",".join(method_types)
            ret_part = method.returns.type if method.returns and method.returns.type != "void" else "void"
            return f"{name}({args_part}){ret_part}"
    for method in methods:
        if len(method.args) == len(sol_params):
            args_part = ",".join(a.type for a in method.args)
            ret_part = method.returns.type if method.returns and method.returns.type != "void" else "void"
            return f"{name}({args_part}){ret_part}"
    return sol_sig


_budget_helper_app_id = None

def _get_budget_helper(algod, sender, signer):
    """Deploy (once) a tiny app that just approves — used for opcode budget pooling."""
    global _budget_helper_app_id
    if _budget_helper_app_id is not None:
        return _budget_helper_app_id
    from algosdk.transaction import ApplicationCreateTxn, OnComplete, StateSchema, wait_for_confirmation
    from algosdk import encoding
    # Minimal TEAL: #pragma version 10\nint 1
    approval = encoding.base64.b64decode(
        algod.compile("#pragma version 10\nint 1")["result"])
    clear = approval
    sp = algod.suggested_params()
    txn = ApplicationCreateTxn(
        sender=sender, sp=sp, on_complete=OnComplete.NoOpOC,
        approval_program=approval, clear_program=clear,
        global_schema=StateSchema(num_uints=0, num_byte_slices=0),
        local_schema=StateSchema(num_uints=0, num_byte_slices=0),
    )
    txid = algod.send_transaction(txn.sign(signer.private_key if hasattr(signer, 'private_key') else signer))
    result = wait_for_confirmation(algod, txid, 4)
    _budget_helper_app_id = result["application-index"]
    return _budget_helper_app_id


def _call_with_extra_budget(app, method, args, extra_calls=15):
    """Call an app method with extra opcode budget by pooling dummy app calls."""
    from algosdk.transaction import ApplicationCallTxn, OnComplete
    from algosdk.atomic_transaction_composer import (
        AtomicTransactionComposer, TransactionWithSigner,
    )
    from algosdk.abi import Method
    import os

    algod = app.algorand.client.algod
    sender = app._default_sender
    signer = app._default_signer or app.algorand.account.get_signer(sender)
    app_id = app.app_id

    # Get or deploy the budget helper app
    helper_id = _get_budget_helper(algod, sender, signer)

    sp = algod.suggested_params()
    sp.flat_fee = True
    sp.fee = 1000 * (extra_calls + 1)

    atc = AtomicTransactionComposer()

    # The real method call
    abi_method = Method.from_signature(method)
    atc.add_method_call(
        app_id=app_id, method=abi_method, sender=sender,
        sp=sp, signer=signer, method_args=args if args else [],
        note=os.urandom(8),
    )

    # Dummy budget-pooling calls to the helper app (cheap, always succeeds)
    sp_dummy = algod.suggested_params()
    sp_dummy.flat_fee = True
    sp_dummy.fee = 0
    for _ in range(extra_calls):
        txn = ApplicationCallTxn(
            sender=sender, sp=sp_dummy, index=helper_id,
            on_complete=OnComplete.NoOpOC, note=os.urandom(8),
        )
        atc.add_transaction(TransactionWithSigner(txn, signer))

    result = atc.execute(algod, wait_rounds=4)

    class FakeResult:
        pass
    r = FakeResult()
    r.abi_return = result.abi_results[0].return_value if result.abi_results else None
    return r


def _regroup_args(raw_args, method_sig):
    """Regroup flat EVM ABI-encoded args into structured ARC4 args.

    For static arrays like uint256[4], EVM passes 4 separate values inline.
    For dynamic arrays like uint256[], EVM passes offset, length, then elements.
    ARC4 expects a single list arg for each.
    """
    import re as _re
    m = _re.match(r'\w+\(([^)]*)\)', method_sig)
    if not m or not m.group(1):
        return raw_args

    # Parse parameter types
    param_types = []
    depth = 0
    current = ""
    for c in m.group(1):
        if c == '(' : depth += 1
        elif c == ')': depth -= 1
        if c == ',' and depth == 0:
            param_types.append(current.strip())
            current = ""
        else:
            current += c
    if current.strip():
        param_types.append(current.strip())

    # Check if all params are simple scalars — no regrouping needed
    dynamic_types = {'string', 'bytes'}
    has_complex = any('[' in p or p in dynamic_types for p in param_types)
    if not has_complex:
        return raw_args

    # Check if arg count matches param count — if so, no regrouping needed
    if len(raw_args) == len(param_types):
        return raw_args

    # Regroup: consume flat args according to param types
    result = []
    idx = 0
    for pt in param_types:
        static_match = _re.match(r'.*\[(\d+)\]$', pt)
        dynamic_match = _re.match(r'.*\[\]$', pt)

        if static_match:
            # Static array: consume N elements inline
            n = int(static_match.group(1))
            if idx + n <= len(raw_args):
                result.append([raw_args[idx + j] for j in range(n)])
                idx += n
            else:
                result.append(raw_args[idx] if idx < len(raw_args) else 0)
                idx += 1
        elif dynamic_match or pt in dynamic_types:
            # Dynamic type: EVM format is offset, length, then data
            # Skip offset (int), read length (int), then consume data
            if idx < len(raw_args) and isinstance(raw_args[idx], int):
                idx += 1  # skip offset
            if idx < len(raw_args) and isinstance(raw_args[idx], int):
                length = raw_args[idx]
                idx += 1
                if pt in dynamic_types:
                    # String/bytes: the next value should be the actual bytes data
                    if idx < len(raw_args) and isinstance(raw_args[idx], bytes):
                        result.append(raw_args[idx])
                        idx += 1
                    else:
                        # Collect as array elements
                        arr = []
                        for j in range(int(length)):
                            arr.append(raw_args[idx] if idx < len(raw_args) else 0)
                            idx += 1
                        result.append(bytes(arr) if pt in dynamic_types else arr)
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


def execute_call(app, call, app_spec=None, verbose=False):
    """Execute a test call and return (passed, detail)."""
    try:
        # Build args
        raw_args = []
        for arg_str in call.args:
            val = parse_value(arg_str)
            raw_args.append(val)

        args = []
        # Determine expected parameter sizes from ARC4 method spec
        resolved = resolve_method(app_spec, call.method_signature) if app_spec else call.method_signature
        param_sizes = {}  # index → expected byte count for byte[N] params
        if app_spec:
            import re as _re
            pm = _re.match(r'\w+\(([^)]*)\)', resolved)
            if pm and pm.group(1):
                ptypes = pm.group(1).split(',')
                for idx, pt in enumerate(ptypes):
                    bm = _re.match(r'byte\[(\d+)\]', pt)
                    if bm:
                        param_sizes[idx] = int(bm.group(1))
        for i, a in enumerate(raw_args):
            if a is None:
                args.append(0)
            elif isinstance(a, bytes):
                if i in param_sizes:
                    # byte[N] static array param: pad/truncate and convert to list
                    expected_size = param_sizes[i]
                    if len(a) < expected_size:
                        a = a + b'\x00' * (expected_size - len(a))
                    elif len(a) > expected_size:
                        a = a[:expected_size]
                    args.append(list(a))
                else:
                    # string/bytes/other: pass as bytes directly
                    args.append(a)
            else:
                args.append(a)

        # Skip payable calls (AVM has no native currency in app calls)
        if call.value_wei > 0:
            return None, "payable skipped"

        method = resolve_method(app_spec, call.method_signature) if app_spec else call.method_signature

        # Regroup flat args for static/dynamic array parameters
        args = _regroup_args(args, call.method_signature)

        # Convert bytes to str for string params (algokit expects str)
        if app_spec:
            resolved_m = resolve_method(app_spec, call.method_signature)
            import re as _re
            pm2 = _re.match(r'\w+\(([^)]*)\)', resolved_m)
            if pm2 and pm2.group(1):
                rptypes = pm2.group(1).split(',')
                for i2, pt2 in enumerate(rptypes):
                    if pt2.strip() == 'string' and i2 < len(args) and isinstance(args[i2], bytes):
                        args[i2] = args[i2].decode('utf-8', errors='replace')

        params = au.AppClientMethodCallParams(method=method, args=args if args else None)

        if call.expect_failure:
            try:
                app.send.call(params, send_params=NO_POPULATE)
                return False, "expected FAILURE but succeeded"
            except Exception:
                return True, "correctly reverted"
        else:
            try:
                result = app.send.call(params)
            except Exception as ex1:
                err_str = str(ex1)
                if "cost budget exceeded" in err_str or "dynamic cost budget" in err_str:
                    try:
                        result = _call_with_extra_budget(app, method, args)
                    except Exception:
                        raise ex1  # If budget retry also fails, report original error
                else:
                    raise
            actual = result.abi_return

            # Parse expected values
            if len(call.expected) == 0 or (len(call.expected) == 1 and call.expected[0] == ""):
                # Void return
                return True, "void ok"

            # If ABI return is None but we expect values, check transaction logs
            # for structured return data (emitted by assembly return() in void functions)
            if actual is None and len(call.expected) > 0:
                try:
                    # Get transaction logs
                    tx_id = result.tx_ids[0] if hasattr(result, 'tx_ids') and result.tx_ids else None
                    logs = None
                    if hasattr(result, 'confirmations') and result.confirmations:
                        logs = result.confirmations[0].get('logs', [])
                    elif tx_id:
                        tx_info = app.algorand.client.algod.pending_transaction_info(tx_id)
                        logs = tx_info.get('logs', [])

                    if logs:
                        import base64
                        # Use the last log entry (structured return data)
                        raw = base64.b64decode(logs[-1])
                        # Skip ARC4 return prefix if present
                        if raw[:4] == b'\x15\x1f\x7c\x75':
                            raw = raw[4:]
                        # Decode as N × 32-byte uint256 values
                        n_values = len(raw) // 32
                        if n_values > 0:
                            actual = [int.from_bytes(raw[i*32:(i+1)*32], 'big') for i in range(n_values)]
                            if len(actual) == 1:
                                actual = actual[0]
                except Exception:
                    pass  # Fall through to normal comparison

            if len(call.expected) == 1:
                expected = parse_value(call.expected[0])
                if _compare_values(actual, expected):
                    return True, f"{actual}"
                return False, f"expected {expected}, got {actual}"

            # Multiple return values
            if isinstance(actual, (list, tuple)):
                actual_list = list(actual)
            elif isinstance(actual, dict):
                actual_list = list(actual.values())
            else:
                actual_list = [actual]

            expected_list = [parse_value(e) for e in call.expected]

            # Handle EVM ABI-encoded dynamic return values:
            # If expected has [offset, length, data] pattern, decode it.
            if len(actual_list) != len(expected_list):
                if (len(expected_list) >= 3
                    and isinstance(expected_list[0], int) and expected_list[0] == 32
                    and isinstance(expected_list[1], int)):
                    # ABI-encoded dynamic type: skip offset+length, take data
                    collapsed = expected_list[2:]
                    if len(collapsed) == 1 and isinstance(collapsed[0], bytes):
                        # Single bytes data — compare as bytes
                        actual_bytes = bytes(actual_list) if all(isinstance(x, int) for x in actual_list) else actual
                        if _compare_values(actual_bytes, collapsed[0]):
                            return True, f"{actual}"
                        return False, f"expected {collapsed[0]}, got {actual_bytes}"
                    expected_list = collapsed

            if len(actual_list) != len(expected_list):
                return False, f"expected {len(expected_list)} values, got {len(actual_list)}"

            for i, (a, e) in enumerate(zip(actual_list, expected_list)):
                if not _compare_values(a, e):
                    return False, f"value[{i}]: expected {e}, got {a}"

            return True, f"{actual_list}"

    except Exception as ex:
        if call.expect_failure:
            return True, "correctly reverted"
        return False, f"exception: {str(ex)[:100]}"


def _compare_values(actual, expected):
    """Compare an actual AVM return value with an expected semantic test value."""
    if expected is None:
        return True  # void or FAILURE (already handled)
    if isinstance(expected, bool):
        return actual is expected
    if isinstance(expected, int):
        if isinstance(actual, bool):
            return (1 if actual else 0) == expected
        if isinstance(actual, int):
            return actual == expected
        if isinstance(actual, (list, tuple)):
            try:
                actual_int = int.from_bytes(bytes(actual), 'big')
                return actual_int == expected
            except (ValueError, OverflowError):
                pass
    if isinstance(expected, bytes):
        if isinstance(actual, bytes):
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


def run_test(test: SemanticTest, localnet, account, verbose=False):
    """Run a single semantic test. Returns (status, details)."""
    if test.skipped:
        return "SKIP", test.skip_reason

    # Compile
    out_dir = OUT_DIR / test.category / test.name
    contracts = compile_test(test.source_path, out_dir)
    if not contracts:
        return "COMPILE_ERROR", "compilation failed"

    # Find the main contract (usually the last/largest one)
    # Filter out non-deployable ones
    deployable = {k: v for k, v in contracts.items()
                  if v["approval_teal"].exists()}
    if not deployable:
        return "COMPILE_ERROR", "no deployable contracts"

    # Use the last contract defined in source (Solidity convention)
    contract_name = find_last_contract(test.source_path, deployable)
    artifacts = deployable[contract_name]

    # Load app spec for method resolution
    app_spec = au.Arc56Contract.from_json(artifacts["arc56"].read_text())

    # Deploy
    app = deploy_contract(localnet, account, artifacts)
    if not app:
        return "DEPLOY_ERROR", "deployment failed"

    # Execute calls
    passed = 0
    failed = 0
    skipped = 0
    details = []

    for call in test.calls:
        ok, detail = execute_call(app, call, app_spec, verbose)
        if ok is None:
            skipped += 1
            if verbose:
                details.append(f"  SKIP: {call.raw_line} ({detail})")
        elif ok:
            passed += 1
            if verbose:
                details.append(f"  PASS: {call.raw_line}")
        else:
            failed += 1
            details.append(f"  FAIL: {call.raw_line} — {detail}")

    if failed > 0:
        return "FAIL", f"{passed}p/{failed}f/{skipped}s" + "\n".join([""] + details)
    return "PASS", f"{passed}p/{skipped}s"


def main():
    parser = argparse.ArgumentParser(description="Run Solidity semantic tests")
    parser.add_argument("--category", default=None, help="Specific category to run")
    parser.add_argument("--limit", type=int, default=None, help="Max tests per category")
    parser.add_argument("--verbose", "-v", action="store_true")
    parser.add_argument("--file", default=None, help="Run a specific .sol file")
    args = parser.parse_args()

    localnet, account = setup_localnet()

    if args.file:
        test = parse_test_file(Path(args.file))
        status, detail = run_test(test, localnet, account, args.verbose)
        print(f"{status}: {test.name} — {detail}")
        return

    # Discover categories
    if args.category:
        categories = [args.category]
    else:
        categories = sorted(d.name for d in TESTS_DIR.iterdir() if d.is_dir())

    results = {"PASS": 0, "FAIL": 0, "SKIP": 0, "COMPILE_ERROR": 0, "DEPLOY_ERROR": 0}

    for cat in categories:
        cat_dir = TESTS_DIR / cat
        if not cat_dir.exists():
            continue

        tests = sorted(cat_dir.glob("*.sol"))
        if args.limit:
            tests = tests[:args.limit]

        print(f"\n=== {cat} ({len(tests)} tests) ===")

        for sol_file in tests:
            test = parse_test_file(sol_file)
            status, detail = run_test(test, localnet, account, args.verbose)
            results[status] += 1

            icon = {"PASS": "✓", "FAIL": "✗", "SKIP": "○",
                    "COMPILE_ERROR": "⚠", "DEPLOY_ERROR": "⚠"}.get(status, "?")
            short_detail = detail.split("\n")[0][:60]
            print(f"  {icon} {test.name}: {short_detail}")

    print(f"\n{'='*50}")
    total = sum(results.values())
    print(f"Total: {total} tests")
    for status, count in sorted(results.items()):
        if count > 0:
            print(f"  {status}: {count}")


if __name__ == "__main__":
    main()
