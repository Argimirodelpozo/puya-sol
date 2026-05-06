"""
AAVE V4 AaveOracle tests.
"""

import pytest
import hashlib
import algokit_utils as au
from algosdk import encoding
from conftest import deploy_contract
from uros_dance import deploy_split_app


ZERO_ADDR = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAY5HFKQ"


def _appid_to_pseudo_addr_bytes(app_id: int) -> bytes:
    """puya-sol app-address convention: \\x00*24 + itob(app_id). Tests
    passing a real app's address as a Solidity `address` arg need this
    format so SolExternalCall::addressToAppId recovers the right id."""
    return b"\x00" * 24 + app_id.to_bytes(8, "big")


def _appid_to_pseudo_addr_str(app_id: int) -> str:
    """Same convention, as a checksummed Algorand address string."""
    return encoding.encode_address(_appid_to_pseudo_addr_bytes(app_id))

_call_counter = 0


def _box_ref(app_id, key):
    return au.BoxReference(app_id=app_id, name=key)


def _mapping_box_key(mapping_name, key_bytes):
    return mapping_name.encode() + hashlib.sha256(key_bytes).digest()


def _biguint_key(val):
    raw = val.to_bytes((val.bit_length() + 7) // 8, 'big') if val > 0 else b'\x00'
    padded = b'\x00' * 64 + raw
    return padded[len(padded) - 64:]


def _call(client, method, *args, boxes=None, extra_fee_micro=None,
          apps=None):
    global _call_counter
    _call_counter += 1
    note = f"ao_{_call_counter}".encode()
    kwargs = dict(method=method, args=list(args), note=note)
    if boxes:
        kwargs["box_references"] = boxes
    if extra_fee_micro is not None:
        kwargs["extra_fee"] = au.AlgoAmount(micro_algo=extra_fee_micro)
    if apps:
        kwargs["app_references"] = apps
    result = client.send.call(au.AppClientMethodCallParams(**kwargs))
    return result.abi_return


@pytest.fixture(scope="module")
def oracle(localnet, account):
    decimals = 8
    description_raw = b"Aave Oracle"
    # __postInit(uint64 decimals, string description) — ARC4 `string` is
    # encoded as a 2-byte BE length prefix + the UTF-8 bytes. Without
    # the prefix the constructor's storage write copies a length read
    # from random data and the field assertion fails on read.
    description = len(description_raw).to_bytes(2, "big") + description_raw
    return deploy_contract(
        localnet, account, "AaveOracle",
        app_args=[decimals.to_bytes(8, "big"), description],
    )


@pytest.fixture(scope="module")
def access_manager(localnet, account):
    """AccessManager contract with the dispenser as initialAdmin. Used as
    the authority for a SpokeInstance — without setTargetFunctionRole on
    spoke methods, every restricted method requires ADMIN_ROLE, which
    the dispenser holds."""
    initial_admin = encoding.decode_address(account.address)
    return deploy_contract(
        localnet, account, "AccessManager",
        app_args=[initial_admin],
    )


def _raw_abi_call(algod, sender, app_id, sig: str, *args: bytes,
                  fee_mult: int = 1, foreign_apps=None,
                  boxes=None) -> dict:
    """Raw ApplicationCallTxn with ABI selector + concatenated args.
    Bypasses algokit's Arc56Contract / algosdk's ABI Method parsing,
    which rejects types like int200 / int256 that puya-sol emits in
    real AAVE V4 arc56s. Each arg must be pre-encoded by the caller."""
    from algosdk.transaction import (
        ApplicationCallTxn, OnComplete, wait_for_confirmation)
    sel = hashlib.new("sha512_256", sig.encode()).digest()[:4]
    sp = algod.suggested_params()
    if fee_mult > 1:
        sp.fee = sp.min_fee * fee_mult
        sp.flat_fee = True
    txn = ApplicationCallTxn(
        sender=sender.address, sp=sp, index=app_id,
        on_complete=OnComplete.NoOpOC,
        app_args=[sel, *args],
        foreign_apps=foreign_apps,
        boxes=boxes,
    )
    txid = algod.send_transaction(txn.sign(sender.private_key))
    return wait_for_confirmation(algod, txid, 4)


@pytest.fixture(scope="module")
def spoke(localnet, account, oracle, access_manager, orch_app_id):
    """SpokeInstance via 3-app uros dance. Constructor arg: oracle's
    address (puya-sol app-address convention) + maxUserReservesLimit_=16.
    After deploy, call initialize(authority=access_manager) so the
    AccessManaged-restricted methods are gated by AccessManager."""
    oracle_arg = _appid_to_pseudo_addr_bytes(oracle.app_id)
    max_user_reserves = (16).to_bytes(8, "big")
    spoke_dep = deploy_split_app(
        localnet.client.algod, account, "SpokeInstance",
        orch_id=orch_app_id,
        app_args=[oracle_arg, max_user_reserves],
    )
    # initialize(authority) — the spoke's __AccessManaged_init writes
    # the AccessManager id; restricted methods downstream check there.
    # NOTE: spoke.initialize(authority) currently traps with
    # `getbit index > 63 with Uint` — puya-sol emits getbit on a
    # uint64 at bit 64 for OZ Initializable's reinitializer modifier
    # (the `_initializing` bool packed beside `_initialized` uint64).
    # The bit should be in a byte slot, not a uint slot. Tracked as
    # follow-up. test_set_spoke doesn't need initialize since it only
    # reads spoke.ORACLE() (set by __postInit), so we leave the spoke
    # uninitialized for now. test_set_reserve_source / _with_unit_feed
    # WILL need initialize fixed (their `restricted` modifier reads
    # the authority set inside initialize).
    spoke_dep.access_manager_id = access_manager.app_id  # for tests
    return spoke_dep


def test_deploy(oracle):
    assert oracle.app_id > 0


def test_description(oracle):
    result = _call(oracle, "DESCRIPTION")
    assert result == "Aave Oracle"


def test_spoke_initial(oracle):
    """SPOKE should be zero address initially."""
    result = _call(oracle, "SPOKE")
    assert result == ZERO_ADDR


def test_get_reserve_source_unset(oracle):
    """getReserveSource for unset reserve should return zero address."""
    reserve_id = 0
    box_key = _mapping_box_key("_reserveSources", _biguint_key(reserve_id))
    result = _call(oracle, "getReserveSource", reserve_id, boxes=[_box_ref(oracle.app_id, box_key)])
    assert result == ZERO_ADDR


@pytest.mark.xfail(
    reason=(
        "SpokeInstance fixture wired (uros split + AccessManager) but "
        "__postInit can't complete on main: it `box_create`s 7 boxes "
        "(_reserves / _hubAssetIdToReserveId / _dynamicConfig / "
        "_positionStatus / _userPositions / _positionManager / __gap), "
        "and the AVM 8-reference cap (boxes + foreign_apps + accts + "
        "assets) leaves room for only 7 boxes IF we drop a foreign "
        "app — but every method on main calls __uros_forward_value "
        "which reads TMPL_UROS_STORAGE_APP_ID via app_params_get, so "
        "storage_id MUST be in foreign_apps. Result: __postInit needs "
        "to be split across multiple grouped txns (each with its own "
        "8-ref budget), or the splitter needs to defer __dyn_storage's "
        "box_create to a separate post-init step. Without ORACLE set "
        "on storage's globals, ISpoke(spoke).ORACLE() returns 0 and "
        "fails the `== address(this)` check inside oracle.setSpoke."
    )
)
def test_set_spoke(oracle, spoke, account):
    """setSpoke should update SPOKE to a real spoke whose ORACLE()
    returns this oracle's address."""
    spoke_addr = _appid_to_pseudo_addr_str(spoke.main_id)
    _call(oracle, "setSpoke", spoke_addr,
          extra_fee_micro=10_000,
          apps=[spoke.main_id, spoke.orch_id, spoke.storage_id])
    result = _call(oracle, "SPOKE")
    assert result == spoke_addr


@pytest.mark.xfail(
    reason=(
        "Same __postInit blocker as test_set_spoke. Plus: "
        "spoke.initialize(authority) traps with `getbit index > 63 "
        "with Uint` — puya-sol emits getbit on a uint64 at bit 64 "
        "for OZ Initializable's reinitializer modifier (the "
        "`_initializing` bool packed beside `_initialized` uint64). "
        "The bit should be in a byte slot, not a uint slot. Plus: "
        "addReserve plumbing — updateReservePriceSource requires "
        "`reserveId < _reserveCount`, so the test would need to "
        "call addReserve first (which itself inner-calls "
        "oracle.setReserveSource, hub.addAsset, etc.)."
    )
)
def test_set_reserve_source(oracle, spoke, account):
    """setReserveSource via spoke.updateReservePriceSource (which has
    the `restricted` modifier — dispenser holds ADMIN_ROLE)."""
    raise NotImplementedError


@pytest.mark.xfail(
    reason=(
        "Same SpokeInstance + addReserve + initialize blockers as "
        "test_set_reserve_source. Once those are unblocked, this test "
        "deploys a UnitPriceFeed (already exists), routes through "
        "spoke.updateReservePriceSource(reserveId, priceSource=feed)."
    )
)
def test_get_reserve_price_with_unit_feed(oracle, spoke, localnet, account):
    """Set up a UnitPriceFeed as price source and verify assignment."""
    raise NotImplementedError
