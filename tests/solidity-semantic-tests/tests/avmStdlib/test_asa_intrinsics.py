"""AVM.sol ASA intrinsics — coverage lane for itxn/AsaIntrinsics.cpp.

Exercises every asa* lowering end-to-end on LocalNet: create (both arities),
the four metadata reads, balance, transfer/clawback, freeze, foreign-asset
opt-in, destroy.
"""

import pytest
from algosdk import account as algosdk_account, transaction
from algosdk.logic import get_application_address

from framework import as_int


def _funded_account(harness, microalgos=1_000_000):
    client = harness.localnet.algod
    acct = harness.localnet.account
    sk, addr = algosdk_account.generate_account()
    params = client.suggested_params()
    pay = transaction.PaymentTxn(acct.address, params, addr, microalgos)
    txid = client.send_transaction(pay.sign(acct.private_key))
    transaction.wait_for_confirmation(client, txid, 4)
    return addr, sk


def _axfer(harness, sender_addr, sender_sk, asset_id, receiver, amount):
    client = harness.localnet.algod
    params = client.suggested_params()
    txn = transaction.AssetTransferTxn(
        sender_addr, params, receiver, amount, asset_id)
    txid = client.send_transaction(txn.sign(sender_sk))
    return transaction.wait_for_confirmation(client, txid, 4)


def test_bits_bitlen_intrinsic(harness):
    """avmStdlib/contracts/asa_kit.sol"""
    arts = harness.compile("avmStdlib/contracts/asa_kit.sol")
    app = harness.deploy(arts)
    for value, expected in ((0, 0), (1, 1), (2, 2), (3, 2), (1 << 255, 256)):
        result = harness.call(app, "bitLength(uint256)", value)
        assert not result.reverted, result.fail_message
        assert as_int(result.abi_return) == expected


def test_asa_intrinsics_lifecycle(harness):
    """avmStdlib/contracts/asa_kit.sol"""
    arts = harness.compile("avmStdlib/contracts/asa_kit.sol")
    app = harness.deploy(arts, extra_funding_microalgos=1_000_000)
    app_addr = get_application_address(app.app_id)
    fee = {"extra_fee": 4_000}

    # asaCreate (4-arg) + the four metadata reads + creator balance.
    aid = as_int(harness.call(app, "create()", **fee).abi_return)
    assert aid > 0
    name, unit, dec, supply = harness.call(app, "meta()").abi_return
    assert (name, unit, as_int(dec), as_int(supply)) == (
        "Kit Token", "KIT", 2, 100_000)
    assert as_int(harness.call(app, "bal(address)", app_addr).abi_return) \
        == 100_000

    # transfer to an opted-in holder; clawback pulls back; freeze blocks the
    # holder's own axfer but never the app's clawback.
    holder_addr, holder_sk = _funded_account(harness)
    _axfer(harness, holder_addr, holder_sk, aid, holder_addr, 0)  # opt in
    harness.call(app, "send(address,uint256)", holder_addr, 1_500, **fee)
    assert as_int(harness.call(app, "bal(address)", holder_addr).abi_return) \
        == 1_500
    harness.call(app, "setFreeze(address,bool)", holder_addr, True, **fee)
    with pytest.raises(Exception):
        _axfer(harness, holder_addr, holder_sk, aid, app_addr, 100)
    harness.call(app, "claw(address,address,uint256)",
                 holder_addr, app_addr, 500, **fee)
    assert as_int(harness.call(app, "bal(address)", holder_addr).abi_return) \
        == 1_000
    harness.call(app, "setFreeze(address,bool)", holder_addr, False, **fee)
    _axfer(harness, holder_addr, holder_sk, aid, app_addr, 100)
    assert as_int(harness.call(app, "bal(address)", holder_addr).abi_return) \
        == 900

    # destroy needs the app to hold every unit again.
    harness.call(app, "claw(address,address,uint256)",
                 holder_addr, app_addr, 900, **fee)
    harness.call(app, "destroy()", **fee)

    # asaCreate (5-arg, default_frozen): only clawback moves units.
    cold = as_int(harness.call(app, "createFrozen()", **fee).abi_return)
    _axfer(harness, holder_addr, holder_sk, cold, holder_addr, 0)  # opt in
    harness.call(app, "sendCold(address,uint256)", holder_addr, 5, **fee)
    assert as_int(
        harness.call(app, "coldBal(address)", holder_addr).abi_return) == 5
    with pytest.raises(Exception):
        _axfer(harness, holder_addr, holder_sk, cold, app_addr, 1)

    # asaOptIn: the app opts into a FOREIGN asset and receives units.
    client = harness.localnet.algod
    acct = harness.localnet.account
    params = client.suggested_params()
    acfg = transaction.AssetConfigTxn(
        acct.address, params, total=1_000, default_frozen=False,
        unit_name="EXT", asset_name="External", decimals=0,
        strict_empty_address_check=False)
    txid = client.send_transaction(acfg.sign(acct.private_key))
    fid = transaction.wait_for_confirmation(client, txid, 4)["asset-index"]
    harness.call(app, "optInto(uint64)", fid, **fee)
    _axfer(harness, acct.address, acct.private_key, fid, app_addr, 250)
    assert as_int(
        harness.call(app, "foreignBal(uint64)", fid).abi_return) == 250
