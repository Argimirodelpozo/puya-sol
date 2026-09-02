"""asm return() ending a library / free function: the body's synthesized
epilogue is dead and lives in a ROOT subroutine (not a contract method) —
without its own dead-code pass puya rejects the unreachable code (seen on
poseidon-solidity PoseidonT3.hash under the Semaphore replay).
"""
import pytest

SOURCE = "puyasolRegression/contracts/asm_return_root_subroutine.sol"


@pytest.fixture
def app(harness):
    return harness.compile_and_deploy(SOURCE, "AsmReturnRoot")


def _ok(harness, app, sig, *args):
    r = harness.call(app, sig, *args, extra_fee=10_000)
    assert not r.reverted, f"{sig}{args}: {r.fail_message}"
    return r.abi_return


def test_library_asm_return_is_the_answer(harness, app):
    assert _ok(harness, app, "viaLibrary(uint256)", 5) == 12


def test_free_function_asm_return_is_the_answer(harness, app):
    assert _ok(harness, app, "viaFree(uint256)", 5) == 15
