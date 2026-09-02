"""contract/ semantics audit vs solc — oracle answers pinned 2026-09-02 on
solc 0.8.28 + py-evm:

  DerivedOrder ctor log: [1, 11, 2, 10, 3, 600] — base ctor bodies run
    most-base-first (BaseA(11) before BaseB(10) before Derived), and EVERY
    state initializer has run before the derived body (a+b+d == 600).
  SideEffectArgs ctor log: [8, 7], seen == 23 — LEGACY evaluates the RIGHT
    operand of `bump(7) + bump(8)*2` first (the standing legacy-order
    doctrine; via-IR would differ).
  Getter shapes: mapping-of-struct getter returns ONLY value members
    (x, y) — array/mapping members dropped; fixed array getter is indexed;
    nested mapping getter takes both keys.
"""

SOURCE = "puyasolRegression/contracts/contract_parity_matrix.sol"


def _log(harness, app):
    n = harness.call(app, "logLen()", extra_fee=10_000).abi_return
    return [harness.call(app, "logAt(uint256)", i, extra_fee=10_000).abi_return
            for i in range(n)]


def test_ctor_order_across_inheritance(harness):
    artifacts = harness.compile(SOURCE)
    app = harness.deploy(artifacts, "DerivedOrder")
    assert _log(harness, app) == [1, 11, 2, 10, 3, 600]


def test_ctor_arg_evaluation_is_legacy_order(harness):
    artifacts = harness.compile(SOURCE)
    app = harness.deploy(artifacts, "SideEffectArgs")
    assert _log(harness, app) == [8, 7]
    assert harness.call(app, "seen()", extra_fee=10_000).abi_return == 23


def test_virtual_from_base_ctor_sees_initializers(harness):
    """Oracle: [47] — ALL initializers run before ANY ctor body, so the
    most-derived override called from the base ctor sees v == 42."""
    artifacts = harness.compile(SOURCE)
    app = harness.deploy(artifacts, "VDerived")
    assert _log(harness, app) == [47]
    assert harness.call(app, "f()", extra_fee=10_000).abi_return == 47


def test_getter_shapes(harness):
    artifacts = harness.compile(SOURCE)
    app = harness.deploy(artifacts, "GetterShapes")
    r = harness.call(app, "items(uint256)", 5, extra_fee=10_000)
    assert not r.reverted, r.fail_message
    assert list(r.abi_return) == [41, 42]
    assert harness.call(app, "fixedArr(uint256)", 1,
                        extra_fee=10_000).abi_return == 7
    assert harness.call(app, "nested(uint256,bool)", 9, True,
                        extra_fee=10_000).abi_return == 77
