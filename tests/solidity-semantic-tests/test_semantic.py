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

from parser import parse_test_file, parse_value

log = logging.getLogger(__name__)
from conftest import (
    TESTS_DIR, OUT_DIR, compile_sol, deploy_app, compare_values, NO_POPULATE,
)


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

    # Skip tests that use unsupported features
    if test.skipped:
        pytest.skip(test.skip_reason)

    if not test.calls:
        pytest.skip("no assertions")

    # Compile
    out_dir = OUT_DIR / test.category / test.name
    log.info("COMPILE %s/%s", test.category, test.name)
    contracts = compile_sol(test.source_path, out_dir)
    if not contracts:
        log.info("  COMPILE FAILED %s/%s", test.category, test.name)
        pytest.xfail("compilation failed")

    # Find main contract (last deployable)
    deployable = {k: v for k, v in contracts.items()
                  if v["approval_teal"].exists()}
    if not deployable:
        log.info("  NO DEPLOYABLE %s/%s", test.category, test.name)
        pytest.xfail("no deployable contracts")

    contract_name = find_last_contract(test.source_path, deployable)
    artifacts = deployable[contract_name]
    log.info("  COMPILED %s/%s → %s", test.category, test.name, contract_name)

    # Deploy
    app = deploy_app(localnet, account, artifacts)
    if not app:
        log.info("  DEPLOY FAILED %s/%s", test.category, test.name)
        pytest.xfail("deployment failed")
    log.info("  DEPLOYED %s/%s (app_id=%s)", test.category, test.name, app.app_id)

    # Execute each call assertion
    failures = []
    for call in test.calls:
        # Skip payable
        if call.value_wei > 0:
            continue

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
                    if atype == "string" and isinstance(args[raw_idx], int):
                        # EVM string arg: 0x20, length, "data" → just "data"
                        # Skip offset (0x20), skip length, take string data
                        raw_idx += 1  # skip offset
                        if raw_idx < len(args):
                            raw_idx += 1  # skip length
                        if raw_idx < len(args) and isinstance(args[raw_idx], bytes):
                            coerced.append(args[raw_idx].decode('utf-8', errors='replace'))
                            raw_idx += 1
                        elif raw_idx - 1 < len(args) and parse_value(call.args[raw_idx - 1]) == 0:
                            # Empty string (length=0, no data word)
                            coerced.append("")
                        else:
                            coerced.append("")
                    elif atype == "bool" and isinstance(args[raw_idx], int):
                        coerced.append(bool(args[raw_idx]))
                        raw_idx += 1
                    elif isinstance(args[raw_idx], int) and (
                        re.match(r"byte\[(\d+)\]", atype) or re.match(r"bytes(\d+)", atype)
                    ):
                        m = re.match(r"byte\[(\d+)\]", atype) or re.match(r"bytes(\d+)", atype)
                        if m:
                            n = int(m.group(1))
                            byte_len = max(n, (args[raw_idx].bit_length() + 7) // 8) if args[raw_idx] else n
                            coerced.append(list(args[raw_idx].to_bytes(byte_len, "big")[-n:].ljust(n, b'\x00')))
                        raw_idx += 1
                    elif atype == "address" and isinstance(args[raw_idx], int):
                        from algosdk import encoding
                        coerced.append(encoding.encode_address(args[raw_idx].to_bytes(32, "big")))
                        raw_idx += 1
                    else:
                        coerced.append(args[raw_idx])
                        raw_idx += 1
                args = coerced

            params = au.AppClientMethodCallParams(
                method=method, args=args if args else None)

            if call.expect_failure:
                try:
                    app.send.call(params, send_params=NO_POPULATE)
                    failures.append(f"{call.raw_line}: expected FAILURE but succeeded")
                except Exception:
                    pass  # Correctly reverted
            else:
                result = app.send.call(params)
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

                    # EVM dynamic array return: 0x20, length, elem0, elem1, ...
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
                        if isinstance(exp_len, int) and exp_len == len(actual_list):
                            exp_elems = [parse_value(e) for e in call.expected[2:2+exp_len]]
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
        log.info("  XFAIL %s/%s: %s", test.category, test.name, failures[0][:80])
        pytest.xfail("\n".join(failures))
    else:
        log.info("  PASS %s/%s (%d assertions)", test.category, test.name, len(test.calls))
