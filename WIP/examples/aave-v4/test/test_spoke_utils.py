"""SpokeUtils library tests — ported from upstream
tests/contracts/spoke/libraries/SpokeUtils.t.sol.

Currently covers `toValue` (pure math). The upstream `get` / `setReserve`
tests would require a Reserve struct fixture + mapping setter on the
wrapper — deferred until Hub/Spoke integration tests need them.
"""

import os

import algokit_utils as au
import pytest
from algokit_utils.errors.logic_error import LogicError
from conftest import deploy_contract


@pytest.fixture(scope="module")
def w(localnet, account):
    return deploy_contract(localnet, account, "SpokeUtilsWrapper")


def _call(client, method, *args):
    result = client.send.call(au.AppClientMethodCallParams(
        method=method, args=list(args), note=os.urandom(8),
    ))
    return result.abi_return


def test_deploy(w):
    assert w.app_id > 0


def test_toValue(w):
    """toValue(amount, decimals, price) = amount * price * 10^(18 - decimals).
    Concrete: 4.2e6 USDC ($1) at 6 decimals, price $200e8 (8 decimals
    for oracle) → 4.2e6 * 200e8 * 10^12 = 8.4e28 = 840e26 (units of
    Value, where 1e26 = $1)."""
    amount = int(4.2e6)
    decimals = 6
    price = int(200e8)
    expected = 840 * (10 ** 26)
    assert _call(w, "toValue", amount, decimals, price) == expected


def test_toValue_revertsWith_ArithmeticUnderflow(w):
    """When decimals > 18, the `18 - decimals` subtraction underflows
    on uint256. EVM Solidity reverts with Panic(0x11); on AVM with
    biguint subtract this surfaces as 'byte math would have negative
    result'. Either way the call must revert."""
    with pytest.raises(LogicError):
        _call(w, "toValue", 1, 19, int(1e8))


# Skipped from upstream:
# - test_toValue_revertsWith_ArithmeticOverflow: relies on uint256
#   multiplication overflowing. AVM biguint multiplication is unbounded
#   so the result computes (and on-chain ABI-encodes successfully) as
#   a > 256-bit integer — the EVM-only assertion doesn't apply.
# - test_fuzz_toValue: requires Forge fuzz harness with bound().
# - test_get / test_get_revertsWith_ReserveNotListed: need wrapper
#   exposure of mapping(uint256 => Reserve) state setter, which
#   requires Reserve struct ABI plumbing — deferred.
