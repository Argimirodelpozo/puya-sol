"""Shared native-payment policy, escrow routing, amount checks, and close payouts."""

import base64
import hashlib
import os

import pytest
from algosdk.account import generate_account
from algosdk.encoding import encode_address
from Crypto.Hash import keccak
from eth_abi import encode as abi_encode

SOURCE = "puyasolRegression/contracts/native_payments.sol"
AMOUNT = 250_000
METHODS = ("transferTo", "sendTo", "callTo", "assemblyTo", "assemblyWordTo", "typedTo", "transferOnce")
PLACEHOLDER = bytes.fromhex("ee" * 20)


def _call(harness, app, profile, signature, *args, **options):
    if profile == "arc4":
        return harness.call(app, signature, *args, extra_fee=30_000, **options)
    parameters = signature[signature.index("(") + 1:-1]
    types = parameters.split(",") if parameters else []
    selector = keccak.new(digest_bits=256, data=signature.encode()).digest()[:4]
    return harness.call_raw(app, selector, extra_args=(abi_encode(types, args),),
                            extra_fee=30_000, **options)


def _uint_return(result, profile):
    assert not result.reverted, result.fail_message
    if profile == "arc4":
        return result.abi_return
    payload = next(log[4:] for log in reversed(result.logs)
                   if log.startswith(bytes.fromhex("151f7c75")))
    return int.from_bytes(payload, "big")


def _deploy(harness, profile="arc4", extra_args=()):
    artifacts = harness.compile(SOURCE, extra_args=["--contract-abi", profile, *extra_args])
    sender = harness.deploy(artifacts, "NativePaymentSender", extra_funding_microalgos=3_000_000)
    receiver = harness.deploy(artifacts, "NativePaymentReceiver")
    return artifacts, sender, receiver


def _target(app_id, profile):
    return (encode_address(app_id.to_bytes(32, "big")) if profile == "arc4"
            else "0x" + app_id.to_bytes(20, "big").hex())


@pytest.mark.parametrize("profile", ["arc4", "evm"])
@pytest.mark.parametrize("method", METHODS)
def test_native_payments_reach_contract_escrow(harness, profile, method):
    _, sender, receiver = _deploy(harness, profile)
    client = harness.localnet.algod
    before = client.account_info(receiver.app_addr)["amount"]
    word = method == "assemblyWordTo"
    target = receiver.app_id if word else _target(receiver.app_id, profile)
    result = _call(harness, sender, profile,
                   f"{method}({'uint256' if word else 'address'},uint256)", target, AMOUNT)
    assert not result.reverted, result.fail_message
    assert client.account_info(receiver.app_addr)["amount"] == before + AMOUNT
    if method != "callTo":
        # transfer/send execute receive; typed/Yul calls already execute a callee.
        value = _call(harness, receiver, profile, "received()")
        assert _uint_return(value, profile) == AMOUNT
    if method == "transferOnce":
        for getter in ("receiverEvaluations()", "amountEvaluations()"):
            assert _uint_return(_call(harness, sender, profile, getter), profile) == 1


@pytest.mark.parametrize("profile", ["arc4", "evm"])
@pytest.mark.parametrize("method", ["transferTo", "sendTo"])
@pytest.mark.parametrize("amount", [0, AMOUNT])
@pytest.mark.parametrize("receiver_name", ["NativePaymentRejectingReceiver", "NativePaymentNoReceiver"])
def test_native_transfer_receiver_rejection_is_atomic(harness, profile, method, amount, receiver_name):
    artifacts, sender, _ = _deploy(harness, profile)
    receiver = harness.deploy(artifacts, receiver_name)
    client = harness.localnet.algod
    before = [client.account_info(app.app_addr)["amount"] for app in (sender, receiver)]
    result = _call(harness, sender, profile, f"{method}(address,uint256)",
                   _target(receiver.app_id, profile), amount, expect_revert=True)
    assert result.reverted, "receive/fallback rejection must abort the grouped payment"
    assert [client.account_info(app.app_addr)["amount"] for app in (sender, receiver)] == before


@pytest.mark.parametrize("profile", ["arc4", "evm"])
@pytest.mark.parametrize("method", ["transferTo", "sendTo"])
@pytest.mark.parametrize("amount", [0, AMOUNT])
def test_native_transfer_dispatches_fallback(harness, profile, method, amount):
    artifacts, sender, _ = _deploy(harness, profile)
    receiver = harness.deploy(artifacts, "NativePaymentFallbackReceiver")
    before = harness.localnet.algod.account_info(receiver.app_addr)["amount"]
    result = _call(harness, sender, profile, f"{method}(address,uint256)",
                   _target(receiver.app_id, profile), amount)
    assert not result.reverted, result.fail_message
    assert harness.localnet.algod.account_info(receiver.app_addr)["amount"] == before + amount
    assert _uint_return(_call(harness, receiver, profile, "received()"), profile) == amount
    assert _uint_return(_call(harness, receiver, profile, "calls()"), profile) == 1


@pytest.mark.parametrize("method", ["transferTo", "sendTo", "callTo", "transferOnce"])
def test_native_payments_preserve_real_accounts(harness, method):
    _, sender, _ = _deploy(harness)
    _, recipient = generate_account()
    result = _call(harness, sender, "arc4", f"{method}(address,uint256)", recipient, AMOUNT)
    assert not result.reverted, result.fail_message
    assert harness.localnet.algod.account_info(recipient)["amount"] == AMOUNT
    if method == "transferOnce":
        assert harness.call(sender, "receiverEvaluations()").abi_return == 1
        assert harness.call(sender, "amountEvaluations()").abi_return == 1


@pytest.mark.parametrize("method", METHODS)
def test_native_payment_amount_overflow_reverts(harness, method):
    _, sender, receiver = _deploy(harness)
    before = harness.localnet.algod.account_info(receiver.app_addr)["amount"]
    word = method == "assemblyWordTo"
    target = receiver.app_id if word else _target(receiver.app_id, "arc4")
    # The low 64 bits are an otherwise-valid payment: truncation would transfer it.
    result = _call(harness, sender, "arc4",
                   f"{method}({'uint256' if word else 'address'},uint256)",
                   target, (1 << 128) + AMOUNT, expect_revert=True)
    assert result.reverted, "an unrepresentable payment amount must revert"
    assert harness.localnet.algod.account_info(receiver.app_addr)["amount"] == before


@pytest.mark.parametrize("profile", ["arc4", "evm"])
@pytest.mark.parametrize("method", ["transferTo", "sendTo", "callTo"])
def test_native_payment_rejects_missing_application(harness, profile, method):
    _, sender, _ = _deploy(harness, profile)
    client = harness.localnet.algod
    before = client.account_info(sender.app_addr)["amount"]
    # A contract-convention value must not become a payment to its keyless bytes.
    target = _target((1 << 64) - 1, profile)
    result = _call(harness, sender, profile, f"{method}(address,uint256)",
                   target, AMOUNT, expect_revert=True)
    assert result.reverted, "a nonexistent application's escrow must not resolve"
    assert client.account_info(sender.app_addr)["amount"] == before


@pytest.mark.parametrize("profile", ["arc4", "evm"])
def test_native_child_funding_keeps_constructor_value(harness, profile):
    _, sender, _ = _deploy(harness, profile)
    result = _call(harness, sender, profile, "create(uint256)", AMOUNT)
    assert _uint_return(result, profile) == AMOUNT


@pytest.mark.parametrize("profile", ["arc4", "evm"])
@pytest.mark.parametrize("receiver_name", ["NativePaymentReceiver", "NativePaymentRejectingReceiver"])
def test_native_close_reaches_contract_escrow(harness, profile, receiver_name):
    artifacts, _, receiver = _deploy(harness, profile)
    if receiver_name != "NativePaymentReceiver":
        receiver = harness.deploy(artifacts, receiver_name)
    closing = harness.deploy(artifacts, "NativePaymentClose", extra_funding_microalgos=AMOUNT)
    client = harness.localnet.algod
    closing_balance = client.account_info(closing.app_addr)["amount"]
    before = client.account_info(receiver.app_addr)["amount"]
    result = _call(harness, closing, profile, "close(address)", _target(receiver.app_id, profile))
    assert not result.reverted, result.fail_message
    assert client.account_info(receiver.app_addr)["amount"] == before + closing_balance
    assert client.account_info(closing.app_addr)["amount"] == 0


@pytest.mark.parametrize("method", ["transferTo", "sendTo", "callTo", "close"])
def test_native_payments_map_xchain_beneficiary(harness, method):
    client = harness.localnet.algod
    # Deliberately approve-all, disposable test-only template; not a signing policy.
    teal = f"#pragma version 9\npushbytes 0x{PLACEHOLDER.hex()}\npop\npushint 1\n"
    template = base64.b64decode(client.compile(teal)["result"])
    assert template.count(PLACEHOLDER) == 1
    owner = os.urandom(20)
    program = template.replace(PLACEHOLDER, owner)
    recipient = encode_address(hashlib.new("sha512_256", b"Program" + program).digest())
    artifacts, sender, _ = _deploy(harness, "evm", ("--xchain-template", template.hex()))
    if method == "close":
        sender = harness.deploy(artifacts, "NativePaymentClose", extra_funding_microalgos=AMOUNT)
        expected = client.account_info(sender.app_addr)["amount"]
        result = _call(harness, sender, "evm", "close(address)", "0x" + owner.hex())
    else:
        expected = AMOUNT
        result = _call(harness, sender, "evm", f"{method}(address,uint256)",
                       "0x" + owner.hex(), AMOUNT)
    assert not result.reverted, result.fail_message
    assert client.account_info(recipient)["amount"] == expected
