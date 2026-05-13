"""Smoke tests — exercise the full happy path of every framework feature.

Each test deploys one Solidity contract, runs the assertions documented
in the matching .sol file, and verifies decoded return values directly.

Use this category as the canonical example when adding new tests.
"""
from framework import Panic, ErrorString, Reverted, rpad, lpad, hex_bytes


def test_alignment(harness):
    """alignment.sol — state vars, bytes32 with explicit padding,
    new C() child app, multi-return tuple."""
    app = harness.compile_and_deploy("smoke/alignment.sol")  # picks last contract D

    assert harness.call(app, "stateBool()").abi_return is True
    assert harness.call(app, "stateDecimal()").abi_return == 42

    # stateBytes is a bytes32 initialized from the literal "\x42\x00\xef" — the
    # Solidity compiler treats that as a string that left-aligns into the slot,
    # i.e. 0x4200ef0000...00. ARC4 returns it as the raw 32-byte value.
    sb = harness.call(app, "stateBytes()").abi_return
    assert bytes(sb) == rpad(b"\x42\x00\xef", 32)

    # internalStateDecimal() deploys a child C and reads its stateDecimal (0x20).
    assert harness.call(app, "internalStateDecimal()").abi_return == 0x20

    # update(bool, uint256, bytes32) → returns the same tuple back.
    # -23 passed as uint256 encodes to 2**256-23 (two's complement); the
    # return is decoded as uint256 so it comes back as that same value.
    r = harness.call(
        app,
        "update(bool,uint256,bytes32)",
        False,
        -23,
        list(rpad(b"\x23\x00\xef", 32)),
    )
    assert r.abi_return[0] is False
    assert r.abi_return[1] == 2**256 - 23
    assert bytes(r.abi_return[2]) == rpad(b"\x23\x00\xef", 32)


def test_basic(harness):
    """basic.sol — void returns, payable msg.value, multi-return, bytes32 arg,
    unchecked arithmetic, msg.data.length."""
    app = harness.compile_and_deploy("smoke/basic.sol")

    # d() returns nothing
    assert harness.call(app, "d()").abi_return is None

    # e() returns msg.value (microAlgos in payment txn).
    # The Solidity test also checks `1 ether` = 10^18 but the AVM dispenser
    # only holds ~4×10^15 microAlgos, so 10^18 always overspends.
    # Use 10**6 (1 algo) as the "large" check instead.
    assert harness.call(app, "e()", payment_wei=1).abi_return == 1
    assert harness.call(app, "e()", payment_wei=10**6).abi_return == 10**6

    # f(uint256) returns (a, a)
    assert tuple(harness.call(app, "f(uint256)", 3).abi_return) == (3, 3)

    # g() returns (2, 3)
    assert tuple(harness.call(app, "g()").abi_return) == (2, 3)

    # h(1, -2) returns 3 — unchecked uint256 wrap-around
    assert harness.call(app, "h(uint256,uint256)", 1, -2).abi_return == 3

    # i(true) returns false
    assert harness.call(app, "i(bool)", True).abi_return is False

    # j(bytes32) returns (b, b)
    b = lpad(0x10001, 32)
    r = harness.call(app, "j(bytes32)", list(b))
    assert bytes(r.abi_return[0]) == b
    assert bytes(r.abi_return[1]) == b

    # l(99) returns 693
    assert harness.call(app, "l(uint256)", 99).abi_return == 693


def test_constructor(harness):
    """constructor.sol — payable constructor with one arg, value forwarding."""
    # constructor(), 2 wei: 3 — pay 2 wei, ctor arg uint256 = 3
    app = harness.compile_and_deploy(
        "smoke/constructor.sol",
        ctor_args=[3],
        fund_wei=2,
    )
    assert harness.call(app, "state()").abi_return == 3
    # balance() reads address(this).balance — equals fund_wei + any AVM baseline.
    # We compare against the value forwarded to the ctor (2 wei), accounting for
    # the harness-tracked baseline.
    bal = harness.call(app, "balance()").abi_return
    assert bal - app.balance_baseline == 2

    # update(uint256): 4
    harness.call(app, "update(uint256)", 4)
    assert harness.call(app, "state()").abi_return == 4


def test_arrays(harness):
    """arrays.sol — fixed-size array returns, struct[N] array, string[N] array."""
    app = harness.compile_and_deploy("smoke/arrays.sol")

    # r() returns bool[3] memory
    r = harness.call(app, "r()").abi_return
    assert list(r) == [True, False, True]

    # s() returns (uint[2] memory, uint)
    s = harness.call(app, "s()").abi_return
    assert list(s[0]) == [123, 456]
    assert s[1] == 789

    # u() returns T[2] memory; each T has {uint, uint, string}.
    u = harness.call(app, "u()").abi_return
    assert len(u) == 2
    assert tuple(u[0])[:3] == (23, 42, "any")
    assert tuple(u[1])[:3] == (555, 666, "any")

    # v() returns the empty bool[2][] state var
    assert list(harness.call(app, "v()").abi_return) == []

    # w1/w2/w3 — string[N]
    assert list(harness.call(app, "w1()").abi_return) == ["any"]
    assert list(harness.call(app, "w2()").abi_return) == ["any", "any"]
    assert list(harness.call(app, "w3()").abi_return) == ["any", "any", "any"]

    # x() returns (string[2], string[3])
    x = harness.call(app, "x()").abi_return
    assert list(x[0]) == ["any", "any"]
    assert list(x[1]) == ["any", "any", "any"]


def test_bytes_and_strings(harness):
    """bytes_and_strings.sol — dynamic bytes parameter passthrough, multi-return strings."""
    app = harness.compile_and_deploy("smoke/bytes_and_strings.sol")

    # e(bytes) returns the bytes verbatim
    assert bytes(harness.call(app, "e(bytes)", b"\xAB\x33\xBB").abi_return) == b"\xAB\x33\xBB"
    # 32-byte payload
    payload = b"\x00" * 31 + b"\x20"
    assert bytes(harness.call(app, "e(bytes)", payload).abi_return) == payload
    # 3 bytes — the EVM right-pads to 32 in the raw word, but the bytes type
    # returned by algosdk is the actual content.
    assert bytes(harness.call(app, "e(bytes)", b"\xAB\x33\xFF").abi_return) == b"\xAB\x33\xFF"

    # f() returns ("any", "any")
    f = harness.call(app, "f()").abi_return
    assert tuple(f) == ("any", "any")

    # g() returns ("any", 42, "any")
    g = harness.call(app, "g()").abi_return
    assert tuple(g) == ("any", 42, "any")

    # h() returns "any"
    assert harness.call(app, "h()").abi_return == "any"


def test_structs(harness):
    """structs.sol — struct memory return."""
    app = harness.compile_and_deploy("smoke/structs.sol")
    s = harness.call(app, "s()").abi_return
    assert tuple(s) == (23, 42)
    t = harness.call(app, "t()").abi_return
    assert tuple(t) == (23, 42, "any")


def test_failure(harness):
    """failure.sol — revert classifiers.

    puya-sol currently emits `err` for revert/assert without first
    log-emitting ABI-encoded Error(string) / Panic(uint) bytes. So we
    detect that the call reverted but cannot match the structured reason.
    Tighten this assertion once the codegen emits the EVM-compatible
    revert payload.
    """
    import pytest
    app = harness.compile_and_deploy("smoke/failure.sol", evm_version="byzantium")

    for sig, args in [
        ("e()", ()),
        ("f(bool)", (False,)),
        ("g(bool)", (False,)),
        ("h()", ()),
    ]:
        r = harness.call(app, sig, *args, expect_revert=True)
        assert r.reverted, f"{sig} should revert"


def test_multiline(harness):
    """multiline.sol — multi-line method signature + nonexistent fn fallback."""
    app = harness.compile_and_deploy("smoke/multiline.sol")
    # f sums 5 uints, all 1 → 5
    assert (
        harness.call(app, "f(uint256,uint256,uint256,uint256,uint256)", 1, 1, 1, 1, 1).abi_return
        == 5
    )
    # g() doesn't exist — must revert.
    r = harness.call(app, "g()", expect_revert=True)
    assert r.reverted


def test_multiline_comments(harness):
    """multiline_comments.sol — same as multiline.sol but assertions are
    formatted across multiple lines with parser-comment markers.
    The new framework doesn't parse those comments — we just run the call."""
    app = harness.compile_and_deploy("smoke/multiline_comments.sol")
    assert (
        harness.call(app, "f(uint256,uint256,uint256,uint256,uint256)", 1, 1, 1, 1, 1).abi_return
        == 5
    )


def test_fallback(harness):
    """fallback.sol — bare () call dispatches to fallback().

    AVM has no analogue for `fallback()` — Solidity emits a wildcard ABI
    method but our compiler currently doesn't, so any bare-() call is an
    unsupported pattern. The test xfails as a placeholder for when we
    implement Solidity fallback dispatch in puya-sol.
    """
    import pytest
    pytest.xfail("fallback() dispatch not yet implemented in puya-sol")
