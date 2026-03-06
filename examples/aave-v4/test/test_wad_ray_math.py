"""
AAVE V4 WadRayMath library tests.
Translated from WadRayMath.t.sol (Foundry).
Uses exact integer arithmetic (no float-to-int conversions) to avoid precision loss.
"""

import pytest
import algokit_utils as au
from conftest import deploy_contract

UINT256_MAX = 2**256 - 1
WAD = 10**18
RAY = 10**27
PERCENTAGE_FACTOR = 10**4


@pytest.fixture(scope="module")
def wad_ray(localnet, account):
    return deploy_contract(localnet, account, "WadRayMathWrapper", extra_pages=1)


def _call(client, method, *args):
    result = client.send.call(au.AppClientMethodCallParams(method=method, args=list(args)))
    return result.abi_return


# ─── Constants ────────────────────────────────────────────────────────────────

def test_deploy(wad_ray):
    assert wad_ray.app_id > 0


def test_constants_wad_decimals(wad_ray):
    assert _call(wad_ray, "WAD_DECIMALS") == 18


def test_constants_wad(wad_ray):
    assert _call(wad_ray, "WAD") == WAD


def test_constants_ray(wad_ray):
    assert _call(wad_ray, "RAY") == RAY


def test_constants_percentage_factor(wad_ray):
    assert _call(wad_ray, "PERCENTAGE_FACTOR") == PERCENTAGE_FACTOR


# ─── Concrete wadMul tests ───────────────────────────────────────────────────
# Solidity: 2.5e18 = 2500000000000000000, 0.5e18 = 500000000000000000, etc.

def test_wadMulDown_zero_a(wad_ray):
    assert _call(wad_ray, "wadMulDown", 0, WAD) == 0


def test_wadMulDown_zero_b(wad_ray):
    assert _call(wad_ray, "wadMulDown", WAD, 0) == 0


def test_wadMulDown_both_zero(wad_ray):
    assert _call(wad_ray, "wadMulDown", 0, 0) == 0


def test_wadMulDown_basic(wad_ray):
    assert _call(wad_ray, "wadMulDown", 25 * 10**17, 5 * 10**17) == 125 * 10**16
    assert _call(wad_ray, "wadMulDown", 3 * WAD, WAD) == 3 * WAD
    assert _call(wad_ray, "wadMulDown", 369, 271) == 0
    assert _call(wad_ray, "wadMulDown", 4122 * 10**17, WAD) == 4122 * 10**17
    assert _call(wad_ray, "wadMulDown", 6 * WAD, 2 * WAD) == 12 * WAD


def test_wadMulUp_zero(wad_ray):
    assert _call(wad_ray, "wadMulUp", 0, WAD) == 0
    assert _call(wad_ray, "wadMulUp", WAD, 0) == 0
    assert _call(wad_ray, "wadMulUp", 0, 0) == 0


def test_wadMulUp_basic(wad_ray):
    assert _call(wad_ray, "wadMulUp", 25 * 10**17, 5 * 10**17) == 125 * 10**16
    assert _call(wad_ray, "wadMulUp", 3 * WAD, WAD) == 3 * WAD
    assert _call(wad_ray, "wadMulUp", 369, 271) == 1
    assert _call(wad_ray, "wadMulUp", 4122 * 10**17, WAD) == 4122 * 10**17
    assert _call(wad_ray, "wadMulUp", 6 * WAD, 2 * WAD) == 12 * WAD


# ─── Concrete rayMul tests ──────────────────────────────────────────────────

def test_rayMulDown_zero(wad_ray):
    assert _call(wad_ray, "rayMulDown", 0, RAY) == 0
    assert _call(wad_ray, "rayMulDown", RAY, 0) == 0
    assert _call(wad_ray, "rayMulDown", 0, 0) == 0


def test_rayMulDown_basic(wad_ray):
    assert _call(wad_ray, "rayMulDown", 25 * 10**26, 5 * 10**26) == 125 * 10**25
    assert _call(wad_ray, "rayMulDown", 3 * RAY, RAY) == 3 * RAY
    assert _call(wad_ray, "rayMulDown", 369, 271) == 0
    assert _call(wad_ray, "rayMulDown", 4122 * 10**26, RAY) == 4122 * 10**26
    assert _call(wad_ray, "rayMulDown", 6 * RAY, 2 * RAY) == 12 * RAY


def test_rayMulUp_zero(wad_ray):
    assert _call(wad_ray, "rayMulUp", 0, RAY) == 0
    assert _call(wad_ray, "rayMulUp", RAY, 0) == 0
    assert _call(wad_ray, "rayMulUp", 0, 0) == 0


def test_rayMulUp_basic(wad_ray):
    assert _call(wad_ray, "rayMulUp", 25 * 10**26, 5 * 10**26) == 125 * 10**25
    assert _call(wad_ray, "rayMulUp", 3 * RAY, RAY) == 3 * RAY
    assert _call(wad_ray, "rayMulUp", 369, 271) == 1
    assert _call(wad_ray, "rayMulUp", 4122 * 10**26, RAY) == 4122 * 10**26
    assert _call(wad_ray, "rayMulUp", 6 * RAY, 2 * RAY) == 12 * RAY


# ─── Concrete wadDiv tests ──────────────────────────────────────────────────

def test_wadDivDown_zero_num(wad_ray):
    assert _call(wad_ray, "wadDivDown", 0, WAD) == 0


def test_wadDivDown_div_by_zero(wad_ray):
    with pytest.raises(Exception):
        _call(wad_ray, "wadDivDown", WAD, 0)


def test_wadDivDown_both_zero(wad_ray):
    with pytest.raises(Exception):
        _call(wad_ray, "wadDivDown", 0, 0)


def test_wadDivDown_basic(wad_ray):
    assert _call(wad_ray, "wadDivDown", 25 * 10**17, 5 * 10**17) == 5 * WAD
    assert _call(wad_ray, "wadDivDown", 4122 * 10**17, WAD) == 4122 * 10**17
    # 8.745e18 / 0.67e18 = 13.052238805970149253...e18
    assert _call(wad_ray, "wadDivDown", 8745 * 10**15, 67 * 10**16) == 13052238805970149253
    assert _call(wad_ray, "wadDivDown", 6 * WAD, 2 * WAD) == 3 * WAD
    assert _call(wad_ray, "wadDivDown", 125 * 10**16, 5 * 10**17) == 25 * 10**17
    assert _call(wad_ray, "wadDivDown", 3 * WAD, WAD) == 3 * WAD
    assert _call(wad_ray, "wadDivDown", 2, 100000000000000 * WAD) == 0


def test_wadDivUp_zero_num(wad_ray):
    assert _call(wad_ray, "wadDivUp", 0, WAD) == 0


def test_wadDivUp_div_by_zero(wad_ray):
    with pytest.raises(Exception):
        _call(wad_ray, "wadDivUp", WAD, 0)


def test_wadDivUp_both_zero(wad_ray):
    with pytest.raises(Exception):
        _call(wad_ray, "wadDivUp", 0, 0)


def test_wadDivUp_basic(wad_ray):
    assert _call(wad_ray, "wadDivUp", 25 * 10**17, 5 * 10**17) == 5 * WAD
    assert _call(wad_ray, "wadDivUp", 4122 * 10**17, WAD) == 4122 * 10**17
    assert _call(wad_ray, "wadDivUp", 8745 * 10**15, 67 * 10**16) == 13052238805970149254
    assert _call(wad_ray, "wadDivUp", 6 * WAD, 2 * WAD) == 3 * WAD
    assert _call(wad_ray, "wadDivUp", 125 * 10**16, 5 * 10**17) == 25 * 10**17
    assert _call(wad_ray, "wadDivUp", 3 * WAD, WAD) == 3 * WAD
    assert _call(wad_ray, "wadDivUp", 2, 100000000000000 * WAD) == 1


# ─── Concrete rayDiv tests ──────────────────────────────────────────────────

def test_rayDivDown_zero_num(wad_ray):
    assert _call(wad_ray, "rayDivDown", 0, RAY) == 0


def test_rayDivDown_div_by_zero(wad_ray):
    with pytest.raises(Exception):
        _call(wad_ray, "rayDivDown", RAY, 0)


def test_rayDivDown_both_zero(wad_ray):
    with pytest.raises(Exception):
        _call(wad_ray, "rayDivDown", 0, 0)


def test_rayDivDown_basic(wad_ray):
    assert _call(wad_ray, "rayDivDown", 25 * 10**26, 5 * 10**26) == 5 * RAY
    assert _call(wad_ray, "rayDivDown", 4122 * 10**26, RAY) == 4122 * 10**26
    # 8.745e27 / 0.67e27 = 13.052238805970149253731343283...e27
    assert _call(wad_ray, "rayDivDown", 8745 * 10**24, 67 * 10**25) == 13052238805970149253731343283
    assert _call(wad_ray, "rayDivDown", 6 * RAY, 2 * RAY) == 3 * RAY
    assert _call(wad_ray, "rayDivDown", 125 * 10**25, 5 * 10**26) == 25 * 10**26
    assert _call(wad_ray, "rayDivDown", 3 * RAY, RAY) == 3 * RAY
    assert _call(wad_ray, "rayDivDown", 2, 100000000000000 * RAY) == 0


def test_rayDivUp_zero_num(wad_ray):
    assert _call(wad_ray, "rayDivUp", 0, RAY) == 0


def test_rayDivUp_div_by_zero(wad_ray):
    with pytest.raises(Exception):
        _call(wad_ray, "rayDivUp", RAY, 0)


def test_rayDivUp_both_zero(wad_ray):
    with pytest.raises(Exception):
        _call(wad_ray, "rayDivUp", 0, 0)


def test_rayDivUp_basic(wad_ray):
    assert _call(wad_ray, "rayDivUp", 25 * 10**26, 5 * 10**26) == 5 * RAY
    assert _call(wad_ray, "rayDivUp", 4122 * 10**26, RAY) == 4122 * 10**26
    assert _call(wad_ray, "rayDivUp", 8745 * 10**24, 67 * 10**25) == 13052238805970149253731343284
    assert _call(wad_ray, "rayDivUp", 6 * RAY, 2 * RAY) == 3 * RAY
    assert _call(wad_ray, "rayDivUp", 125 * 10**25, 5 * 10**26) == 25 * 10**26
    assert _call(wad_ray, "rayDivUp", 3 * RAY, RAY) == 3 * RAY
    assert _call(wad_ray, "rayDivUp", 2, 100000000000000 * RAY) == 1


# ─── Conversion tests ───────────────────────────────────────────────────────

def test_fromWadDown(wad_ray):
    assert _call(wad_ray, "fromWadDown", 5 * WAD) == 5
    assert _call(wad_ray, "fromWadDown", 0) == 0
    assert _call(wad_ray, "fromWadDown", WAD - 1) == 0
    assert _call(wad_ray, "fromWadDown", WAD) == 1


def test_fromRayUp(wad_ray):
    assert _call(wad_ray, "fromRayUp", 0) == 0
    assert _call(wad_ray, "fromRayUp", RAY) == 1
    assert _call(wad_ray, "fromRayUp", 1) == 1  # ceil(1/RAY) = 1
    assert _call(wad_ray, "fromRayUp", 5 * RAY) == 5


def test_toWad(wad_ray):
    assert _call(wad_ray, "toWad", 5) == 5 * WAD
    assert _call(wad_ray, "toWad", 0) == 0
    assert _call(wad_ray, "toWad", 1) == WAD


def test_toWad_overflow(wad_ray):
    big = UINT256_MAX // WAD + 1
    with pytest.raises(Exception):
        _call(wad_ray, "toWad", big)


def test_toRay(wad_ray):
    assert _call(wad_ray, "toRay", 5) == 5 * RAY
    assert _call(wad_ray, "toRay", 0) == 0
    assert _call(wad_ray, "toRay", 1) == RAY


def test_toRay_overflow(wad_ray):
    big = UINT256_MAX // RAY + 1
    with pytest.raises(Exception):
        _call(wad_ray, "toRay", big)


def test_bpsToWad(wad_ray):
    wad_per_bps = WAD // PERCENTAGE_FACTOR  # 1e14
    assert _call(wad_ray, "bpsToWad", 10000) == 10000 * wad_per_bps
    assert _call(wad_ray, "bpsToWad", 5000) == 5000 * wad_per_bps
    assert _call(wad_ray, "bpsToWad", 0) == 0


def test_bpsToRay(wad_ray):
    ray_per_bps = RAY // PERCENTAGE_FACTOR  # 1e23
    assert _call(wad_ray, "bpsToRay", 10000) == 10000 * ray_per_bps
    assert _call(wad_ray, "bpsToRay", 5000) == 5000 * ray_per_bps
    assert _call(wad_ray, "bpsToRay", 0) == 0


def test_roundRayUp_exact_multiple(wad_ray):
    assert _call(wad_ray, "roundRayUp", 0) == 0
    assert _call(wad_ray, "roundRayUp", RAY) == RAY
    assert _call(wad_ray, "roundRayUp", 5 * RAY) == 5 * RAY


def test_roundRayUp_not_multiple(wad_ray):
    assert _call(wad_ray, "roundRayUp", 1) == RAY
    assert _call(wad_ray, "roundRayUp", RAY + 1) == 2 * RAY
    assert _call(wad_ray, "roundRayUp", 5 * RAY - 1) == 5 * RAY


def test_roundRayUp_overflow(wad_ray):
    max_a = (UINT256_MAX // RAY) * RAY
    assert _call(wad_ray, "roundRayUp", max_a) == max_a
    with pytest.raises(Exception):
        _call(wad_ray, "roundRayUp", max_a + 1)
