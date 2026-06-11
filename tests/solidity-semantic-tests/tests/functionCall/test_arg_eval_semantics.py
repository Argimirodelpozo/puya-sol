"""CUSTOM regression guards (NOT vendored) for function-call argument semantics."""
from framework import as_int

def test_arg_eval_once_and_order(harness):
    """functionCall/contracts/arg_eval_semantics.sol

    Call arguments must each evaluate exactly once, left-to-right, for internal,
    nested, and external (this.) calls.
    """
    app = harness.compile_and_deploy("functionCall/contracts/arg_eval_semantics.sol")
    assert tuple(as_int(x) for x in harness.call(app, "argOnce()").abi_return) == (30, 2)
    assert tuple(as_int(x) for x in harness.call(app, "nested()").abi_return) == (10, 1)
    assert tuple(as_int(x) for x in harness.call(app, "viaThis()").abi_return) == (30, 2)
    assert as_int(harness.call(app, "order()").abi_return) == 12


def test_extcall_bytes_arg_once(harness):
    """functionCall/contracts/extcall_bytes_arg_side_effect.sol

    CUSTOM regression guard (NOT vendored). A side-effecting dynamic-bytes
    argument to an external call — `callee.take(mkBytes())` — must evaluate
    once. encodeArgToBytes referenced the arg in both the ARC4 length header
    and the concat body (was 2x); fixed with makeEvalOnce.
    """
    app = harness.compile_and_deploy("functionCall/contracts/extcall_bytes_arg_side_effect.sol")
    assert tuple(as_int(x) for x in harness.call(app, "extOnce()").abi_return) == (3, 1)
