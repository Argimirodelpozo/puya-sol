"""EVM-profile fallback/receive dispatch audit.

Expectations pinned from solc's LEGACY dispatcher
(ContractCompiler::appendFunctionSelector, read 2026-09-01):
  - calldatasize < 4 routes to FALLBACK; receive only for calldatasize == 0;
  - unmatched 4-byte selector routes to fallback (never receive);
  - no fallback and no receive: unmatched calldata reverts, empty returndata;
  - fallback(bytes)->bytes: the returned bytes ARE the returndata verbatim
    (RETURN(ptr+0x20, mload(ptr)) — no ABI encoding, no length prefix).

The ARC4-profile arm of this matrix lives in tests/fallback/* (ported solc
semantic tests); this file guards the --contract-abi evm transport, where the
carrier is [selector, body] app args and returndata is the 0x151f7c75 log.
"""
from Crypto.Hash import keccak
from eth_abi import decode, encode


SOURCE = "puyasolRegression/contracts/evm_fallback_matrix.sol"
RETURN_PREFIX = bytes.fromhex("151f7c75")


def _selector(signature: str) -> bytes:
    h = keccak.new(digest_bits=256)
    h.update(signature.encode())
    return h.digest()[:4]


def _call(harness, app, signature, types, values, **kw):
    body = encode(types, values)
    kw.setdefault("extra_fee", 30_000)
    result = harness.call_raw(
        app, _selector(signature), extra_args=(body,), **kw)
    assert not result.reverted, result.fail_message
    assert result.logs and result.logs[-1].startswith(RETURN_PREFIX)
    return result.logs[-1][len(RETURN_PREFIX):]


def _evm_addr(app):
    return "0x" + (bytes(12) + app.app_id.to_bytes(8, "big")).hex()


def _compile(harness):
    return harness.compile(SOURCE, extra_args=["--contract-abi", "evm"])


def test_returns_form_fallback_returndata_is_raw(harness):
    """fallback(bytes)->bytes: caller's ret == the fallback's bytes VERBATIM.

    An ARC4 length header leaking into the carrier would add 2 bytes."""
    artifacts = _compile(harness)
    echo = harness.deploy(artifacts, "EchoFallback")
    caller = harness.deploy(artifacts, "FallbackCaller")
    payload = bytes.fromhex("deadbeef0102030405")
    out = _call(harness, caller, "callEcho(address)",
                ["address"], [_evm_addr(echo)])
    ok, ret = decode(["bool", "bytes"], out)
    assert ok is True
    assert ret == b"\xaa" + payload, ret.hex()


def test_short_calldata_routes_to_fallback_with_exact_msgdata(harness):
    """1-3 byte calldata -> fallback (not receive), msg.data preserved."""
    artifacts = _compile(harness)
    target = harness.deploy(artifacts, "ReceiveAndFallback")
    caller = harness.deploy(artifacts, "FallbackCaller")
    out = _call(harness, caller, "callShort(address)",
                ["address"], [_evm_addr(target)])
    ok, _ = decode(["bool", "bytes"], out)
    assert ok is True
    marker = decode(["uint256"], _call(harness, target, "marker()", [], []))[0]
    assert marker == 2, "short calldata must select fallback, not receive"
    seen = decode(["bytes"], _call(harness, target, "seen()", [], []))[0]
    assert seen == b"\xbe\xef", seen.hex()


def test_empty_calldata_routes_to_receive(harness):
    artifacts = _compile(harness)
    target = harness.deploy(artifacts, "ReceiveAndFallback")
    caller = harness.deploy(artifacts, "FallbackCaller")
    out = _call(harness, caller, "callEmpty(address)",
                ["address"], [_evm_addr(target)])
    assert decode(["bool"], out)[0] is True
    marker = decode(["uint256"], _call(harness, target, "marker()", [], []))[0]
    assert marker == 1, "empty calldata must select receive"


def test_unmatched_selector_routes_to_fallback_not_receive(harness):
    artifacts = _compile(harness)
    target = harness.deploy(artifacts, "ReceiveAndFallback")
    caller = harness.deploy(artifacts, "FallbackCaller")
    junk = bytes.fromhex("deadbeef")
    out = _call(harness, caller, "callUnmatched(address)",
                ["address"], [_evm_addr(target)])
    ok, _ = decode(["bool", "bytes"], out)
    assert ok is True
    marker = decode(["uint256"], _call(harness, target, "marker()", [], []))[0]
    assert marker == 2
    seen = decode(["bytes"], _call(harness, target, "seen()", [], []))[0]
    assert seen == junk


def test_unmatched_selector_without_fallback_aborts_outer(harness):
    """Solc: the callee reverts and the caller observes ok=False.

    AVM adaptation (LowLevelCallOutcome, documented): a rejected inner
    transaction aborts the OUTER call — false is not catchable. The
    observable is therefore the caller's own call failing."""
    artifacts = _compile(harness)
    target = harness.deploy(artifacts, "NoFallback")
    caller = harness.deploy(artifacts, "FallbackCaller")
    result = harness.call_raw(
        caller, _selector("callUnmatched(address)"),
        extra_args=(encode(["address"], [_evm_addr(target)]),),
        extra_fee=30_000, expect_revert=True)
    assert result.reverted, \
        "callee without fallback must reject; outer call aborts with it"
