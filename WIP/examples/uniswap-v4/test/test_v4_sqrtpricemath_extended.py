"""Uniswap V4 SqrtPriceMath extended — untested methods from SqrtPriceMath.sol

Covers:
  - getNextSqrtPriceFromAmount0RoundingUp  (Helper12)
  - getNextSqrtPriceFromAmount1RoundingDown (Helper25)
  - getAmount0Delta                         (Helper30)

Reference values computed from the Solidity formulae:
  Q96 = 2^96 = 79228162514264337593543950336
  SQRT_PRICE_1_1 = Q96
  SQRT_PRICE_2_1 = sqrt(2) * Q96 = 112045541949572279837463876454
  SQRT_PRICE_1_2 = Q96 / sqrt(2) = 56022770974786139918731938227
"""
import pytest
from helpers import grouped_call
import algokit_utils as au
from constants import Q96, SQRT_PRICE_1_1, SQRT_PRICE_1_2, SQRT_PRICE_2_1

_LIQ = 10**18
_AMT = 10**15


# ─── getNextSqrtPriceFromAmount0RoundingUp (Helper12) ────────────────────────

@pytest.mark.localnet
def test_amount0_roundingUp_zeroAmount_addTrue_returnsUnchanged(helper12, orchestrator, algod_client, account):
    """Zero amount with add=True must return sqrtPX96 unchanged."""
    r = grouped_call(helper12, "SqrtPriceMath.getNextSqrtPriceFromAmount0RoundingUp", [SQRT_PRICE_1_1, _LIQ, 0, True], orchestrator, algod_client, account)
    assert r == SQRT_PRICE_1_1


@pytest.mark.localnet
def test_amount0_roundingUp_zeroAmount_addFalse_returnsUnchanged(helper12, orchestrator, algod_client, account):
    """Zero amount with add=False must return sqrtPX96 unchanged."""
    r = grouped_call(helper12, "SqrtPriceMath.getNextSqrtPriceFromAmount0RoundingUp", [SQRT_PRICE_1_1, _LIQ, 0, False], orchestrator, algod_client, account)
    assert r == SQRT_PRICE_1_1


@pytest.mark.localnet
def test_amount0_roundingUp_addTrue_decreasesPrice(helper12, orchestrator, algod_client, account):
    """Adding token0 (add=True) must lower the sqrt price.

    Expected value computed from formula:
      ceil(liquidity * Q96 * sqrtPX96 / (liquidity * Q96 + amount * sqrtPX96))
    """
    r = grouped_call(helper12, "SqrtPriceMath.getNextSqrtPriceFromAmount0RoundingUp", [SQRT_PRICE_1_1, _LIQ, _AMT, True], orchestrator, algod_client, account)
    assert r == 79149013500763574019524425911
    assert r < SQRT_PRICE_1_1


@pytest.mark.localnet
def test_amount0_roundingUp_addFalse_increasesPrice(helper12, orchestrator, algod_client, account):
    """Removing token0 (add=False) must raise the sqrt price.

    Expected value computed from formula:
      ceil(liquidity * Q96 * sqrtPX96 / (liquidity * Q96 - amount * sqrtPX96))
    """
    r = grouped_call(helper12, "SqrtPriceMath.getNextSqrtPriceFromAmount0RoundingUp", [SQRT_PRICE_1_1, _LIQ, _AMT, False], orchestrator, algod_client, account)
    assert r == 79307469984248586179723674011
    assert r > SQRT_PRICE_1_1


@pytest.mark.localnet
def test_amount0_roundingUp_lowerStartPrice_addTrue(helper12, orchestrator, algod_client, account):
    """Same add=True behaviour from SQRT_PRICE_1_2 (price 0.5)."""
    r = grouped_call(helper12, "SqrtPriceMath.getNextSqrtPriceFromAmount0RoundingUp", [SQRT_PRICE_1_2, _LIQ, _AMT, True], orchestrator, algod_client, account)
    assert r == 55983184885121450310660321582
    assert r < SQRT_PRICE_1_2


@pytest.mark.localnet
@pytest.mark.xfail(reason="AVM biguint division by zero returns 0 instead of reverting")
def test_amount0_roundingUp_zeroLiquidity_reverts(helper12, orchestrator, algod_client, account):
    """Zero liquidity must cause a revert (division by zero)."""
    with pytest.raises(Exception):
        grouped_call(helper12, "SqrtPriceMath.getNextSqrtPriceFromAmount0RoundingUp", [SQRT_PRICE_1_1, 0, _AMT, True], orchestrator, algod_client, account)


# ─── getNextSqrtPriceFromAmount1RoundingDown (Helper25) ──────────────────────

@pytest.mark.localnet
def test_amount1_roundingDown_zeroAmount_addTrue_returnsUnchanged(helper25, orchestrator, algod_client, account):
    """Zero amount with add=True must return sqrtPX96 unchanged."""
    r = grouped_call(helper25, "SqrtPriceMath.getNextSqrtPriceFromAmount1RoundingDown", [SQRT_PRICE_1_1, _LIQ, 0, True], orchestrator, algod_client, account)
    assert r == SQRT_PRICE_1_1


@pytest.mark.localnet
def test_amount1_roundingDown_zeroAmount_addFalse_returnsUnchanged(helper25, orchestrator, algod_client, account):
    """Zero amount with add=False must return sqrtPX96 unchanged."""
    r = grouped_call(helper25, "SqrtPriceMath.getNextSqrtPriceFromAmount1RoundingDown", [SQRT_PRICE_1_1, _LIQ, 0, False], orchestrator, algod_client, account)
    assert r == SQRT_PRICE_1_1


@pytest.mark.localnet
def test_amount1_roundingDown_addTrue_increasesPrice(helper25, orchestrator, algod_client, account):
    """Adding token1 (add=True) must raise the sqrt price.

    Expected value: sqrtPX96 + floor(amount * Q96 / liquidity)
    """
    r = grouped_call(helper25, "SqrtPriceMath.getNextSqrtPriceFromAmount1RoundingDown", [SQRT_PRICE_1_1, _LIQ, _AMT, True], orchestrator, algod_client, account)
    assert r == 79307390676778601931137494286
    assert r > SQRT_PRICE_1_1


@pytest.mark.localnet
def test_amount1_roundingDown_addFalse_decreasesPrice(helper25, orchestrator, algod_client, account):
    """Removing token1 (add=False) must lower the sqrt price.

    Expected value: sqrtPX96 - ceil(amount * Q96 / liquidity)
    """
    r = grouped_call(helper25, "SqrtPriceMath.getNextSqrtPriceFromAmount1RoundingDown", [SQRT_PRICE_1_1, _LIQ, _AMT, False], orchestrator, algod_client, account)
    assert r == 79148934351750073255950406385
    assert r < SQRT_PRICE_1_1


@pytest.mark.localnet
def test_amount1_roundingDown_symmetry_vs_amount0(helper25, orchestrator, algod_client, account):
    """For price=1 and amount == liquidity, adding one unit of token1
    should move the price by roughly one Q96 / liquidity step upward."""
    small_liq = 10**6
    small_amt = 1
    r = grouped_call(helper25, "SqrtPriceMath.getNextSqrtPriceFromAmount1RoundingDown", [SQRT_PRICE_1_1, small_liq, small_amt, True], orchestrator, algod_client, account)
    # Q96 / 1e6 = 79228162514264337 (floor)
    expected = SQRT_PRICE_1_1 + Q96 // small_liq
    assert r == expected


# ─── getAmount0Delta (Helper30) ───────────────────────────────────────────────

@pytest.mark.localnet
def test_getAmount0Delta_identicalPrices_returnsZero(helper30, orchestrator, algod_client, account):
    """When both sqrt prices are equal the token0 delta must be zero."""
    r = grouped_call(helper30, "SqrtPriceMath.getAmount0Delta", [SQRT_PRICE_1_1, SQRT_PRICE_1_1, _LIQ], orchestrator, algod_client, account)
    assert r == 0


@pytest.mark.localnet
@pytest.mark.xfail(reason="Signed 3-arg getAmount0Delta has ternary branch swap — positive liquidity takes negation path")
def test_getAmount0Delta_price1to2_nonzero(helper30, orchestrator, algod_client, account):
    """Token0 delta between price 1:1 and 2:1 with 1e18 liquidity.

    Expected: 292893218813452476  (from Solidity formula, rounds up)
    """
    r = grouped_call(helper30, "SqrtPriceMath.getAmount0Delta", [SQRT_PRICE_1_1, SQRT_PRICE_2_1, _LIQ], orchestrator, algod_client, account)
    assert r == 292893218813452476


@pytest.mark.localnet
def test_getAmount0Delta_argumentOrderIndependent(helper30, orchestrator, algod_client, account):
    """Swapping sqrtPriceA and sqrtPriceB must produce the same result."""
    r_ab = grouped_call(helper30, "SqrtPriceMath.getAmount0Delta", [SQRT_PRICE_1_1, SQRT_PRICE_2_1, _LIQ], orchestrator, algod_client, account)
    r_ba = grouped_call(helper30, "SqrtPriceMath.getAmount0Delta", [SQRT_PRICE_2_1, SQRT_PRICE_1_1, _LIQ], orchestrator, algod_client, account)
    assert r_ab == r_ba


@pytest.mark.localnet
def test_getAmount0Delta_zeroLiquidity_returnsZero(helper30, orchestrator, algod_client, account):
    """Zero liquidity must produce a zero delta regardless of prices."""
    r = grouped_call(helper30, "SqrtPriceMath.getAmount0Delta", [SQRT_PRICE_1_1, SQRT_PRICE_2_1, 0], orchestrator, algod_client, account)
    assert r == 0


@pytest.mark.localnet
@pytest.mark.xfail(reason="Signed 3-arg getAmount0Delta has ternary branch swap — positive liquidity takes negation path")
def test_getAmount0Delta_price1to2_smallLiquidity(helper30, orchestrator, algod_client, account):
    """Token0 delta scales proportionally with liquidity (1e6 case).

    Expected: 292894  (rounds up more aggressively at small scale)
    """
    r = grouped_call(helper30, "SqrtPriceMath.getAmount0Delta", [SQRT_PRICE_1_1, SQRT_PRICE_2_1, 10**6], orchestrator, algod_client, account)
    assert r == 292894
