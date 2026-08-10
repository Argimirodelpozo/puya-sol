"""Tests for the constructor category."""
import pytest

from framework import (
    Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted,
    as_int, as_bytes,
)


def test_arrays_in_constructors(harness):
    """constructor/contracts/arrays_in_constructors.sol

    `new Main(s, x)` with a dynamic-array ctor arg: the child's ctor writes a
    box-stored array, so the child defers ALL init to __postInit; the caller
    must make the SAME decision (computeNeedsPostInit — was a drifted local
    msg.*-only copy) and ARC4-encode the aggregate arg. The future child's box
    ref is auto-discovered by the harness's populate-on-retry (deterministic
    app ids between simulate and submit). extra_fee: create+fund+pay+postInit
    + two getter calls ≈ 7 txns of min-fee, above the default 5x headroom.
    """
    app = harness.compile_and_deploy('constructor/contracts/arrays_in_constructors.sol')
    r = harness.call(app, 'f(uint256,address[])', 7, 0x40, 10, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10,
                     extra_fee=20000)
    assert as_int(r.abi_return[0]) == 7
    # ch = m_s[7] = address(8): AVM account form = 32-byte BE of 8 (algosdk
    # returns the base32 address string; decode to the public key).
    from algosdk.encoding import decode_address
    assert int.from_bytes(decode_address(r.abi_return[1]), 'big') == 8

def test_base_constructor_arguments(harness):
    """constructor/contracts/base_constructor_arguments.sol"""
    app = harness.compile_and_deploy("constructor/contracts/base_constructor_arguments.sol")
    # getA() -> 49
    r = harness.call(app, "getA()")
    assert as_int(r.abi_return) == 49

def test_bytes_in_constructors_packer(harness):
    """constructor/contracts/bytes_in_constructors_packer.sol"""
    app = harness.compile_and_deploy('constructor/contracts/bytes_in_constructors_packer.sol')

def test_bytes_in_constructors_unpacker(harness):
    """constructor/contracts/bytes_in_constructors_unpacker.sol"""
    s = b"abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz"
    app = harness.compile_and_deploy(
        "constructor/contracts/bytes_in_constructors_unpacker.sol",
        ctor_args=[7, s],
    )
    assert as_int(harness.call(app, "m_x()").abi_return) == 7
    assert bytes(harness.call(app, "m_s()").abi_return) == s

@pytest.mark.xfail(reason="Yul `create` has no AVM equivalent; now a hard compile error per EVM_DIVERGENCE.md (was a silent stub-to-0)", strict=False)
def test_callvalue_check(harness):
    """constructor/contracts/callvalue_check.sol"""
    app = harness.compile_and_deploy('constructor/contracts/callvalue_check.sol')

def test_constructor_arguments_external(harness):
    """constructor/contracts/constructor_arguments_external.sol"""
    app = harness.compile_and_deploy(
        "constructor/contracts/constructor_arguments_external.sol",
        ctor_args=[b"abc", True],
    )
    assert harness.call(app, "getFlag()").abi_return is True
    assert bytes(harness.call(app, "getName()").abi_return) == b"abc"

def test_constructor_arguments_internal(harness):
    """constructor/contracts/constructor_arguments_internal.sol"""
    app = harness.compile_and_deploy("constructor/contracts/constructor_arguments_internal.sol")
    # getFlag() -> true
    r = harness.call(app, "getFlag()")
    assert bool(as_int(r.abi_return)) is True
    # getName() -> "abc"
    r = harness.call(app, "getName()")
    # TODO: verify expected: "abc"
    assert not r.reverted

def test_constructor_function_argument(harness):
    """constructor/contracts/constructor_function_argument.sol"""
    app = harness.compile_and_deploy("constructor/contracts/constructor_function_argument.sol", ctor_args=[0xfdd67305928fcac8d213d1e47bfa6165cd0b87b946644cd0000000000000000])
    # constructor-only test — deployment succeeding is the assertion

@pytest.mark.xfail(reason="ACCEPTED LIMIT (AVM re-entrancy prohibition): C.f() creates D, whose "
    "ctor calls the fn-ptr `this.sixteen` — an inner app call BACK INTO C while C is still "
    "executing. The AVM forbids re-entering an app already on the call stack ('attempt to "
    "re-enter', protocol-level), by design — unlike EVM, where re-entrancy is allowed (and a "
    "notorious exploit source). No compilation strategy can produce this call shape; a callback "
    "into any OTHER contract works fine.", strict=False)
def test_constructor_function_complex(harness):
    """constructor/contracts/constructor_function_complex.sol"""
    app = harness.compile_and_deploy("constructor/contracts/constructor_function_complex.sol", contract_name="C")
    r = harness.call(app, "f()", extra_fee=20000)
    assert as_int(r.abi_return) == 16

def test_constructor_static_array_argument(harness):
    """constructor/contracts/constructor_static_array_argument.sol"""
    app = harness.compile_and_deploy(
        "constructor/contracts/constructor_static_array_argument.sol",
        ctor_args=[1, [2, 3, 4]],
    )
    # a() -> 1
    r = harness.call(app, "a()")
    assert as_int(r.abi_return) == 1
    # b(uint256): 0 -> 2
    r = harness.call(app, "b(uint256)", 0)
    assert as_int(r.abi_return) == 2
    # b(uint256): 1 -> 3
    r = harness.call(app, "b(uint256)", 1)
    assert as_int(r.abi_return) == 3
    # b(uint256): 2 -> 4
    r = harness.call(app, "b(uint256)", 2)
    assert as_int(r.abi_return) == 4

def test_evm_exceptions_in_constructor_call_fail(harness):
    """constructor/contracts/evm_exceptions_in_constructor_call_fail.sol"""
    app = harness.compile_and_deploy("constructor/contracts/evm_exceptions_in_constructor_call_fail.sol")
    # testIt() ->
    r = harness.call(app, "testIt()")
    # (void return — call succeeding is the assertion)
    # test() -> 2
    r = harness.call(app, "test()")
    assert as_int(r.abi_return) == 2

def test_function_usage_in_constructor_arguments(harness):
    """constructor/contracts/function_usage_in_constructor_arguments.sol"""
    app = harness.compile_and_deploy("constructor/contracts/function_usage_in_constructor_arguments.sol")
    # getA() -> 2
    r = harness.call(app, "getA()")
    assert as_int(r.abi_return) == 2

def test_functions_called_by_constructor(harness):
    """constructor/contracts/functions_called_by_constructor.sol"""
    app = harness.compile_and_deploy("constructor/contracts/functions_called_by_constructor.sol")
    # getName() -> "abc"
    r = harness.call(app, "getName()")
    # TODO: verify expected: "abc"
    assert not r.reverted

def test_functions_called_by_constructor_through_dispatch(harness):
    """constructor/contracts/functions_called_by_constructor_through_dispatch.sol"""
    app = harness.compile_and_deploy("constructor/contracts/functions_called_by_constructor_through_dispatch.sol")
    # getName() -> "def\x00\x00\x00"
    r = harness.call(app, "getName()")
    # TODO: verify expected: "def\x00\x00\x00"
    assert not r.reverted

def test_inline_member_init_inheritence_without_constructor(harness):
    """constructor/contracts/inline_member_init_inheritence_without_constructor.sol"""
    app = harness.compile_and_deploy("constructor/contracts/inline_member_init_inheritence_without_constructor.sol")
    # getBMember() -> 5
    r = harness.call(app, "getBMember()")
    assert as_int(r.abi_return) == 5
    # getDMember() -> 6
    r = harness.call(app, "getDMember()")
    assert as_int(r.abi_return) == 6

def test_no_callvalue_check(harness):
    """constructor/contracts/no_callvalue_check.sol"""
    pytest.xfail("`new C{salt:...}(...)` / Yul `create2(...)` are compile-time hard errors on AVM. "
                 "CREATE2's deterministic address derivation (salt + initcode hash) has no AVM "
                 "equivalent — app IDs are assigned sequentially by the chain, so a salt-derived "
                 "address can't be pre-computed.")
    app = harness.compile_and_deploy("constructor/contracts/no_callvalue_check.sol", fund_wei=1000)
    r = harness.call(app, "f()", payment_wei=100, extra_fee=15000)
    assert bool(as_int(r.abi_return)) is True

def test_order_of_evaluation(harness):
    """constructor/contracts/order_of_evaluation.sol"""
    app = harness.compile_and_deploy("constructor/contracts/order_of_evaluation.sol")
    # g() -> 0x20, 4, 1, 3, 2, 4
    r = harness.call(app, "g()")
    # TODO: verify structural decoding matches expected: 32, 4, 1, 3, 2, 4
    assert not r.reverted

def test_payable_constructor(harness):
    """constructor/contracts/payable_constructor.sol"""
    app = harness.compile_and_deploy("constructor/contracts/payable_constructor.sol", fund_wei=27)
    # constructor-only test — deployment succeeding is the assertion

def test_state_variable_initialization(harness):
    """constructor/contracts/state_variable_initialization.sol"""
    app = harness.compile_and_deploy("constructor/contracts/state_variable_initialization.sol")
    # i() -> 2
    r = harness.call(app, "i()")
    assert as_int(r.abi_return) == 2
    # k() -> 0
    r = harness.call(app, "k()")
    assert as_int(r.abi_return) == 0

def test_store_function_in_constructor(harness):
    """constructor/contracts/store_function_in_constructor.sol"""
    app = harness.compile_and_deploy("constructor/contracts/store_function_in_constructor.sol")
    # use(uint256): 3 -> 6
    r = harness.call(app, "use(uint256)", 3)
    assert as_int(r.abi_return) == 6
    # result_in_constructor() -> 4
    r = harness.call(app, "result_in_constructor()")
    assert as_int(r.abi_return) == 4

def test_store_function_in_constructor_packed(harness):
    """constructor/contracts/store_function_in_constructor_packed.sol"""
    app = harness.compile_and_deploy("constructor/contracts/store_function_in_constructor_packed.sol")
    # use(uint16): 3 -> 0xfff9
    r = harness.call(app, "use(uint16)", 3)
    assert as_int(r.abi_return) == 65529
    # result_in_constructor() -> 0xfffb
    r = harness.call(app, "result_in_constructor()")
    assert as_int(r.abi_return) == 65531
    # other() -> 0x1fff
    r = harness.call(app, "other()")
    assert as_int(r.abi_return) == 8191

def test_store_internal_unused_function_in_constructor(harness):
    """constructor/contracts/store_internal_unused_function_in_constructor.sol"""
    app = harness.compile_and_deploy("constructor/contracts/store_internal_unused_function_in_constructor.sol")
    # t() -> 7
    r = harness.call(app, "t()")
    assert as_int(r.abi_return) == 7

def test_store_internal_unused_library_function_in_constructor(harness):
    """constructor/contracts/store_internal_unused_library_function_in_constructor.sol"""
    app = harness.compile_and_deploy("constructor/contracts/store_internal_unused_library_function_in_constructor.sol")
    # t() -> 7
    r = harness.call(app, "t()")
    assert as_int(r.abi_return) == 7

def test_transient_state_variable_initialization(harness):
    """constructor/contracts/transient_state_variable_initialization.sol"""
    app = harness.compile_and_deploy("constructor/contracts/transient_state_variable_initialization.sol")
    # f() -> 100
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 100


def test_new_child_value_funds_once(harness):
    """constructor/contracts/new_child_value_probe.sol

    CUSTOM regression guard (NOT vendored). `new Child{value: N}()` where the
    child ctor reads msg.value (postInit route) must transfer N exactly once.
    Before the fix the fund pay bundled MBR+N AND the [pay(N), __postInit]
    group paid N again — the child's balance was MBR + 2N (verified 2_000_000
    for N=500_000). msg.value inside the ctor reads the postInit-group pay.
    AVM expectation: balance = MBR(1_000_000) + N, seenValue = N.
    """
    app = harness.compile_and_deploy(
        "constructor/contracts/new_child_value_probe.sol", fund_wei=8_000_000)
    r = harness.call(app, "deploy()", extra_fee=10_000).abi_return
    assert as_int(r[0]) == 500_000, f"child saw msg.value {as_int(r[0])}"
    # Base grant is mode-dependent: 1 ALGO default, 4 ALGO slot mode (children
    # pay box MBR there). Either way the VALUE arrives exactly once on top.
    assert as_int(r[1]) in (1_500_000, 4_500_000), \
        f"child balance {as_int(r[1])} != base grant + value"
