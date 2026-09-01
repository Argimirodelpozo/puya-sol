"""xchain account model (--xchain-template) — EVM_DIVERGENCE.md.

A 20-byte EVM identity E owns the LogicSig account A(E) =
sha512_256("Program" || template-with-owner-spliced). This lane proves the
whole loop with a TOY approve-all template compiled on the spot:

- payout to a 160-bit identity funds A(E) — and A(E) is genuinely
  SPENDABLE: the lsig signs a payment out again;
- an app call SENT BY A(E) carrying the owner claim adopts E as
  msg.sender (whoami == E; deposit keys on E);
- a false claim from the wrong sender is rejected by the entry arm.
"""

import base64
import hashlib

from algosdk import transaction
from algosdk.atomic_transaction_composer import (
    AtomicTransactionComposer, LogicSigTransactionSigner, TransactionWithSigner)
from algosdk.encoding import decode_address, encode_address
from algosdk.logic import get_application_address
from Crypto.Hash import keccak as _keccak
from eth_abi import encode as evm_abi_encode

from framework import as_int
from framework.call import _populate_group_resources_progressively

PLACEHOLDER = bytes.fromhex("ee" * 20)
OWNER = __import__("os").urandom(20)  # fresh per run: A(E) is deterministic

TOY_TEMPLATE_TEAL = f"""#pragma version 9
pushbytes 0x{PLACEHOLDER.hex()}
pop
pushint 1
"""


def sel(sig):
    d = _keccak.new(digest_bits=256)
    d.update(sig.encode())
    return d.digest()[:4]


def lsig_address(program: bytes) -> str:
    return encode_address(hashlib.new(
        "sha512_256", b"Program" + program).digest())


def evm_ret(result):
    magic = bytes.fromhex("151f7c75")
    return next((bytes(l)[4:] for l in reversed(result.logs)
                 if bytes(l).startswith(magic)), None)


def test_xchain_accounts(harness):
    """puyasolRegression/contracts/xchain_accounts.sol"""
    client = harness.localnet.algod
    acct = harness.localnet.account

    template = base64.b64decode(client.compile(TOY_TEMPLATE_TEAL)["result"])
    assert template.count(PLACEHOLDER) == 1

    arts = harness.compile(
        "puyasolRegression/contracts/xchain_accounts.sol",
        extra_args=["--contract-abi", "evm",
                    "--xchain-template", template.hex(),
                    "--xchain-placeholder", PLACEHOLDER.hex()])
    app = harness.deploy(arts, extra_funding_microalgos=2_000_000)

    owner_prog = template.replace(PLACEHOLDER, OWNER)
    derived = lsig_address(owner_prog)

    # 1. payout(E) funds A(E) — the derived, spendable account.
    amount = 250_000
    body = evm_abi_encode(["address", "uint256"],
                          ["0x" + OWNER.hex(), amount])
    r = harness.call_raw(app, sel("payout(address,uint256)"),
                         extra_args=(body,), extra_fee=10_000)
    assert not r.reverted, r.fail_message
    assert client.account_info(derived)["amount"] == amount

    # 2. A(E) SPENDS: the lsig signs a payment back out.
    lsig = transaction.LogicSigAccount(owner_prog)
    sp = client.suggested_params()
    pay = transaction.PaymentTxn(derived, sp, acct.address, 50_000)
    txid = client.send_transaction(
        transaction.LogicSigTransaction(pay, lsig))
    transaction.wait_for_confirmation(client, txid, 4)
    assert client.account_info(derived)["amount"] < amount

    # 3. A(E) calls the app WITH the owner claim: msg.sender adopts E.
    def lsig_app_call(selector, bodybytes, claim=OWNER):
        atc = AtomicTransactionComposer()
        sp2 = client.suggested_params()
        sp2.flat_fee = True
        sp2.fee = 2000
        args = [selector, bodybytes] + ([claim] if claim is not None else [])
        txn = transaction.ApplicationNoOpTxn(
            derived, sp2, app.app_id, app_args=args)
        atc.add_transaction(
            TransactionWithSigner(txn, LogicSigTransactionSigner(lsig)))
        atc = _populate_group_resources_progressively(atc, client, "xchain")
        return atc.execute(client, 4)

    res = lsig_app_call(sel("whoami()"), b"")
    logs = [base64.b64decode(l) for l in
            client.pending_transaction_info(res.tx_ids[0]).get("logs", [])]
    ret = next(l[4:] for l in reversed(logs)
               if l.startswith(bytes.fromhex("151f7c75")))
    assert ret[-20:] == OWNER

    # deposit keys on the TRUE identity E.
    lsig_app_call(sel("deposit(uint256)"),
                  evm_abi_encode(["uint256"], [77]))
    balbody = evm_abi_encode(["address"], ["0x" + OWNER.hex()])
    r = harness.call_raw(app, sel("bal(address)"), extra_args=(balbody,))
    assert int.from_bytes(evm_ret(r), "big") == 77

    # 4. a FALSE claim (wrong sender for the claimed owner) is rejected.
    import pytest
    other = bytes.fromhex("99" * 20)
    with pytest.raises(Exception):
        lsig_app_call(sel("whoami()"), b"", claim=other)
