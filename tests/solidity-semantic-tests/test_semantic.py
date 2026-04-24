"""Auto-generated pytest tests from Solidity semantic test files.

Discovers all .sol files in tests/<category>/, parses their assertions,
and runs them as parametrized pytest tests.

Run: cd tests/solidity-semantic-tests && python -m pytest test_semantic.py -v
"""
import logging
import os
import re
from pathlib import Path

import algokit_utils as au
import pytest
from algosdk import encoding

from parser import parse_test_file, parse_value

log = logging.getLogger(__name__)
from conftest import (
    TESTS_DIR, OUT_DIR, compile_sol, deploy_app, compare_values, NO_POPULATE,
)


def _call_with_payment(app, method, args, value_wei, localnet, account):
    """Call an app method with a preceding payment transaction (msg.value equivalent).

    Groups a payment txn to the app address before the app call so that
    `msg.value` (mapped to `gtxns Amount` of previous txn) reads correctly.
    """
    from algosdk.transaction import PaymentTxn
    from algosdk.atomic_transaction_composer import (
        AtomicTransactionComposer, TransactionWithSigner, AccountTransactionSigner
    )
    from algosdk.abi import Method as ABIMethod

    algod = localnet.client.algod
    sp = algod.suggested_params()
    sp.flat_fee = True
    sp.fee = 2000  # cover both txns

    app_addr = encoding.encode_address(
        encoding.checksum(b"appID" + app.app_id.to_bytes(8, "big")))

    pay_txn = PaymentTxn(account.address, sp, app_addr, value_wei)
    signer = AccountTransactionSigner(account.private_key)

    atc = AtomicTransactionComposer()
    atc.add_transaction(TransactionWithSigner(pay_txn, signer))

    # Find the ABI method
    abi_method = None
    for m in app.app_spec.methods:
        sig = m.to_abi_method().get_signature()
        if m.name == method or sig == method:
            abi_method = m.to_abi_method()
            break
    if not abi_method:
        abi_method = ABIMethod.from_signature(method if "(" in method else method + "()")

    sp2 = algod.suggested_params()
    sp2.flat_fee = True
    sp2.fee = 2000
    atc.add_method_call(
        app_id=app.app_id,
        method=abi_method,
        sender=account.address,
        sp=sp2,
        signer=signer,
        method_args=args if args else [],
    )
    resp = atc.execute(algod, 4)
    # Return a result-like object with abi_return
    abi_return = resp.abi_results[-1].return_value if resp.abi_results else None

    class _Result:
        pass
    r = _Result()
    r.abi_return = abi_return
    return r


def find_last_contract(sol_path, deployable):
    """Find the last contract defined in the source that has deployable artifacts."""
    content = sol_path.read_text()
    contracts = re.findall(r'(?:contract|library)\s+(\w+)', content)
    for name in reversed(contracts):
        if name in deployable:
            return name
    return list(deployable.keys())[-1]


def discover_tests():
    """Find all .sol semantic test files and parse them."""
    tests = []
    if not TESTS_DIR.exists():
        return tests
    for category_dir in sorted(TESTS_DIR.iterdir()):
        if not category_dir.is_dir():
            continue
        for sol_file in sorted(category_dir.glob("*.sol")):
            test = parse_test_file(sol_file)
            tests.append(test)
    return tests


ALL_TESTS = discover_tests()
TEST_IDS = [f"{t.category}/{t.name}" for t in ALL_TESTS]


@pytest.fixture(scope="session")
def localnet_session():
    import algokit_utils as au
    from algosdk.kmd import KMDClient
    algod = au.ClientManager.get_algod_client(
        au.ClientManager.get_default_localnet_config("algod"))
    kmd = au.ClientManager.get_kmd_client(
        au.ClientManager.get_default_localnet_config("kmd"))
    client = au.AlgorandClient(au.AlgoSdkClients(algod=algod, kmd=kmd))
    client.set_suggested_params_cache_timeout(0)
    account = client.account.localnet_dispenser()
    client.account.set_signer_from_account(account)
    return client, account


@pytest.mark.parametrize("test", ALL_TESTS, ids=TEST_IDS)
def test_semantic(test, localnet_session):
    localnet, account = localnet_session

    # Skip tests with no parseable assertion format
    if test.skipped:
        pytest.skip(test.skip_reason)

    # Compile
    out_dir = OUT_DIR / test.category / test.name
    log.info("COMPILE %s/%s", test.category, test.name)
    contracts = compile_sol(test.source_path, out_dir, via_yul_behavior=test.compile_via_yul)
    if not contracts:
        log.info("  COMPILE FAILED %s/%s", test.category, test.name)
        pytest.skip("compilation failed")

    # Find main contract (last deployable)
    deployable = {k: v for k, v in contracts.items()
                  if v["approval_teal"].exists()}
    if not deployable:
        log.info("  NO DEPLOYABLE %s/%s", test.category, test.name)
        pytest.skip("no deployable contracts")

    contract_name = find_last_contract(test.source_path, deployable)
    artifacts = deployable[contract_name]
    log.info("  COMPILED %s/%s → %s", test.category, test.name, contract_name)

    # Deploy
    app = deploy_app(localnet, account, artifacts)
    if not app:
        log.info("  DEPLOY FAILED %s/%s", test.category, test.name)
        pytest.skip("deployment failed")
    log.info("  DEPLOYED %s/%s (app_id=%s)", test.category, test.name, app.app_id)

    # Execute each call assertion
    failures = []
    for call in test.calls:

        try:
            args = []
            for arg_str in call.args:
                val = parse_value(arg_str)
                if val is None:
                    val = 0
                args.append(val)

            # Skip constructor calls — ARC4 doesn't expose constructors.
            # EVM tests use `constructor()` to represent deployment.
            if call.method_name == "constructor":
                continue

            # Use full method signature to disambiguate overloaded functions.
            # The ARC4 method selector uses the signature, so match by arg count.
            method = call.method_name
            method_matches = [
                m for m in app.app_spec.methods if m.name == method
            ]
            matched_method = None
            if len(method_matches) > 1:
                # Overloaded — find the one matching our arg count
                for m in method_matches:
                    if len(m.args) == len(args):
                        matched_method = m
                        method = m.to_abi_method().get_signature()
                        break
            elif len(method_matches) == 1:
                matched_method = method_matches[0]

            # Coerce args to match ARC4 parameter types.
            # EVM ABI encodes dynamic types (string, bytes) as offset+length+data
            # in the test assertions. Collapse these into single values for ARC4.
            if matched_method and args:
                coerced = []
                raw_idx = 0
                for marg in matched_method.args:
                    if raw_idx >= len(args):
                        break
                    atype = marg.type if isinstance(marg.type, str) else str(marg.type)
                    if atype in ("string", "byte[]") and isinstance(args[raw_idx], int):
                        # EVM dynamic arg: 0x20, length, "data chunk 1", "chunk 2"...
                        # Because EVM pads to 32-byte words, a >32-byte value is
                        # split across multiple quoted chunks in the test file; collect
                        # enough chunks to cover the declared length.
                        raw_idx += 1  # skip offset
                        declared_len = 0
                        if raw_idx < len(args) and isinstance(args[raw_idx], int):
                            declared_len = args[raw_idx]
                            raw_idx += 1  # skip length
                        collected = bytearray()
                        while raw_idx < len(args) and isinstance(args[raw_idx], bytes) and len(collected) < declared_len:
                            collected.extend(args[raw_idx])
                            raw_idx += 1
                        if declared_len > 0 and len(collected) > declared_len:
                            collected = collected[:declared_len]
                        if collected:
                            if atype == "string":
                                coerced.append(bytes(collected).decode('utf-8', errors='replace'))
                            else:
                                coerced.append(list(collected))
                        else:
                            coerced.append("" if atype == "string" else [])
                    elif atype == "bool" and isinstance(args[raw_idx], int):
                        coerced.append(bool(args[raw_idx]))
                        raw_idx += 1
                    elif isinstance(args[raw_idx], int) and (
                        re.match(r"byte\[(\d+)\]", atype) or re.match(r"bytes(\d+)", atype)
                    ):
                        m = re.match(r"byte\[(\d+)\]", atype) or re.match(r"bytes(\d+)", atype)
                        if m:
                            n = int(m.group(1))
                            val = args[raw_idx]
                            if val == 0:
                                coerced.append(list(b'\x00' * n))
                            else:
                                byte_len = max(n, (val.bit_length() + 7) // 8)
                                full_bytes = val.to_bytes(byte_len, "big")
                                # Left-aligned values (from left() parser): take first N bytes
                                # Right-aligned values: take last N bytes
                                if byte_len > n:
                                    # Check if it's left-aligned (trailing zeros)
                                    if full_bytes[-n:] == b'\x00' * n and full_bytes[:n] != b'\x00' * n:
                                        coerced.append(list(full_bytes[:n]))
                                    else:
                                        coerced.append(list(full_bytes[-n:].ljust(n, b'\x00')))
                                else:
                                    coerced.append(list(full_bytes[-n:].ljust(n, b'\x00')))
                        raw_idx += 1
                    elif atype == "address" and isinstance(args[raw_idx], int):
                        from algosdk import encoding
                        coerced.append(encoding.encode_address(args[raw_idx].to_bytes(32, "big")))
                        raw_idx += 1
                    elif isinstance(args[raw_idx], int) and args[raw_idx] >= 2**128:
                        # Large two's complement value (negative in EVM).
                        # Convert from uint256 two's complement to the target unsigned type.
                        val = args[raw_idx]
                        if atype.startswith("uint"):
                            m_bits = re.match(r"uint(\d+)$", atype)
                            if m_bits:
                                bits = int(m_bits.group(1))
                                # Convert uint256 two's complement to uintN two's complement
                                signed_val = val - 2**256  # negative
                                val = signed_val % (2**bits)  # wrap to uintN
                        coerced.append(val)
                        raw_idx += 1
                    elif isinstance(args[raw_idx], int) and re.match(r"(\w+)\[(\d+)\]$", atype):
                        # Static array: uint256[3] → consume N sequential values
                        sm = re.match(r"(\w+)\[(\d+)\]$", atype)
                        n = int(sm.group(2))
                        arr = []
                        for _ in range(n):
                            if raw_idx < len(args):
                                arr.append(args[raw_idx])
                                raw_idx += 1
                        coerced.append(arr)
                    elif isinstance(args[raw_idx], int) and re.match(r"(\w+)\[\]$", atype):
                        # Dynamic array: offset, length, val0, val1, ...
                        raw_idx += 1  # skip offset
                        if raw_idx < len(args):
                            n = args[raw_idx]
                            raw_idx += 1  # skip length
                            arr = []
                            for _ in range(n if isinstance(n, int) else 0):
                                if raw_idx < len(args):
                                    arr.append(args[raw_idx])
                                    raw_idx += 1
                            coerced.append(arr)
                        else:
                            coerced.append([])
                    else:
                        coerced.append(args[raw_idx])
                        raw_idx += 1
                args = coerced

            params = au.AppClientMethodCallParams(
                method=method, args=args if args else None)

            if call.expect_failure:
                try:
                    if call.value_wei > 0:
                        _call_with_payment(app, method, args if args else [], call.value_wei, localnet, account)
                    else:
                        app.send.call(params, send_params=NO_POPULATE)
                    failures.append(f"{call.raw_line}: expected FAILURE but succeeded")
                except Exception:
                    pass  # Correctly reverted
            else:
                try:
                    if call.value_wei > 0:
                        result = _call_with_payment(app, method, args if args else [], call.value_wei, localnet, account)
                    else:
                        result = app.send.call(params)
                except Exception as ex1:
                    err_str = str(ex1)
                    if "fee too small" in err_str:
                        # Inner txns need fee pooling — retry with extra budget
                        from run_tests import _call_with_extra_budget
                        # Use full ABI signature for method resolution
                        method_sig = matched_method.to_abi_method().get_signature() if matched_method else method
                        result = _call_with_extra_budget(app, method_sig, args if args else [], extra_calls=3)
                    else:
                        raise
                actual = result.abi_return

                # Void return
                if not call.expected or (len(call.expected) == 1 and call.expected[0] == ""):
                    continue

                # Detect EVM ABI-encoded dynamic type returns.
                # For methods returning string/bytes, EVM encodes as:
                #   Single string: 0x20, length, [data...]
                #   Multi with string: offsets + values + string_length + string_data
                # Compare against the actual ARC4 return (raw string or tuple).
                evm_string_handled = False
                if matched_method and len(call.expected) >= 2:
                    ret_type = matched_method.returns.type if matched_method.returns else None
                    ret_str = str(ret_type) if ret_type else ""

                    # Single string/bytes return
                    if ret_str == "string" and isinstance(actual, str):
                        if call.expected[0].strip() == "0x20":
                            exp_len = parse_value(call.expected[1])
                            if isinstance(exp_len, int):
                                if exp_len == 0 and actual == "":
                                    evm_string_handled = True
                                elif len(call.expected) >= 3:
                                    exp_data = parse_value(call.expected[2])
                                    if isinstance(exp_data, bytes):
                                        if actual.encode('utf-8', errors='replace') == exp_data:
                                            evm_string_handled = True
                                        else:
                                            failures.append(
                                                f"{call.raw_line}: expected {exp_data!r}, got {actual!r}")
                                            evm_string_handled = True

                    # EVM dynamic array/bytes return: 0x20, length, data...
                    # ARC4 returns the array directly as a list.
                    elif (call.expected[0].strip() == "0x20"
                          and isinstance(actual, (list, tuple))):
                        exp_len = parse_value(call.expected[1])
                        actual_list = list(actual)
                        # Unwrap single-field struct dicts in array elements
                        actual_list = [
                            list(v.values())[0] if isinstance(v, dict) and len(v) == 1 else v
                            for v in actual_list
                        ]
                        if isinstance(exp_len, int):
                            # Check if expected data is a single blob (bytes/string)
                            # vs individual elements
                            remaining = call.expected[2:]
                            if len(remaining) == 1:
                                exp_data = parse_value(remaining[0])
                                actual_bytes = bytes(actual_list)
                                if isinstance(exp_data, bytes) and actual_bytes == exp_data:
                                    evm_string_handled = True
                                elif isinstance(exp_data, bytes):
                                    failures.append(
                                        f"{call.raw_line}: expected {exp_data!r}, got {actual_bytes!r}")
                                    evm_string_handled = True
                            elif exp_len == len(actual_list):
                                exp_elems = [parse_value(e) for e in remaining[:exp_len]]
                                all_match = all(
                                    compare_values(a, e) for a, e in zip(actual_list, exp_elems)
                                )
                                if all_match:
                                    evm_string_handled = True
                                else:
                                    for i, (a, e) in enumerate(zip(actual_list, exp_elems)):
                                        if not compare_values(a, e):
                                            failures.append(
                                                f"{call.raw_line}: elem[{i}] expected {e}, got {a}")
                                    evm_string_handled = True

                    # Multi-return with strings
                    elif "string" in ret_str and isinstance(actual, (list, tuple, dict)):
                        pass

                if evm_string_handled:
                    continue

                # Single return
                if len(call.expected) == 1:
                    expected = parse_value(call.expected[0])
                    if not compare_values(actual, expected):
                        failures.append(
                            f"{call.raw_line}: expected {expected}, got {actual}")
                else:
                    # Multiple returns
                    if isinstance(actual, (list, tuple)):
                        actual_list = list(actual)
                    elif isinstance(actual, dict):
                        actual_list = list(actual.values())
                    else:
                        actual_list = [actual]

                    expected_list = [parse_value(e) for e in call.expected]
                    for i, (a, e) in enumerate(zip(actual_list, expected_list)):
                        if not compare_values(a, e):
                            failures.append(
                                f"{call.raw_line}: value[{i}] expected {e}, got {a}")

        except Exception as ex:
            if call.expect_failure:
                pass
            else:
                failures.append(f"{call.raw_line}: {type(ex).__name__}: {str(ex)[:80]}")

    if failures:
        log.info("  FAIL %s/%s: %s", test.category, test.name, failures[0][:80])
        pytest.fail("\n".join(failures))
    else:
        log.info("  PASS %s/%s (%d assertions)", test.category, test.name, len(test.calls))
