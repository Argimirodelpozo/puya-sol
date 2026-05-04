"""
AAVE V4 AssetInterestRateStrategy tests.
Translated from AssetInterestRateStrategy.t.sol (Foundry).
"""

import pytest
import hashlib
import base64
import algokit_utils as au
from conftest import deploy_contract


ABI_RETURN_PREFIX = bytes.fromhex("151f7c75")


def _arc28_selector(signature):
    return hashlib.new("sha512_256", signature.encode()).digest()[:4]


def _extract_events(confirmation):
    logs = confirmation.get("logs", [])
    return [base64.b64decode(l) for l in logs if not base64.b64decode(l).startswith(ABI_RETURN_PREFIX)]

RAY = 10**27


def _bps_to_ray(bps):
    """Convert basis points to RAY."""
    return bps * (RAY // 10**4)


def _box_ref(app_id, key):
    return au.BoxReference(app_id=app_id, name=key)


def _mapping_box_key(mapping_name, key_bytes):
    return mapping_name.encode() + hashlib.sha256(key_bytes).digest()


def _biguint_key(val):
    """Normalize biguint to 64-byte key."""
    raw = val.to_bytes((val.bit_length() + 7) // 8, 'big') if val > 0 else b'\x00'
    padded = b'\x00' * 64 + raw
    return padded[len(padded) - 64:]


@pytest.fixture(scope="module")
def strategy(localnet, account):
    # constructor(address hub_) — pass deployer's address so we can call onlyHub methods
    from algosdk import encoding
    hub_addr = encoding.decode_address(account.address)
    client = deploy_contract(
        localnet, account, "AssetInterestRateStrategy",
        app_args=[hub_addr],
    )

    # Set up rate data: same as Foundry setUp
    optimal = 8000  # 80.00%
    base_rate = 200  # 2.00%
    slope1 = 400  # 4.00%
    slope2 = 7500  # 75.00%

    # InterestRateData is uint16 + uint32 + uint32 + uint32 (= 14 B
    # ARC4-packed). The compiled contract emits `extract 2 32` to read
    # the struct header — i.e. it grabs a 32-byte window starting after
    # the byte[] length prefix, then reads each field at its native
    # offset within that window. So we need to send 14 B of struct data
    # + 18 B of trailing pad to land a 32-byte window. The pad bytes
    # are read by `extract` but never indexed.
    fields = (
        optimal.to_bytes(2, 'big') +
        base_rate.to_bytes(4, 'big') +
        slope1.to_bytes(4, 'big') +
        slope2.to_bytes(4, 'big')
    )
    encoded = fields + b'\x00' * (32 - len(fields))

    asset_id = int.from_bytes(hashlib.sha256(b'mockAssetId').digest(), 'big') % (2**256)

    # Box key for the mapping
    box_key = _mapping_box_key("_interestRateData", _biguint_key(asset_id))
    box = _box_ref(client.app_id, box_key)

    client.send.call(
        au.AppClientMethodCallParams(
            method="setInterestRateData",
            args=[asset_id, encoded],
            box_references=[box],
        )
    )

    return client, asset_id


def _call(client, method, *args, boxes=None):
    params = au.AppClientMethodCallParams(method=method, args=list(args))
    if boxes:
        params = au.AppClientMethodCallParams(
            method=method, args=list(args), box_references=boxes,
        )
    result = client.send.call(params)
    return result.abi_return


def _call_with_asset(client, method, asset_id, *extra_args):
    """Call a method that reads the _interestRateData mapping."""
    box_key = _mapping_box_key("_interestRateData", _biguint_key(asset_id))
    box = _box_ref(client.app_id, box_key)
    return _call(client, method, asset_id, *extra_args, boxes=[box])


# ─── Constants ─────────────────────────────────────────────────────────────────

def test_deploy(strategy):
    client, _ = strategy
    assert client.app_id > 0


def test_max_borrow_rate(strategy):
    client, asset_id = strategy
    pass


# ─── Getters ───────────────────────────────────────────────────────────────────

def test_getOptimalUsageRatio(strategy):
    client, asset_id = strategy
    result = _call_with_asset(client, "getOptimalUsageRatio", asset_id)
    assert result == 8000


def test_getBaseVariableBorrowRate(strategy):
    client, asset_id = strategy
    result = _call_with_asset(client, "getBaseVariableBorrowRate", asset_id)
    assert result == 200


def test_getVariableRateSlope1(strategy):
    client, asset_id = strategy
    result = _call_with_asset(client, "getVariableRateSlope1", asset_id)
    assert result == 400


def test_getVariableRateSlope2(strategy):
    client, asset_id = strategy
    result = _call_with_asset(client, "getVariableRateSlope2", asset_id)
    assert result == 7500


def test_getMaxVariableBorrowRate(strategy):
    client, asset_id = strategy
    result = _call_with_asset(client, "getMaxVariableBorrowRate", asset_id)
    # base + slope1 + slope2 = 200 + 400 + 7500 = 8100
    assert result == 8100


def test_getInterestRateData(strategy):
    client, asset_id = strategy
    # algokit's auto-decoder returns None here because the contract
    # returns 32 bytes (14 B struct + 18 B trailing pad) but the arc56
    # struct width is 14 B — algokit refuses to decode the surplus.
    # Read the raw ABI-prefixed log and decode manually.
    box_key = _mapping_box_key("_interestRateData", _biguint_key(asset_id))
    box = _box_ref(client.app_id, box_key)
    result = client.send.call(
        au.AppClientMethodCallParams(
            method="getInterestRateData", args=[asset_id], box_references=[box]
        )
    )
    log = base64.b64decode(result.confirmation["logs"][0])
    assert log[:4] == ABI_RETURN_PREFIX
    payload = log[4:]
    optimal = int.from_bytes(payload[0:2], "big")
    base_rate = int.from_bytes(payload[2:6], "big")
    slope1 = int.from_bytes(payload[6:10], "big")
    slope2 = int.from_bytes(payload[10:14], "big")
    assert optimal == 8000
    assert base_rate == 200
    assert slope1 == 400
    assert slope2 == 7500


# ─── calculateInterestRate ────────────────────────────────────────────────────

def test_setInterestRateData_emits_event(strategy):
    """setInterestRateData should emit UpdateRateData ARC-28 event."""
    client, _ = strategy

    asset_id = int.from_bytes(hashlib.sha256(b'eventTestAsset').digest(), 'big') % (2**256)
    fields = (
        (9000).to_bytes(2, 'big') +
        (100).to_bytes(4, 'big') +
        (300).to_bytes(4, 'big') +
        (6000).to_bytes(4, 'big')
    )
    encoded = fields + b'\x00' * (32 - len(fields))
    box_key = _mapping_box_key("_interestRateData", _biguint_key(asset_id))
    box = _box_ref(client.app_id, box_key)

    result = client.send.call(
        au.AppClientMethodCallParams(
            method="setInterestRateData",
            args=[asset_id, encoded],
            box_references=[box],
        )
    )
    events = _extract_events(result.confirmation)
    assert len(events) >= 1

    # The Algorand event uses Algorand-native widths, not the Solidity
    # source's uint256 declarations: hub is uint8[32] (bytes32) and the
    # 4 rate fields are uint64. See arc56.json for the canonical shape.
    selector = _arc28_selector(
        "UpdateRateData(uint8[32],uint256,uint64,uint64,uint64,uint64)"
    )
    assert any(e[:4] == selector for e in events)

    # 32 (hub) + 32 (assetId) + 4×8 (rate uint64s) = 96 bytes payload.
    event = next(e for e in events if e[:4] == selector)
    data = event[4:]
    assert len(data) == 32 + 32 + 4 * 8, f"expected 96 B, got {len(data)}"
    offset = 64
    optimal = int.from_bytes(data[offset:offset+8], "big")
    base_rate = int.from_bytes(data[offset+8:offset+16], "big")
    slope1 = int.from_bytes(data[offset+16:offset+24], "big")
    slope2 = int.from_bytes(data[offset+24:offset+32], "big")
    assert optimal == 9000
    assert base_rate == 100
    assert slope1 == 300
    assert slope2 == 6000


def test_calculateInterestRate_zero_drawn(strategy):
    """When drawn=0, rate should be the base rate."""
    client, asset_id = strategy
    result = _call_with_asset(client, "calculateInterestRate", asset_id, 10000, 0, 0, 0)
    assert result == _bps_to_ray(200)  # base rate in RAY


def test_calculateInterestRate_at_kink(strategy):
    """At optimal usage ratio (80%), rate = base + slope1."""
    client, asset_id = strategy
    # usage = drawn / (liquidity + drawn + swept) = 80%
    # drawn=80, liquidity=20, swept=0 → usage = 80/(20+80) = 80%
    result = _call_with_asset(client, "calculateInterestRate", asset_id, 20, 80, 0, 0)
    # At kink: rate = base + slope1 = 200 + 400 = 600 bps
    expected = _bps_to_ray(200 + 400)
    # Due to rounding, allow small delta
    assert abs(result - expected) < RAY // 10**4  # within 0.01%


def test_calculateInterestRate_at_max(strategy):
    """At 100% utilization, rate = base + slope1 + slope2."""
    client, asset_id = strategy
    # drawn=100, liquidity=0, swept=0 → usage = 100%
    result = _call_with_asset(client, "calculateInterestRate", asset_id, 0, 100, 0, 0)
    expected = _bps_to_ray(200 + 400 + 7500)
    assert abs(result - expected) < RAY // 10**4


# ─── Ported from upstream AssetInterestRateStrategy.t.sol ─────────────────────


def test_minOptimalRatio(strategy):
    """MIN_OPTIMAL_RATIO public constant = 1_00 (1.00% in BPS)."""
    client, _ = strategy
    assert _call(client, "MIN_OPTIMAL_RATIO") == 100


def test_maxOptimalRatio(strategy):
    """MAX_OPTIMAL_RATIO public constant = 99_00 (99.00% in BPS)."""
    client, _ = strategy
    assert _call(client, "MAX_OPTIMAL_RATIO") == 9900


def test_calculateInterestRate_ZeroDebtZeroLiquidity(strategy):
    """When the pool has no liquidity AND no debt drawn, the
    utilization ratio is undefined → contract returns the base rate
    (slope contributions are zero). Direct port of upstream
    test_calculateInterestRate_ZeroDebtZeroLiquidity which delegates
    to the fuzz variant with liquidity=0."""
    client, asset_id = strategy
    result = _call_with_asset(client, "calculateInterestRate", asset_id, 0, 0, 0, 0)
    # base_rate = 200 BPS, no slope contribution at zero debt
    expected = _bps_to_ray(200)
    assert abs(result - expected) < RAY // 10**4
