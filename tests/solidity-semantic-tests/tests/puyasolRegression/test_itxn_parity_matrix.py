"""External-call (itxn/) semantics audit vs solc — oracle answers pinned
2026-09-02 on solc 0.8.28 + py-evm:

  thisCallSender()     -> [True, 0]    (this.g() is a real CALL on EVM)
  crossValue()         -> [True, 777]  (callee sees caller contract + value)
  gasOptionSideEffect()-> 1            (solc EVALUATES {gas: expr})
  typedCodeless()      -> reverts      (extcodesize check)

AVM adaptation asserted here where forced: self inner calls are FORBIDDEN
(no reentrancy), so this.g() lowers to a subroutine and the callee keeps the
ORIGINAL msg.sender/msg.value — documented in EVM_DIVERGENCE.md. The other
cells must MATCH the oracle.
"""

SOURCE = "puyasolRegression/contracts/itxn_parity_matrix.sol"


def _deploy(harness):
    artifacts = harness.compile(SOURCE)
    return harness.deploy(artifacts, "ItxnMatrix", fund_wei=2_000_000)


def test_this_call_keeps_original_context_adaptation(harness):
    app = _deploy(harness)
    r = harness.call(app, "thisCallSender()", extra_fee=20_000, payment_wei=777)
    assert not r.reverted, r.fail_message
    sender_is_self, inner_value = list(r.abi_return)
    # EVM: (True, 0). AVM adaptation: subroutine keeps original context.
    assert sender_is_self is False, "this.g() must NOT rewrite msg.sender on AVM"
    assert inner_value == 777, "this.g() keeps the original msg.value on AVM"


def test_cross_contract_value_and_sender(harness):
    app = _deploy(harness)
    r = harness.call(app, "crossValue()", extra_fee=30_000, payment_wei=777)
    assert not r.reverted, r.fail_message
    sender_is_me, v = list(r.abi_return)
    assert sender_is_me is True, "callee must see the caller CONTRACT as sender"
    assert v == 777, "value must forward with the typed call"


def test_gas_option_expression_is_evaluated(harness):
    app = _deploy(harness)
    r = harness.call(app, "gasOptionSideEffect()", extra_fee=30_000)
    assert not r.reverted, r.fail_message
    assert r.abi_return == 1, "solc evaluates {gas: expr}; the side effect must run"


def test_typed_call_to_codeless_address_reverts(harness):
    app = _deploy(harness)
    r = harness.call(app, "typedCodeless()", extra_fee=20_000,
                     expect_revert=True)
    assert r.reverted, "codeless typed call must fail (EVM extcodesize parity)"
