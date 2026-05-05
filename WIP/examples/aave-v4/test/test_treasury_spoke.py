"""
AAVE V4 TreasurySpoke tests.
"""

import pytest
import hashlib
import base64
import algokit_utils as au
from algosdk import encoding
from conftest import deploy_contract
from uros_dance import deploy_split_app


ABI_RETURN_PREFIX = bytes.fromhex("151f7c75")


def _arc28_selector(signature):
    """Compute ARC-28 event selector: sha512_256(signature)[:4]."""
    return hashlib.new("sha512_256", signature.encode()).digest()[:4]


def _extract_events(confirmation):
    """Extract non-ABI-return log entries from a transaction confirmation."""
    logs = confirmation.get("logs", [])
    return [base64.b64decode(l) for l in logs if not base64.b64decode(l).startswith(ABI_RETURN_PREFIX)]


ZERO_ADDR = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAY5HFKQ"


def _appid_to_pseudo_addr_bytes(app_id: int) -> bytes:
    """puya-sol app-address convention: \\x00*24 + itob(app_id). Tests
    passing a real app's address as a Solidity `address` arg need this
    format so SolExternalCall::addressToAppId recovers the right id."""
    return b"\x00" * 24 + app_id.to_bytes(8, "big")


@pytest.fixture(scope="module")
def hub(localnet, account, orch_app_id):
    """Real Hub via the 3-app dance (main + __storage + orch). Hub's
    constructor takes a single `address authority_`; we pass the
    dispenser. getSpokeAddedAssets / getSpokeAddedShares (the methods
    treasury_spoke calls) are unauth views, so the authority value
    isn't actually checked at runtime."""
    authority = encoding.decode_address(account.address)
    return deploy_split_app(
        localnet.client.algod, account, "Hub",
        orch_id=orch_app_id,
        app_args=[authority],
    )


@pytest.fixture(scope="module")
def spoke(localnet, account, hub):
    # TreasurySpoke ctor: (address owner_, address hub_). hub_ in puya-sol
    # convention so the spoke's inner calls land on Hub's main app.
    owner_ = encoding.decode_address(account.address)
    hub_ = _appid_to_pseudo_addr_bytes(hub.main_id)
    return deploy_contract(
        localnet, account, "TreasurySpoke",
        app_args=[owner_, hub_],
    )


_call_counter = 0


def _call(client, method, *args, extra_fee_micro=None, apps=None):
    global _call_counter
    _call_counter += 1
    note = f"ts_{_call_counter}".encode()
    kwargs = dict(method=method, args=list(args), note=note)
    if extra_fee_micro is not None:
        kwargs["extra_fee"] = au.AlgoAmount(micro_algo=extra_fee_micro)
    if apps:
        kwargs["app_references"] = apps
    result = client.send.call(au.AppClientMethodCallParams(**kwargs))
    return result.abi_return


def test_deploy(spoke):
    assert spoke.app_id > 0


def test_owner(spoke, account):
    result = _call(spoke, "owner")
    assert result == account.address


def test_pendingOwner_initial(spoke):
    """pendingOwner should be zero address initially."""
    result = _call(spoke, "pendingOwner")
    assert result == ZERO_ADDR


def test_getUserTotalDebt(spoke, account):
    """Treasury spoke returns 0 for all debt queries."""
    result = _call(spoke, "getUserTotalDebt", 0, account.address)
    assert result == 0


def test_getUserPremiumDebtRay(spoke, account):
    """Treasury spoke returns 0 for premium debt."""
    result = _call(spoke, "getUserPremiumDebtRay", 0, account.address)
    assert result == 0


def test_getUserDebt(spoke, account):
    """getUserDebt should return (0, 0) tuple."""
    result = _call(spoke, "getUserDebt", 0, account.address)
    vals = list(result.values()) if isinstance(result, dict) else list(result)
    assert vals[0] == 0
    assert vals[1] == 0


def test_getReserveDebt(spoke):
    """getReserveDebt should return (0, 0) tuple."""
    result = _call(spoke, "getReserveDebt", 0)
    vals = list(result.values()) if isinstance(result, dict) else list(result)
    assert vals[0] == 0
    assert vals[1] == 0


def test_getReserveTotalDebt(spoke):
    """getReserveTotalDebt should return 0."""
    result = _call(spoke, "getReserveTotalDebt", 0)
    assert result == 0


def test_getUserSuppliedAssets(spoke, account):
    """getUserSuppliedAssets should return 0."""
    result = _call(spoke, "getUserSuppliedAssets", 0, account.address)
    assert result == 0


def test_getUserSuppliedShares(spoke, account):
    """getUserSuppliedShares should return 0."""
    result = _call(spoke, "getUserSuppliedShares", 0, account.address)
    assert result == 0


def test_borrow_reverts(spoke, account):
    """borrow should revert with UnsupportedAction for treasury spoke."""
    with pytest.raises(Exception, match="UnsupportedAction"):
        _call(spoke, "borrow", 0, 100, account.address)


def test_repay_reverts(spoke, account):
    """repay should revert with UnsupportedAction for treasury spoke."""
    with pytest.raises(Exception, match="UnsupportedAction"):
        _call(spoke, "repay", 0, 100, account.address)


def _hub_apps(hub):
    """All three of Hub's app refs (main / orch / storage). Needed in
    foreign-apps for any cross-contract call that goes through the
    dance, so AVM resource resolution admits the inner-itxn cascade."""
    return [hub.main_id, hub.orch_id, hub.storage_id]


def test_getSuppliedAmount_zero(spoke, hub):
    """TreasurySpoke.getSuppliedAmount → HUB.getSpokeAddedAssets.
    Inner-call chain:
      account → spoke (outer)
        → hub.main      (itxn 1)
          → orch        (itxn 2 from main's stub)
            → __storage Update install chunk  (itxn 3)
            → __storage call getSpokeAddedAssets (itxn 4)
            → __storage Update restore default (itxn 5)
    Plus pay-forward callsub if msg.value > 0 (here 0). 5 inner-itxns
    total, fee budget = 6×min_fee."""
    result = _call(spoke, "getSuppliedAmount", 0,
        extra_fee_micro=8000, apps=_hub_apps(hub))
    assert result == 0


def test_getSuppliedShares_zero(spoke, hub):
    result = _call(spoke, "getSuppliedShares", 0,
        extra_fee_micro=8000, apps=_hub_apps(hub))
    assert result == 0


def test_getReserveSuppliedAssets_zero(spoke, hub):
    result = _call(spoke, "getReserveSuppliedAssets", 0,
        extra_fee_micro=8000, apps=_hub_apps(hub))
    assert result == 0


def test_getReserveSuppliedShares_zero(spoke, hub):
    result = _call(spoke, "getReserveSuppliedShares", 0,
        extra_fee_micro=8000, apps=_hub_apps(hub))
    assert result == 0


def _call_with_result(client, method, *args):
    """Call and return the full result (for log inspection)."""
    global _call_counter
    _call_counter += 1
    note = f"ts_{_call_counter}".encode()
    return client.send.call(
        au.AppClientMethodCallParams(method=method, args=list(args), note=note)
    )


def test_transferOwnership(spoke, account):
    """transferOwnership should set pendingOwner."""
    _call(spoke, "transferOwnership", account.address)
    result = _call(spoke, "pendingOwner")
    assert result == account.address


def test_transferOwnership_emits_event(spoke, account):
    """transferOwnership should emit OwnershipTransferStarted ARC-28 event."""
    result = _call_with_result(spoke, "transferOwnership", account.address)
    events = _extract_events(result.confirmation)
    assert len(events) >= 1
    # Algorand event encoding: address → uint8[32] (bytes32). See arc56.json.
    selector = _arc28_selector("OwnershipTransferStarted(uint8[32],uint8[32])")
    event = next(e for e in events if e[:4] == selector)
    # Data contains two ARC4-encoded addresses (32 bytes each)
    data = event[4:]
    assert len(data) == 64
    new_owner = encoding.encode_address(data[32:64])
    assert new_owner == account.address
