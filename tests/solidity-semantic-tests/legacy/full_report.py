#!/usr/bin/env python3
"""Generate a comprehensive report of all Solidity semantic tests.

For each test: compile, deploy, run assertions, classify result.
Outputs a detailed markdown report.

Usage: python3.10 full_report.py [--workers 4]
"""
import json
import os
import re
import subprocess
import sys
import traceback
from collections import defaultdict
from pathlib import Path

import algokit_utils as au
from algosdk import encoding
from algosdk.transaction import (
    ApplicationCreateTxn, ApplicationCallTxn, OnComplete, StateSchema,
    PaymentTxn, wait_for_confirmation,
)
from algosdk.atomic_transaction_composer import (
    AtomicTransactionComposer, TransactionWithSigner,
)
from algosdk.abi import Method

from parser import parse_test_file, parse_value

ROOT = Path(__file__).parent.parent.parent
COMPILER = ROOT / "build" / "puya-sol"
PUYA = ROOT.parent / "puya" / ".venv" / "bin" / "puya"
TESTS_DIR = Path(__file__).parent / "tests"
OUT_DIR = Path(__file__).parent / "out"
NO_POPULATE = au.SendParams(populate_app_call_resources=False)

SOL_TO_ARC4 = {
    "bool": "bool", "address": "address", "string": "string", "bytes": "byte[]",
}
for i in range(1, 33):
    SOL_TO_ARC4[f"bytes{i}"] = f"byte[{i}]"
for bits in [8, 16, 32, 64, 128, 256]:
    SOL_TO_ARC4[f"uint{bits}"] = f"uint{bits}"
    SOL_TO_ARC4[f"int{bits}"] = f"int{bits}"


def resolve_method(app_spec, sol_sig):
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


def find_last_contract(sol_path, deployable):
    content = sol_path.read_text()
    contracts = re.findall(r'(?:contract|library)\s+(\w+)', content)
    for name in reversed(contracts):
        if name in deployable:
            return name
    return list(deployable.keys())[-1]


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


_budget_helper_app_id = None


def _get_budget_helper(algod, sender, signer):
    global _budget_helper_app_id
    if _budget_helper_app_id is not None:
        return _budget_helper_app_id
    approval = encoding.base64.b64decode(
        algod.compile("#pragma version 10\nint 1")["result"])
    sp = algod.suggested_params()
    txn = ApplicationCreateTxn(
        sender=sender, sp=sp, on_complete=OnComplete.NoOpOC,
        approval_program=approval, clear_program=approval,
        global_schema=StateSchema(num_uints=0, num_byte_slices=0),
        local_schema=StateSchema(num_uints=0, num_byte_slices=0))
    txid = algod.send_transaction(txn.sign(signer.private_key))
    result = wait_for_confirmation(algod, txid, 4)
    _budget_helper_app_id = result["application-index"]
    return _budget_helper_app_id


def call_with_extra_budget(app, method, args, signer, extra_calls=15):
    algod = app.algorand.client.algod
    sender = app._default_sender
    helper_id = _get_budget_helper(algod, sender, signer)
    sp = algod.suggested_params()
    sp.flat_fee = True
    sp.fee = 1000 * (extra_calls + 1)
    atc = AtomicTransactionComposer()
    abi_method = Method.from_signature(method)
    atc.add_method_call(
        app_id=app.app_id, method=abi_method, sender=sender,
        sp=sp, signer=signer, method_args=args if args else [],
        note=os.urandom(8))
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
    m = re.match(r'\w+\(([^)]*)\)', method_sig)
    if not m or not m.group(1):
        return raw_args
    param_types = []
    depth = 0; current = ""
    for c in m.group(1):
        if c == '(': depth += 1
        elif c == ')': depth -= 1
        if c == ',' and depth == 0:
            param_types.append(current.strip()); current = ""
        else: current += c
    if current.strip(): param_types.append(current.strip())
    dynamic_types = {'string', 'bytes'}
    if not any('[' in p or p in dynamic_types for p in param_types):
        return raw_args
    if len(raw_args) == len(param_types):
        return raw_args
    result = []; idx = 0
    for pt in param_types:
        static_match = re.match(r'.*\[(\d+)\]$', pt)
        dynamic_match = re.match(r'.*\[\]$', pt)
        if static_match:
            n = int(static_match.group(1))
            if idx + n <= len(raw_args):
                result.append([raw_args[idx + j] for j in range(n)]); idx += n
            else:
                result.append(raw_args[idx] if idx < len(raw_args) else 0); idx += 1
        elif dynamic_match or pt in dynamic_types:
            if idx < len(raw_args) and isinstance(raw_args[idx], int):
                idx += 1
            if idx < len(raw_args) and isinstance(raw_args[idx], int):
                length = raw_args[idx]; idx += 1
                if pt in dynamic_types:
                    if idx < len(raw_args) and isinstance(raw_args[idx], bytes):
                        result.append(raw_args[idx]); idx += 1
                    else:
                        arr = []
                        for j in range(int(length)):
                            arr.append(raw_args[idx] if idx < len(raw_args) else 0); idx += 1
                        result.append(bytes(arr))
                else:
                    arr = []
                    for j in range(int(length)):
                        arr.append(raw_args[idx] if idx < len(raw_args) else 0); idx += 1
                    result.append(arr)
            else:
                result.append(raw_args[idx] if idx < len(raw_args) else 0); idx += 1
        else:
            result.append(raw_args[idx] if idx < len(raw_args) else 0); idx += 1
    return result


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
            extra_pages=extra_pages)
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


def run_single_test(test, localnet, account, signer):
    """Run a single test. Returns (status, detail, assertions_passed, assertions_failed)."""
    if test.skipped:
        return "SKIP", test.skip_reason, 0, 0

    if not test.calls:
        return "SKIP", "no assertions", 0, 0

    out_dir = OUT_DIR / test.category / test.name

    # Check compilation
    if not out_dir.exists():
        return "COMPILE_ERROR", "no output directory", 0, 0

    deployable = {}
    for arc56 in out_dir.glob("*.arc56.json"):
        cname = arc56.stem.replace(".arc56", "")
        approval = out_dir / f"{cname}.approval.teal"
        clear = out_dir / f"{cname}.clear.teal"
        if approval.exists():
            deployable[cname] = {"arc56": arc56, "approval_teal": approval, "clear_teal": clear}

    if not deployable:
        return "COMPILE_ERROR", "no deployable contracts", 0, 0

    contract_name = find_last_contract(test.source_path, deployable)
    artifacts = deployable[contract_name]

    try:
        app_spec = au.Arc56Contract.from_json(artifacts["arc56"].read_text())
    except Exception as e:
        return "COMPILE_ERROR", f"invalid arc56: {e}", 0, 0

    app = deploy_contract(localnet, account, artifacts)
    if not app:
        return "DEPLOY_ERROR", "deployment failed", 0, 0

    # Execute assertions
    passed = 0
    failed = 0
    fail_details = []

    for call in test.calls:
        if call.value_wei > 0:
            continue  # skip payable

        try:
            raw_args = [parse_value(a) for a in call.args]

            method = resolve_method(app_spec, call.method_signature)

            # Determine byte[N] param sizes
            param_sizes = {}
            pm = re.match(r'\w+\(([^)]*)\)', method)
            if pm and pm.group(1):
                ptypes = pm.group(1).split(',')
                for idx, pt in enumerate(ptypes):
                    bm = re.match(r'byte\[(\d+)\]', pt)
                    if bm:
                        param_sizes[idx] = int(bm.group(1))

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

            # Regroup for array/string params
            args = _regroup_args(args, call.method_signature)

            # Convert bytes to str for string params
            pm2 = re.match(r'\w+\(([^)]*)\)', method)
            if pm2 and pm2.group(1):
                rptypes = pm2.group(1).split(',')
                for i2, pt2 in enumerate(rptypes):
                    if pt2.strip() == 'string' and i2 < len(args) and isinstance(args[i2], bytes):
                        args[i2] = args[i2].decode('utf-8', errors='replace')

            params = au.AppClientMethodCallParams(method=method, args=args if args else None)

            if call.expect_failure:
                try:
                    app.send.call(params, send_params=NO_POPULATE)
                    failed += 1
                    fail_details.append(f"expected FAILURE but succeeded: {call.raw_line}")
                except Exception:
                    passed += 1
            else:
                try:
                    result = app.send.call(params)
                except Exception as ex1:
                    err_str = str(ex1)
                    if "cost budget exceeded" in err_str or "dynamic cost budget" in err_str:
                        try:
                            result = call_with_extra_budget(app, method, args, signer)
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
                        fail_details.append(f"{call.raw_line}: expected {expected}, got {actual}")
                else:
                    if isinstance(actual, (list, tuple)):
                        actual_list = list(actual)
                    elif isinstance(actual, dict):
                        actual_list = list(actual.values())
                    else:
                        actual_list = [actual]

                    expected_list = [parse_value(e) for e in call.expected]

                    # Handle ABI-encoded dynamic returns
                    if len(actual_list) != len(expected_list):
                        if (len(expected_list) >= 3
                            and isinstance(expected_list[0], int) and expected_list[0] == 32
                            and isinstance(expected_list[1], int)):
                            collapsed = expected_list[2:]
                            if len(collapsed) == 1 and isinstance(collapsed[0], bytes):
                                actual_bytes = bytes(actual_list) if all(isinstance(x, int) for x in actual_list) else actual
                                if compare_values(actual_bytes, collapsed[0]):
                                    passed += 1
                                    continue
                                else:
                                    failed += 1
                                    fail_details.append(f"{call.raw_line}: expected {collapsed[0]}, got {actual_bytes}")
                                    continue
                            expected_list = collapsed

                    if len(actual_list) != len(expected_list):
                        failed += 1
                        fail_details.append(f"{call.raw_line}: expected {len(expected_list)} values, got {len(actual_list)}")
                        continue

                    all_ok = True
                    for i, (a, e) in enumerate(zip(actual_list, expected_list)):
                        if not compare_values(a, e):
                            all_ok = False
                            fail_details.append(f"{call.raw_line}: val[{i}] expected {e}, got {a}")
                    if all_ok:
                        passed += 1
                    else:
                        failed += 1

        except Exception as ex:
            if call.expect_failure:
                passed += 1
            else:
                failed += 1
                fail_details.append(f"{call.raw_line}: {type(ex).__name__}: {str(ex)[:100]}")

    if failed > 0:
        return "RUNTIME_FAIL", "; ".join(fail_details[:3]), passed, failed
    return "PASS", f"{passed} assertions", passed, failed


def main():
    algod = au.ClientManager.get_algod_client(
        au.ClientManager.get_default_localnet_config("algod"))
    kmd = au.ClientManager.get_kmd_client(
        au.ClientManager.get_default_localnet_config("kmd"))
    localnet = au.AlgorandClient(au.AlgoSdkClients(algod=algod, kmd=kmd))
    localnet.set_suggested_params_cache_timeout(0)
    account = localnet.account.localnet_dispenser()
    localnet.account.set_signer_from_account(account)
    signer = localnet.account.get_signer(account.address)

    # Collect all tests
    all_results = []
    category_summary = defaultdict(lambda: {"PASS": 0, "SKIP": 0, "COMPILE_ERROR": 0, "DEPLOY_ERROR": 0, "RUNTIME_FAIL": 0, "total": 0})

    categories = sorted(d.name for d in TESTS_DIR.iterdir() if d.is_dir())
    total_tests = sum(len(list((TESTS_DIR / c).glob("*.sol"))) for c in categories)
    done = 0

    for cat in categories:
        cat_dir = TESTS_DIR / cat
        for sol in sorted(cat_dir.glob("*.sol")):
            test = parse_test_file(sol)
            status, detail, p, f = run_single_test(test, localnet, account, signer)
            all_results.append({
                "category": cat,
                "name": test.name,
                "status": status,
                "detail": detail,
                "assertions_passed": p,
                "assertions_failed": f,
            })
            category_summary[cat][status] += 1
            category_summary[cat]["total"] += 1
            done += 1
            if done % 25 == 0:
                counts = defaultdict(int)
                for r in all_results:
                    counts[r["status"]] += 1
                print(f"  Progress: {done}/{total_tests} "
                      f"(PASS={counts['PASS']} FAIL={counts['RUNTIME_FAIL']} "
                      f"CERR={counts['COMPILE_ERROR']} SKIP={counts['SKIP']})",
                      flush=True)

    # Generate report
    report_lines = []
    report_lines.append("# Solidity Semantic Test Suite — Full Report")
    report_lines.append("")

    # Overall summary
    totals = defaultdict(int)
    total_assertions_pass = 0
    total_assertions_fail = 0
    for r in all_results:
        totals[r["status"]] += 1
        total_assertions_pass += r["assertions_passed"]
        total_assertions_fail += r["assertions_failed"]

    report_lines.append("## Overall Summary")
    report_lines.append("")
    report_lines.append(f"| Status | Count | % |")
    report_lines.append(f"|--------|-------|---|")
    total = len(all_results)
    for status in ["PASS", "RUNTIME_FAIL", "COMPILE_ERROR", "DEPLOY_ERROR", "SKIP"]:
        count = totals[status]
        pct = count / total * 100 if total else 0
        report_lines.append(f"| {status} | {count} | {pct:.1f}% |")
    report_lines.append(f"| **Total** | **{total}** | |")
    report_lines.append("")
    report_lines.append(f"Assertions: {total_assertions_pass} passed, {total_assertions_fail} failed")
    report_lines.append("")

    # Per-category summary
    report_lines.append("## Per-Category Summary")
    report_lines.append("")
    report_lines.append(f"| Category | Total | Pass | Fail | Compile Err | Deploy Err | Skip |")
    report_lines.append(f"|----------|-------|------|------|-------------|------------|------|")
    for cat in sorted(category_summary.keys()):
        s = category_summary[cat]
        report_lines.append(
            f"| {cat} | {s['total']} | {s['PASS']} | {s['RUNTIME_FAIL']} | "
            f"{s['COMPILE_ERROR']} | {s['DEPLOY_ERROR']} | {s['SKIP']} |"
        )
    report_lines.append("")

    # Detailed results by status
    report_lines.append("## Tests That PASS")
    report_lines.append("")
    for r in sorted(all_results, key=lambda x: (x["category"], x["name"])):
        if r["status"] == "PASS":
            report_lines.append(f"- `{r['category']}/{r['name']}`: {r['detail']}")
    report_lines.append("")

    report_lines.append("## Tests That COMPILE but FAIL at Runtime")
    report_lines.append("")
    for r in sorted(all_results, key=lambda x: (x["category"], x["name"])):
        if r["status"] == "RUNTIME_FAIL":
            report_lines.append(f"- `{r['category']}/{r['name']}`: {r['assertions_passed']}p/{r['assertions_failed']}f — {r['detail'][:150]}")
    report_lines.append("")

    report_lines.append("## Tests That COMPILE but FAIL to Deploy")
    report_lines.append("")
    for r in sorted(all_results, key=lambda x: (x["category"], x["name"])):
        if r["status"] == "DEPLOY_ERROR":
            report_lines.append(f"- `{r['category']}/{r['name']}`")
    report_lines.append("")

    report_lines.append("## Tests That FAIL to Compile")
    report_lines.append("")
    for r in sorted(all_results, key=lambda x: (x["category"], x["name"])):
        if r["status"] == "COMPILE_ERROR":
            report_lines.append(f"- `{r['category']}/{r['name']}`: {r['detail'][:100]}")
    report_lines.append("")

    report_lines.append("## Skipped Tests")
    report_lines.append("")
    for r in sorted(all_results, key=lambda x: (x["category"], x["name"])):
        if r["status"] == "SKIP":
            report_lines.append(f"- `{r['category']}/{r['name']}`: {r['detail']}")
    report_lines.append("")

    report_path = Path(__file__).parent / "REPORT.md"
    report_path.write_text("\n".join(report_lines))
    print(f"\nReport written to {report_path}")

    # Also write JSON for programmatic access
    json_path = Path(__file__).parent / "report.json"
    json_path.write_text(json.dumps(all_results, indent=2))
    print(f"JSON written to {json_path}")

    # Print summary
    print(f"\n{'='*60}")
    print(f"Total: {total} tests")
    for status in ["PASS", "RUNTIME_FAIL", "COMPILE_ERROR", "DEPLOY_ERROR", "SKIP"]:
        count = totals[status]
        pct = count / total * 100 if total else 0
        print(f"  {status}: {count} ({pct:.1f}%)")
    print(f"Assertions: {total_assertions_pass} passed, {total_assertions_fail} failed")


if __name__ == "__main__":
    main()
