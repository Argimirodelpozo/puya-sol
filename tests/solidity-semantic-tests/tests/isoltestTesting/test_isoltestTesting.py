"""Auto-generated tests for the isoltestTesting category.

Each test deploys the contract defined in the matching .sol file and
runs the assertions originally documented in the test's `// ----`
block. The .sol files are unchanged; this Python module is the new
source of truth — edit it freely to fix or sharpen assertions.
"""
import pytest

from framework import Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted


def test_account(harness):
    """isoltestTesting/account.sol"""
    app = harness.compile_and_deploy("isoltestTesting/account.sol")
    # who_am_i() -> 0x1212121212121212121212121212120000000012
    r = harness.call(app, "who_am_i()")
    assert r.abi_return == 103164821458651970696730694074090566015747358738
    # who_am_i() -> 0x1212121212121212121212121212120000001012
    r = harness.call(app, "who_am_i()")
    assert r.abi_return == 103164821458651970696730694074090566015747362834
    # who_am_i() -> 0x1212121212121212121212121212120000002012
    r = harness.call(app, "who_am_i()")
    assert r.abi_return == 103164821458651970696730694074090566015747366930
    # who_am_i() -> 0x1212121212121212121212121212120000003012
    r = harness.call(app, "who_am_i()")
    assert r.abi_return == 103164821458651970696730694074090566015747371026
    # who_am_i() -> 0x1212121212121212121212121212120000004012
    r = harness.call(app, "who_am_i()")
    assert r.abi_return == 103164821458651970696730694074090566015747375122
    # who_am_i() -> 0x1212121212121212121212121212120000005012
    r = harness.call(app, "who_am_i()")
    assert r.abi_return == 103164821458651970696730694074090566015747379218
    # who_am_i() -> 0x1212121212121212121212121212120000006012
    r = harness.call(app, "who_am_i()")
    assert r.abi_return == 103164821458651970696730694074090566015747383314
    # who_am_i() -> 0x1212121212121212121212121212120000007012
    r = harness.call(app, "who_am_i()")
    assert r.abi_return == 103164821458651970696730694074090566015747387410
    # who_am_i() -> 0x1212121212121212121212121212120000008012
    r = harness.call(app, "who_am_i()")
    assert r.abi_return == 103164821458651970696730694074090566015747391506
    # who_am_i() -> 0x1212121212121212121212121212120000009012
    r = harness.call(app, "who_am_i()")
    assert r.abi_return == 103164821458651970696730694074090566015747395602
    # who_am_i() -> 0x121212121212121212121212121212000000a012
    r = harness.call(app, "who_am_i()")
    assert r.abi_return == 103164821458651970696730694074090566015747399698
    # who_am_i() -> 0x121212121212121212121212121212000000b012
    r = harness.call(app, "who_am_i()")
    assert r.abi_return == 103164821458651970696730694074090566015747403794
    # who_am_i() -> 0x121212121212121212121212121212000000c012
    r = harness.call(app, "who_am_i()")
    assert r.abi_return == 103164821458651970696730694074090566015747407890

def test_balance_other_contract(harness):
    """isoltestTesting/balance_other_contract.sol"""
    app = harness.compile_and_deploy("isoltestTesting/balance_other_contract.sol", fund_wei=2000)
    # constructor-only test — deployment succeeding is the assertion

def test_balance_with_balance(harness):
    """isoltestTesting/balance_with_balance.sol"""
    app = harness.compile_and_deploy("isoltestTesting/balance_with_balance.sol", fund_wei=1000)
    # constructor-only test — deployment succeeding is the assertion

def test_balance_with_balance2(harness):
    """isoltestTesting/balance_with_balance2.sol"""
    app = harness.compile_and_deploy("isoltestTesting/balance_with_balance2.sol", fund_wei=1000000000000000000)
    # constructor-only test — deployment succeeding is the assertion

def test_balance_without_balance(harness):
    """isoltestTesting/balance_without_balance.sol"""
    app = harness.compile_and_deploy("isoltestTesting/balance_without_balance.sol")
    pytest.xfail("isoltest-bareword test — no AVM analogue")

def test_builtins(harness):
    """isoltestTesting/builtins.sol"""
    app = harness.compile_and_deploy("isoltestTesting/builtins.sol")
    pytest.xfail("isoltest-bareword test — no AVM analogue")

def test_effects(harness):
    """isoltestTesting/effects.sol"""
    app = harness.compile_and_deploy("isoltestTesting/effects.sol")
    pytest.xfail("isoltest-bareword test — no AVM analogue")

def test_empty_contract(harness):
    """isoltestTesting/empty_contract.sol"""
    app = harness.compile_and_deploy("isoltestTesting/empty_contract.sol")
    # i_am_not_there() -> FAILURE
    r = harness.call(app, "i_am_not_there()", expect_revert=True)
    assert r.reverted

def test_format_raw_string_with_control_chars(harness):
    """isoltestTesting/format_raw_string_with_control_chars.sol"""
    app = harness.compile_and_deploy("isoltestTesting/format_raw_string_with_control_chars.sol")
    # f(string): 0x20, 16, "\xf0\x9f\x98\x83\xf0\x9f\x98\x83\xf0\x9f\x98\x83\xf0\x9f\x98\x83" -> 0x20, 16, "\xf0\x9f\x98\x83\xf0\x9f\x98\x83\xf0\x9f\x98\x83\xf0\x9f\x98\x83" # Input/Output: "😃😃😃😃" #
    r = harness.call(app, "f(string)", 32, 16, bytes.fromhex('f09f9883f09f9883f09f9883f09f9883'))
    # TODO: verify expected: 0x20 | 16 | "\xf0\x9f\x98\x83\xf0\x9f\x98\x83\xf0\x9f\x98\x83\xf0\x9f\x98\x83" # Input/Output: "😃😃😃😃" #
    assert not r.reverted

def test_isoltestFormatting(harness):
    """isoltestTesting/isoltestFormatting.sol"""
    app = harness.compile_and_deploy("isoltestTesting/isoltestFormatting.sol")
    # f() -> 4, 11, 0x0111, 0x333333, 2222222222222222222
    r = harness.call(app, "f()")
    # TODO: verify structural decoding matches expected: 4, 11, 273, 3355443, 2222222222222222222
    assert not r.reverted
    # g() -> 0x10, 0x0100, 0x0101, 0x333333, 2222222222222222222
    r = harness.call(app, "g()")
    # TODO: verify structural decoding matches expected: 16, 256, 257, 3355443, 2222222222222222222
    assert not r.reverted

def test_precompiles_ignoring_trailing_input(harness):
    """isoltestTesting/precompiles_ignoring_trailing_input.sol"""
    app = harness.compile_and_deploy("isoltestTesting/precompiles_ignoring_trailing_input.sol")
    # ecRecover(uint256[4]): 0x18c547e4f7b0f325ad1e56f57e26c745b09a3e503d86e00e5255ff7f715d3d1c, 0x000000000000000000000000000000000000000000000000000000000000001c, 0x73b1693892219d736caba55bdb67216e485557ea6b6af75f37096c9aa6a5a75f, 0xeeb940b1d03b21e36b0e47e79769f095fe2ab855bd91e3a38756b7d75a9c4549                   -> 0x20, 0x20, 966588469268559010541288244128342317224451555083
    r = harness.call(app, "ecRecover(uint256[4])", 0x18c547e4f7b0f325ad1e56f57e26c745b09a3e503d86e00e5255ff7f715d3d1c, 28, 0x73b1693892219d736caba55bdb67216e485557ea6b6af75f37096c9aa6a5a75f, 0xeeb940b1d03b21e36b0e47e79769f095fe2ab855bd91e3a38756b7d75a9c4549)
    assert tuple(r.abi_return) == (32, 32, 966588469268559010541288244128342317224451555083)
    # ecRecover(uint256[4]): 0x18c547e4f7b0f325ad1e56f57e26c745b09a3e503d86e00e5255ff7f715d3d1c, 0x000000000000000000000000000000000000000000000000000000000000001c, 0x73b1693892219d736caba55bdb67216e485557ea6b6af75f37096c9aa6a5a75f, 0xeeb940b1d03b21e36b0e47e79769f095fe2ab855bd91e3a38756b7d75a9c4549, 0x01, 0x02, 0x03 -> 0x20, 0x20, 966588469268559010541288244128342317224451555083
    r = harness.call(app, "ecRecover(uint256[4])", 0x18c547e4f7b0f325ad1e56f57e26c745b09a3e503d86e00e5255ff7f715d3d1c, 28, 0x73b1693892219d736caba55bdb67216e485557ea6b6af75f37096c9aa6a5a75f, 0xeeb940b1d03b21e36b0e47e79769f095fe2ab855bd91e3a38756b7d75a9c4549, 1, 2, 3)
    assert tuple(r.abi_return) == (32, 32, 966588469268559010541288244128342317224451555083)
    # ecAdd(uint256[4]): 0x01, 0x02, 0x01, 0x02                   -> 0x20, 0x40, 1368015179489954701390400359078579693043519447331113978918064868415326638035, 9918110051302171585080402603319702774565515993150576347155970296011118125764
    r = harness.call(app, "ecAdd(uint256[4])", 1, 2, 1, 2)
    assert tuple(r.abi_return) == (32, 64, 1368015179489954701390400359078579693043519447331113978918064868415326638035, 9918110051302171585080402603319702774565515993150576347155970296011118125764)
    # ecAdd(uint256[4]): 0x01, 0x02, 0x01, 0x02, 0x01             -> 0x20, 0x40, 1368015179489954701390400359078579693043519447331113978918064868415326638035, 9918110051302171585080402603319702774565515993150576347155970296011118125764
    r = harness.call(app, "ecAdd(uint256[4])", 1, 2, 1, 2, 1)
    assert tuple(r.abi_return) == (32, 64, 1368015179489954701390400359078579693043519447331113978918064868415326638035, 9918110051302171585080402603319702774565515993150576347155970296011118125764)
    # ecAdd(uint256[4]): 0x01, 0x02, 0x01, 0x02, 0x01, 0x02       -> 0x20, 0x40, 1368015179489954701390400359078579693043519447331113978918064868415326638035, 9918110051302171585080402603319702774565515993150576347155970296011118125764
    r = harness.call(app, "ecAdd(uint256[4])", 1, 2, 1, 2, 1, 2)
    assert tuple(r.abi_return) == (32, 64, 1368015179489954701390400359078579693043519447331113978918064868415326638035, 9918110051302171585080402603319702774565515993150576347155970296011118125764)
    # ecAdd(uint256[4]): 0x01, 0x02, 0x01, 0x02, 0x01, 0x02, 0x03 -> 0x20, 0x40, 1368015179489954701390400359078579693043519447331113978918064868415326638035, 9918110051302171585080402603319702774565515993150576347155970296011118125764
    r = harness.call(app, "ecAdd(uint256[4])", 1, 2, 1, 2, 1, 2, 3)
    assert tuple(r.abi_return) == (32, 64, 1368015179489954701390400359078579693043519447331113978918064868415326638035, 9918110051302171585080402603319702774565515993150576347155970296011118125764)
    # ecMul(uint256[3]): 0x01, 0x02, 0x02                   -> 0x20, 0x40, 1368015179489954701390400359078579693043519447331113978918064868415326638035, 9918110051302171585080402603319702774565515993150576347155970296011118125764
    r = harness.call(app, "ecMul(uint256[3])", 1, 2, 2)
    assert tuple(r.abi_return) == (32, 64, 1368015179489954701390400359078579693043519447331113978918064868415326638035, 9918110051302171585080402603319702774565515993150576347155970296011118125764)
    # ecMul(uint256[3]): 0x01, 0x02, 0x02, 0x01             -> 0x20, 0x40, 1368015179489954701390400359078579693043519447331113978918064868415326638035, 9918110051302171585080402603319702774565515993150576347155970296011118125764
    r = harness.call(app, "ecMul(uint256[3])", 1, 2, 2, 1)
    assert tuple(r.abi_return) == (32, 64, 1368015179489954701390400359078579693043519447331113978918064868415326638035, 9918110051302171585080402603319702774565515993150576347155970296011118125764)
    # ecMul(uint256[3]): 0x01, 0x02, 0x02, 0x01, 0x02       -> 0x20, 0x40, 1368015179489954701390400359078579693043519447331113978918064868415326638035, 9918110051302171585080402603319702774565515993150576347155970296011118125764
    r = harness.call(app, "ecMul(uint256[3])", 1, 2, 2, 1, 2)
    assert tuple(r.abi_return) == (32, 64, 1368015179489954701390400359078579693043519447331113978918064868415326638035, 9918110051302171585080402603319702774565515993150576347155970296011118125764)
    # ecMul(uint256[3]): 0x01, 0x02, 0x02, 0x01, 0x02, 0x03 -> 0x20, 0x40, 1368015179489954701390400359078579693043519447331113978918064868415326638035, 9918110051302171585080402603319702774565515993150576347155970296011118125764
    r = harness.call(app, "ecMul(uint256[3])", 1, 2, 2, 1, 2, 3)
    assert tuple(r.abi_return) == (32, 64, 1368015179489954701390400359078579693043519447331113978918064868415326638035, 9918110051302171585080402603319702774565515993150576347155970296011118125764)
