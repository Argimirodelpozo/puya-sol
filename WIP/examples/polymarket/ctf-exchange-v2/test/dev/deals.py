"""Deal helpers for matchOrders settlement tests.

Mirrors Foundry's `deal()` cheat: directly mint balances to maker/taker
accounts on the stateful USDC + CTF mocks.

Usage:
    deal_usdc(usdc, recipient_addr32, 50_000_000)
    deal_outcome(ctf, recipient_addr32, yes_token_id, 50_000_000)
    prepare_condition(ctf, condition_id_32, yes_id, no_id)

`recipient_addr32` is a 32-byte AVM address (not a 20-byte eth address).

CTF balance keys + USDC allowance keys are sha256-hashed because raw
`account||tokenId` (64B) + boxmap prefix exceeds AVM's 64B box-name cap.
USDC balances use the bare 32-byte address (b_ prefix → 34B, fits).
"""
import hashlib

import algokit_utils as au

from .deploy import AUTO_POPULATE


def _sha256(b: bytes) -> bytes:
    return hashlib.sha256(b).digest()


def deal_usdc(usdc, recipient_addr32: bytes, amount: int, *, extra_fee: int = 10_000):
    """Mint `amount` USDC to `recipient_addr32`."""
    return usdc.send.call(au.AppClientMethodCallParams(
        method="mint",
        args=[bytes(recipient_addr32), amount],
        extra_fee=au.AlgoAmount(micro_algo=extra_fee),
        box_references=[au.BoxReference(app_id=0, name=b"b_" + bytes(recipient_addr32))],
    ), send_params=AUTO_POPULATE).abi_return


def set_allowance(usdc, owner_addr32: bytes, spender_addr32: bytes, amount: int,
                  *, extra_fee: int = 10_000):
    """Test-only: set `owner`'s USDC allowance for `spender` directly.
    Mirrors `vm.prank(maker); usdc.approve(spender, amount)` on Foundry."""
    allow_key = _sha256(bytes(owner_addr32) + bytes(spender_addr32))
    return usdc.send.call(au.AppClientMethodCallParams(
        method="setAllowance",
        args=[bytes(owner_addr32), bytes(spender_addr32), amount],
        extra_fee=au.AlgoAmount(micro_algo=extra_fee),
        box_references=[au.BoxReference(app_id=0, name=b"a_" + allow_key)],
    ), send_params=AUTO_POPULATE).abi_return


def deal_usdc_and_approve(usdc, owner_addr32: bytes, spender_addr32: bytes,
                          amount: int):
    """Mint `amount` USDC to `owner_addr32` and set their allowance for
    `spender_addr32` to `amount`. Mirrors `dealUsdcAndApprove` from v1
    Foundry tests."""
    deal_usdc(usdc, owner_addr32, amount)
    set_allowance(usdc, owner_addr32, spender_addr32, amount)


def deal_outcome(ctf, recipient_addr32: bytes, token_id: int, amount: int,
                 *, extra_fee: int = 10_000):
    """Mint `amount` of CTF outcome token `token_id` to `recipient_addr32`."""
    tid_bytes = token_id.to_bytes(32, "big")
    bal_key = _sha256(bytes(recipient_addr32) + tid_bytes)
    return ctf.send.call(au.AppClientMethodCallParams(
        method="mint",
        args=[bytes(recipient_addr32), token_id, amount],
        extra_fee=au.AlgoAmount(micro_algo=extra_fee),
        box_references=[au.BoxReference(app_id=0, name=b"b_" + bal_key)],
    ), send_params=AUTO_POPULATE).abi_return


def set_approval(ctf, owner_addr32: bytes, operator_addr32: bytes,
                 approved: bool = True, *, extra_fee: int = 10_000):
    """Test-only: set `owner`'s setApprovalForAll(operator, approved)
    directly on the CTF mock."""
    ap_key = _sha256(bytes(owner_addr32) + bytes(operator_addr32))
    return ctf.send.call(au.AppClientMethodCallParams(
        method="setApproval",
        args=[bytes(owner_addr32), bytes(operator_addr32), approved],
        extra_fee=au.AlgoAmount(micro_algo=extra_fee),
        box_references=[au.BoxReference(app_id=0, name=b"ap_" + ap_key)],
    ), send_params=AUTO_POPULATE).abi_return


def deal_outcome_and_approve(ctf, owner_addr32: bytes, operator_addr32: bytes,
                             token_id: int, amount: int):
    """Mint `amount` of `token_id` to `owner` and set
    setApprovalForAll(operator, true). Mirrors
    `dealOutcomeTokensAndApprove` from v1 Foundry tests."""
    deal_outcome(ctf, owner_addr32, token_id, amount)
    set_approval(ctf, owner_addr32, operator_addr32, True)


def prepare_condition(ctf, condition_id_32: bytes, yes_id: int, no_id: int,
                      *, extra_fee: int = 10_000):
    """Register a (yesId, noId) partition for `condition_id_32`. matchOrders'
    MINT/MERGE paths look this up via splitPosition / mergePositions."""
    return ctf.send.call(au.AppClientMethodCallParams(
        method="prepare_condition",
        args=[list(condition_id_32), yes_id, no_id],
        extra_fee=au.AlgoAmount(micro_algo=extra_fee),
        box_references=[au.BoxReference(app_id=0, name=b"p_" + bytes(condition_id_32))],
    ), send_params=AUTO_POPULATE).abi_return


def usdc_balance(usdc, addr32: bytes) -> int:
    """Read `addr32`'s USDC balance from the stateful mock."""
    raw = usdc.send.call(au.AppClientMethodCallParams(
        method="balanceOf",
        args=[bytes(addr32)],
        extra_fee=au.AlgoAmount(micro_algo=10_000),
        box_references=[au.BoxReference(app_id=0, name=b"b_" + bytes(addr32))],
    ), send_params=AUTO_POPULATE).abi_return
    return int(raw) if isinstance(raw, int) else int.from_bytes(bytes(raw), "big")


def ctf_balance(ctf, addr32: bytes, token_id: int) -> int:
    """Read `addr32`'s balance for outcome `token_id` on the CTF mock."""
    tid_bytes = token_id.to_bytes(32, "big")
    bal_key = _sha256(bytes(addr32) + tid_bytes)
    raw = ctf.send.call(au.AppClientMethodCallParams(
        method="balanceOf",
        args=[bytes(addr32), token_id],
        extra_fee=au.AlgoAmount(micro_algo=10_000),
        box_references=[au.BoxReference(app_id=0, name=b"b_" + bal_key)],
    ), send_params=AUTO_POPULATE).abi_return
    return int(raw) if isinstance(raw, int) else int.from_bytes(bytes(raw), "big")
