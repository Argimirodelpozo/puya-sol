"""Preserve Yul functions without capturing caller locals or changing EVM frames."""

import json

import pytest

from framework import as_int

SOURCE = "puyasolRegression/contracts/yul_subroutines.sol"


@pytest.mark.parametrize("slot_layout", [False, True])
def test_yul_subroutine_semantics(harness, slot_layout):
    app = harness.compile_and_deploy(
        SOURCE, extra_args=["--evm-storage-layout"] if slot_layout else [])

    def call(method, *args):
        value = harness.call(app, method, *args, extra_fee=50_000).abi_return
        return tuple(map(as_int, value)) if isinstance(value, (tuple, list)) else as_int(value)

    for n in range(7):
        assert call("mutual(uint256)", n) == (int(n % 2 == 0), n % 2)
    values = [i * 10 + j for i in range(4) for j in range(3)]
    for n in (0, 1, 5, 12, 20):
        assert call("nestedLeave(uint256)", n) == (sum(values[:n]), min(n, 12))
    for n, expected in ((0, (7, 0)), (1, (0, 9)), (2, (11, 113))):
        assert call("switchLeave(uint256)", n) == expected
    for offset in (512, 4095, 4096, 8161):
        assert call("memoryShared(uint256,uint256)", offset, 37) == (37, 38, 32)
    assert call("memoryArgument(uint256[])", [3, 9]) == (3, 4, 2)
    assert call("memoryFixed(uint256[2])", [7, 8]) == (7, 17, 8)
    word, data = harness.call(app, "memoryBytes(bytes)", b"abc", extra_fee=50_000).abi_return
    assert as_int(word) == int.from_bytes(b"abc".ljust(32, b"\x00"), "big")
    assert bytes(data) == b"\xa5bc"
    assert call("order()") == (1110, 11)
    for value in (0, 27, (1 << 256) - 1):
        assert call("staticCalldata(uint256)", value) == value
        assert call("evmReturn(uint256)", value) == value
    data = bytes(range(50))
    for offset in (0, 31, 49, 50, 100):
        word = int.from_bytes(data[offset:offset + 32].ljust(32, b"\x00"), "big")
        assert call("calldataShared(bytes,uint256)", data, offset) == (word, 164, word)
    assert call("writeStorage(uint256,bool)", 41, False) == 41
    assert harness.call(app, "writeStorage(uint256,bool)", 99, True,
                        expect_revert=True, extra_fee=50_000).reverted
    assert call("stored()") == 41
    assert call("writeStorage(uint256,bool)", 43, False) == 43


def test_yul_call_argument_capture(harness):
    app = harness.compile_and_deploy(SOURCE)
    result = harness.call(app, "captureValues()", extra_fee=50_000).abi_return
    assert tuple(map(as_int, result)) == (100, 201, 302, 403)


def test_yul_functions_survive_frontend(harness):
    harness.compile(SOURCE)
    roots = json.loads((harness.out_dir / "awst.json").read_text())
    subs = [root for root in roots if root.get("_type") == "Subroutine"
            and "::__yul_" in root["id"]]
    assert len(subs) == 23
    calldata_subs = [sub for sub in subs if any(arg["name"] == "__cd_blob"
                    for arg in sub["args"])]
    assert len(calldata_subs) == 4
    assert all(sub.get("inline") is not True for sub in subs)
    # The two whole-EVM-return helpers retain the enclosing Solidity frame.
    assert not any(sub["name"].endswith(("_finish", "_forward"))
                   and not any(arg["name"] == "__cd_blob" for arg in sub["args"])
                   for sub in subs)


@pytest.mark.parametrize("slot_layout", [False, True])
def test_yul_library_and_free_function_roots(harness, slot_layout):
    artifacts = harness.compile(
        "puyasolRegression/contracts/yul_subroutine_roots.sol",
        extra_args=["--evm-storage-layout"] if slot_layout else [])
    first = harness.deploy(artifacts, "FirstYulHost")
    second = harness.deploy(artifacts, "SecondYulHost")
    for value in (0, 17, 1 << 128):
        assert as_int(harness.call(first, "f(uint256)", value).abi_return) == value * 2 + 1
        assert as_int(harness.call(second, "f(uint256)", value).abi_return) == (value + 1) * 2
    harness.call(first, "write(uint256)", 31)
    harness.call(second, "write(uint256)", 47)
    assert as_int(harness.call(first, "first()").abi_return) == 31
    assert as_int(harness.call(second, "second()").abi_return) == 47


@pytest.mark.parametrize("slot_layout", [False, True])
def test_yul_deployable_libraries_have_distinct_helper_emissions(harness, slot_layout):
    artifacts = harness.compile(
        "puyasolRegression/contracts/yul_library_emissions.sol",
        extra_args=["--evm-storage-layout"] if slot_layout else [])
    first = harness.deploy(artifacts, "FirstYulLibrary")
    second = harness.deploy(artifacts, "SecondYulLibrary")
    for n in (0, 7, 2**128):
        assert as_int(harness.call(first, "f(uint256)", n).abi_return) == n * 2
        assert as_int(harness.call(second, "f(uint256)", n).abi_return) == n + 1
