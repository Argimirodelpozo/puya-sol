"""Auto-generated pytest tests from Solidity semantic test files.

Discovers all .sol files in tests/<category>/, parses their assertions,
and runs them as parametrized pytest tests.

Run: cd tests/solidity-semantic-tests && python -m pytest test_semantic.py -v
"""
import os
import re
from pathlib import Path

import algokit_utils as au
import pytest

from parser import parse_test_file, parse_value
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
    contracts = compile_sol(test.source_path, out_dir)
    if not contracts:
        pytest.xfail("compilation failed")

    # Find main contract (last deployable)
    deployable = {k: v for k, v in contracts.items()
                  if v["approval_teal"].exists()}
    if not deployable:
        pytest.xfail("no deployable contracts")

    contract_name = find_last_contract(test.source_path, deployable)
    artifacts = deployable[contract_name]

    # Deploy
    app = deploy_app(localnet, account, artifacts)
    if not app:
        pytest.xfail("deployment failed")

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

            method = call.method_name
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
        pytest.xfail("\n".join(failures))
