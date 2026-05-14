"""Tests for the tryCatch category."""
import pytest

from framework import (
    Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted,
    as_int, as_bytes,
)

pytestmark = pytest.mark.skip(reason="try/catch is an EVM construct for catching REVERT data from external calls. AVM uses inner-txn budget pools and ATC composition; revert recovery is fundamentally different. Not supported in puya-sol.")


def test_assert_(harness):
    """tryCatch/contracts/assert.sol"""
    app = harness.compile_and_deploy("tryCatch/contracts/assert.sol")
    # f(bool): true -> 1
    r = harness.call(app, "f(bool)", True)
    assert as_int(r.abi_return) == 1
    # f(bool): false -> 2
    r = harness.call(app, "f(bool)", False)
    assert as_int(r.abi_return) == 2

def test_assert_pre_byzantium(harness):
    """tryCatch/contracts/assert_pre_byzantium.sol"""
    app = harness.compile_and_deploy("tryCatch/contracts/assert_pre_byzantium.sol", evm_version='spuriousDragon')
    # f(bool): true -> 1
    r = harness.call(app, "f(bool)", True)
    assert as_int(r.abi_return) == 1
    # f(bool): false -> 2
    r = harness.call(app, "f(bool)", False)
    assert as_int(r.abi_return) == 2

def test_create(harness):
    """tryCatch/contracts/create.sol"""
    app = harness.compile_and_deploy("tryCatch/contracts/create.sol")
    # f() -> false, 0x40, 13, "test message."
    r = harness.call(app, "f()")
    # TODO: verify expected: false | 0x40 | 13 | "test message."
    assert not r.reverted
    # g() -> true, 0x40, 7, "success"
    r = harness.call(app, "g()")
    # TODO: verify expected: true | 0x40 | 7 | "success"
    assert not r.reverted

def test_invalid_error_encoding(harness):
    """tryCatch/contracts/invalid_error_encoding.sol"""
    app = harness.compile_and_deploy("tryCatch/contracts/invalid_error_encoding.sol")
    # f1() -> 2
    r = harness.call(app, "f1()")
    assert as_int(r.abi_return) == 2
    # f1a() -> 2
    r = harness.call(app, "f1a()")
    assert as_int(r.abi_return) == 2
    # f1b() -> FAILURE, hex"12345678", 0x0, 0, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"
    r = harness.call(app, "f1b()", expect_revert=True)
    assert r.reverted
    # f1c() -> 2
    r = harness.call(app, "f1c()")
    assert as_int(r.abi_return) == 2
    # f2() -> 2
    r = harness.call(app, "f2()")
    assert as_int(r.abi_return) == 2
    # f2a() -> 2
    r = harness.call(app, "f2a()")
    assert as_int(r.abi_return) == 2
    # f2b() -> FAILURE, hex"08c379a0", 0x100, 0, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"
    r = harness.call(app, "f2b()", expect_revert=True)
    assert r.reverted
    # f2c() -> 1
    r = harness.call(app, "f2c()")
    assert as_int(r.abi_return) == 1
    # f3() -> 2
    r = harness.call(app, "f3()")
    assert as_int(r.abi_return) == 2
    # f3a() -> 2
    r = harness.call(app, "f3a()")
    assert as_int(r.abi_return) == 2
    # f3b() -> FAILURE, hex"08c379a0", 0x20, 48, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"
    r = harness.call(app, "f3b()", expect_revert=True)
    assert r.reverted
    # f3c() -> 1
    r = harness.call(app, "f3c()")
    assert as_int(r.abi_return) == 1
    # f4() -> 1
    r = harness.call(app, "f4()")
    assert as_int(r.abi_return) == 1
    # f4a() -> 1
    r = harness.call(app, "f4a()")
    assert as_int(r.abi_return) == 1
    # f4b() -> 1
    r = harness.call(app, "f4b()")
    assert as_int(r.abi_return) == 1
    # f4c() -> 1
    r = harness.call(app, "f4c()")
    assert as_int(r.abi_return) == 1

def test_lowLevel(harness):
    """tryCatch/contracts/lowLevel.sol"""
    app = harness.compile_and_deploy("tryCatch/contracts/lowLevel.sol")
    # f(bool): true -> 1, 2, 96, 0
    r = harness.call(app, "f(bool)", True)
    assert tuple(as_int(x) for x in r.abi_return) == (1, 2, 96, 0)
    # f(bool): false -> 0, 0, 96, 100, 0x8c379a000000000000000000000000000000000000000000000000000000000, 0x2000000000000000000000000000000000000000000000000000000000, 0x76d657373616765000000000000000000000000000000000000000000, 0
    r = harness.call(app, "f(bool)", False)
    # TODO: verify structural decoding matches expected: 0, 0, 96, 100, 3963877391197344453575983046348115674221700746820753546331534351508065746944, 862718293348820473429344482784628181556388621521298319395315527974912, 200240400974129698026711077260797157360650913022725854495054588018688, 0
    assert not r.reverted

def test_malformed_error(harness):
    """tryCatch/contracts/malformed_error.sol"""
    app = harness.compile_and_deploy("tryCatch/contracts/malformed_error.sol")
    # a() -> 0x00
    r = harness.call(app, "a()")
    assert as_int(r.abi_return) == 0
    # b() -> 0x00
    r = harness.call(app, "b()")
    assert as_int(r.abi_return) == 0
    # b2() -> 0x00
    r = harness.call(app, "b2()")
    assert as_int(r.abi_return) == 0
    # b3() -> 0x20, 0x00
    r = harness.call(app, "b3()")
    assert tuple(as_int(x) for x in r.abi_return) == (32, 0)
    # c() -> 0x20, 7, "abcdefg"
    r = harness.call(app, "c()")
    assert r.abi_return == 'abcdefg'
    # d() -> 0x20, 7, "abcdefg"
    r = harness.call(app, "d()")
    assert r.abi_return == 'abcdefg'

def test_malformed_panic(harness):
    """tryCatch/contracts/malformed_panic.sol"""
    app = harness.compile_and_deploy("tryCatch/contracts/malformed_panic.sol")
    # a() -> 0x00
    r = harness.call(app, "a()")
    assert as_int(r.abi_return) == 0
    # b() -> 0x00
    r = harness.call(app, "b()")
    assert as_int(r.abi_return) == 0
    # c() -> 0x43
    r = harness.call(app, "c()")
    assert as_int(r.abi_return) == 67
    # d() -> 0x43
    r = harness.call(app, "d()")
    assert as_int(r.abi_return) == 67

def test_malformed_panic_2(harness):
    """tryCatch/contracts/malformed_panic_2.sol"""
    app = harness.compile_and_deploy("tryCatch/contracts/malformed_panic_2.sol")
    # a() -> FAILURE, hex"4e487b"
    r = harness.call(app, "a()", expect_revert=True)
    assert r.reverted
    # b() -> FAILURE, hex"4e487b710000"
    r = harness.call(app, "b()", expect_revert=True)
    assert r.reverted
    # c() -> 0x43
    r = harness.call(app, "c()")
    assert as_int(r.abi_return) == 67
    # d() -> 0x43
    r = harness.call(app, "d()")
    assert as_int(r.abi_return) == 67

def test_malformed_panic_3(harness):
    """tryCatch/contracts/malformed_panic_3.sol"""
    app = harness.compile_and_deploy("tryCatch/contracts/malformed_panic_3.sol")
    # a() -> FAILURE, hex"4e487b"
    r = harness.call(app, "a()", expect_revert=True)
    assert r.reverted
    # b() -> FAILURE, hex"4e487b710000"
    r = harness.call(app, "b()", expect_revert=True)
    assert r.reverted
    # c() -> 0x43
    r = harness.call(app, "c()")
    assert as_int(r.abi_return) == 67
    # d() -> 0x43
    r = harness.call(app, "d()")
    assert as_int(r.abi_return) == 67

def test_malformed_panic_4(harness):
    """tryCatch/contracts/malformed_panic_4.sol"""
    app = harness.compile_and_deploy("tryCatch/contracts/malformed_panic_4.sol")
    # a() -> 0x00
    r = harness.call(app, "a()")
    assert as_int(r.abi_return) == 0
    # b() -> 0x00
    r = harness.call(app, "b()")
    assert as_int(r.abi_return) == 0
    # c() -> 0x43
    r = harness.call(app, "c()")
    assert as_int(r.abi_return) == 67

def test_nested(harness):
    """tryCatch/contracts/nested.sol"""
    app = harness.compile_and_deploy("tryCatch/contracts/nested.sol")
    # f(bool,bool): true, true -> 1, 2, 96, 7, "success"
    r = harness.call(app, "f(bool,bool)", True, True)
    # TODO: verify expected: 1 | 2 | 96 | 7 | "success"
    assert not r.reverted
    # f(bool,bool): true, false -> 12, 0, 96, 7, "failure"
    r = harness.call(app, "f(bool,bool)", True, False)
    # TODO: verify expected: 12 | 0 | 96 | 7 | "failure"
    assert not r.reverted
    # f(bool,bool): false, true -> 99, 0, 96, 7, "failure"
    r = harness.call(app, "f(bool,bool)", False, True)
    # TODO: verify expected: 99 | 0 | 96 | 7 | "failure"
    assert not r.reverted
    # f(bool,bool): false, false -> 99, 0, 96, 7, "failure"
    r = harness.call(app, "f(bool,bool)", False, False)
    # TODO: verify expected: 99 | 0 | 96 | 7 | "failure"
    assert not r.reverted

def test_panic(harness):
    """tryCatch/contracts/panic.sol"""
    app = harness.compile_and_deploy("tryCatch/contracts/panic.sol")
    # onlyPanic(bool,uint256,uint256): true, 7, 6 -> 1, 0x00
    r = harness.call(app, "onlyPanic(bool,uint256,uint256)", True, 7, 6)
    assert tuple(as_int(x) for x in r.abi_return) == (1, 0)
    # onlyPanic(bool,uint256,uint256): true, 6, 7 -> 0x00, 0x11
    r = harness.call(app, "onlyPanic(bool,uint256,uint256)", True, 6, 7)
    assert tuple(as_int(x) for x in r.abi_return) == (0, 17)
    # onlyPanic(bool,uint256,uint256): false, 7, 6 -> FAILURE, hex"08c379a0", 0x20, 7, "failure"
    r = harness.call(app, "onlyPanic(bool,uint256,uint256)", False, 7, 6, expect_revert=True)
    assert r.reverted
    # onlyPanic(bool,uint256,uint256): false, 6, 7 -> FAILURE, hex"08c379a0", 0x20, 7, "failure"
    r = harness.call(app, "onlyPanic(bool,uint256,uint256)", False, 6, 7, expect_revert=True)
    assert r.reverted
    # panicAndError(bool,uint256,uint256): true, 7, 6 -> 1, 0x00, 0x60, 0x00
    r = harness.call(app, "panicAndError(bool,uint256,uint256)", True, 7, 6)
    assert tuple(as_int(x) for x in r.abi_return) == (1, 0, 96, 0)
    # panicAndError(bool,uint256,uint256): true, 6, 7 -> 0x00, 0x11, 0x60, 0x00
    r = harness.call(app, "panicAndError(bool,uint256,uint256)", True, 6, 7)
    assert tuple(as_int(x) for x in r.abi_return) == (0, 17, 96, 0)
    # panicAndError(bool,uint256,uint256): false, 7, 6 -> 0x00, 0x00, 0x60, 7, "failure"
    r = harness.call(app, "panicAndError(bool,uint256,uint256)", False, 7, 6)
    # TODO: verify expected: 0x00 | 0x00 | 0x60 | 7 | "failure"
    assert not r.reverted
    # panicAndError(bool,uint256,uint256): false, 6, 7 -> 0x00, 0x00, 0x60, 7, "failure"
    r = harness.call(app, "panicAndError(bool,uint256,uint256)", False, 6, 7)
    # TODO: verify expected: 0x00 | 0x00 | 0x60 | 7 | "failure"
    assert not r.reverted

def test_require(harness):
    """tryCatch/contracts/require.sol"""
    app = harness.compile_and_deploy("tryCatch/contracts/require.sol")
    # f(bool): true -> 1
    r = harness.call(app, "f(bool)", True)
    assert as_int(r.abi_return) == 1
    # f(bool): false -> 2
    r = harness.call(app, "f(bool)", False)
    assert as_int(r.abi_return) == 2

def test_require_pre_byzantium(harness):
    """tryCatch/contracts/require_pre_byzantium.sol"""
    app = harness.compile_and_deploy("tryCatch/contracts/require_pre_byzantium.sol", evm_version='spuriousDragon')
    # f(bool): true -> 1
    r = harness.call(app, "f(bool)", True)
    assert as_int(r.abi_return) == 1
    # f(bool): false -> 2
    r = harness.call(app, "f(bool)", False)
    assert as_int(r.abi_return) == 2

def test_return_function(harness):
    """tryCatch/contracts/return_function.sol"""
    app = harness.compile_and_deploy("tryCatch/contracts/return_function.sol")
    # f() -> 0x1, 0x1234946644cd0000000000000000, 9
    r = harness.call(app, "f()")
    assert tuple(as_int(x) for x in r.abi_return) == (1, 369249164733261476318508228804608, 9)

def test_simple(harness):
    """tryCatch/contracts/simple.sol"""
    app = harness.compile_and_deploy("tryCatch/contracts/simple.sol")
    # f(bool): true -> 1, 2
    r = harness.call(app, "f(bool)", True)
    assert tuple(as_int(x) for x in r.abi_return) == (1, 2)
    # f(bool): false -> 9, 10
    r = harness.call(app, "f(bool)", False)
    assert tuple(as_int(x) for x in r.abi_return) == (9, 10)

def test_simple_notuple(harness):
    """tryCatch/contracts/simple_notuple.sol"""
    app = harness.compile_and_deploy("tryCatch/contracts/simple_notuple.sol")
    # f(bool): true -> 13
    r = harness.call(app, "f(bool)", True)
    assert as_int(r.abi_return) == 13
    # f(bool): false -> 9
    r = harness.call(app, "f(bool)", False)
    assert as_int(r.abi_return) == 9

def test_structured(harness):
    """tryCatch/contracts/structured.sol"""
    app = harness.compile_and_deploy("tryCatch/contracts/structured.sol")
    # f(bool): true -> 1, 2, 0x60, 7, "success"
    r = harness.call(app, "f(bool)", True)
    # TODO: verify expected: 1 | 2 | 0x60 | 7 | "success"
    assert not r.reverted
    # f(bool): false -> 0, 0, 0x60, 7, "message"
    r = harness.call(app, "f(bool)", False)
    # TODO: verify expected: 0 | 0 | 0x60 | 7 | "message"
    assert not r.reverted

def test_structuredAndLowLevel(harness):
    """tryCatch/contracts/structuredAndLowLevel.sol"""
    app = harness.compile_and_deploy("tryCatch/contracts/structuredAndLowLevel.sol")
    # f(bool): true -> 1, 2, 96, 7, "success"
    r = harness.call(app, "f(bool)", True)
    # TODO: verify expected: 1 | 2 | 96 | 7 | "success"
    assert not r.reverted
    # f(bool): false -> 99, 0, 96, 82, "message longer than 32 bytes 32 ", "bytes 32 bytes 32 bytes 32 bytes", " 32 bytes 32 bytes"
    r = harness.call(app, "f(bool)", False)
    # TODO: verify expected: 99 | 0 | 96 | 82 | "message longer than 32 bytes 32 " | "bytes 32 bytes 32 bytes 32 bytes" | " 32 bytes 32 bytes"
    assert not r.reverted

def test_try_catch_library_call(harness):
    """tryCatch/contracts/try_catch_library_call.sol"""
    app = harness.compile_and_deploy("tryCatch/contracts/try_catch_library_call.sol")
    # f(bool): true -> 8, 0x40, 0
    r = harness.call(app, "f(bool)", True)
    assert tuple(as_int(x) for x in r.abi_return) == (8, 64, 0)
    # f(bool): false -> 18, 0x40, 7, "failure"
    r = harness.call(app, "f(bool)", False)
    # TODO: verify expected: 18 | 0x40 | 7 | "failure"
    assert not r.reverted
    # g(bool): true -> 9, 0x40, 0
    r = harness.call(app, "g(bool)", True)
    assert tuple(as_int(x) for x in r.abi_return) == (9, 64, 0)
    # g(bool): false -> 19, 0x40, 7, "failure"
    r = harness.call(app, "g(bool)", False)
    # TODO: verify expected: 19 | 0x40 | 7 | "failure"
    assert not r.reverted
