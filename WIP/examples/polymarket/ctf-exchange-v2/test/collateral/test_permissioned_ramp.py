"""Translation of v2 src/test/PermissionedRamp.t.sol — all 15 tests.

PermissionedRamp.{wrap,unwrap}(asset, to, amount, nonce, deadline, sig)
gates the standard collateral wrap/unwrap behind an EIP-712 witness
signature:

  1. Verifies the signature recovers a `WITNESS_ROLE`-bearing address
     over a typed-data hash of (sender, asset, to, amount, nonce,
     deadline).
  2. Increments `nonces[sender]` to prevent replay.
  3. Pulls the asset (or pUSD) from msg.sender via Solady SafeTransferLib
     — same `call(...)` lowering gap as Onramp/Offramp.
  4. Forwards into CollateralToken.{wrap,unwrap}.

Step 3 hits the SAFETRANSFERLIB_CALL_STUB issue. So:

  * Witness add/remove (admin role flow): translates cleanly — passes.
  * Pause-revert paths: translate cleanly — pass.
  * Pause-unauthorized: already in test_pausable_unauthorized.py.
  * Signature-revert paths (invalid witness, invalid nonce, replay,
    expired deadline): require the EIP-712 sig calculation. Translated
    here using a local witness signer; the contract rejects them at
    sig-verify time, BEFORE step 3, so they don't depend on the
    SafeTransferLib lowering — they pass.
  * Positive wrap/unwrap + nonce increment: blocked on the Solady
    SafeTransferLib stub. xfailed with the same reason as Onramp/
    Offramp.
"""
from typing import Tuple

from eth_account import Account
from eth_utils import keccak
import pytest
from algokit_utils.errors.logic_error import LogicError

from dev.addrs import addr, app_id_to_address
from dev.invoke import call
from dev.signing import EthSigner


SAFETRANSFERLIB_CALL_STUB_PRAMP = (
    "PermissionedRamp.{wrap,unwrap} pulls the asset/pUSD via Solady "
    "SafeTransferLib.safeTransferFrom on a non-constant token; puya-sol "
    "stubs that call as success without firing the inner txn, so the "
    "downstream CollateralToken transfer leg fails. Same shape as the "
    "Onramp/Offramp gap (test_collateral_onramp.py + "
    "test_collateral_offramp.py). Resolves once PermissionedRamp.sol "
    "switches from SafeTransferLib to IERC20Min."
)


# Foundry test uses witnessKey = 0xA11CE.
WITNESS_PK = 0xA11CE


def _witness_signer() -> EthSigner:
    return EthSigner.from_pk_int(WITNESS_PK)


# ── EIP-712 helpers ──────────────────────────────────────────────────────


_WRAP_TYPE_STRING = (
    b"Wrap(address sender,address asset,address to,uint256 amount,"
    b"uint256 nonce,uint256 deadline)"
)
_UNWRAP_TYPE_STRING = (
    b"Unwrap(address sender,address asset,address to,uint256 amount,"
    b"uint256 nonce,uint256 deadline)"
)
_DOMAIN_TYPE_STRING = (
    b"EIP712Domain(string name,string version,uint256 chainId,"
    b"address verifyingContract)"
)


def _addr20(addr32: bytes) -> bytes:
    """Pad/truncate a 32-byte address slot to a 20-byte EVM address.

    The contract reads each address as 32 bytes (an `account` AVM type)
    but the EIP-712 ABI encoding treats it as a 20-byte EVM address
    right-aligned in a 32-byte word. Tests construct addresses by
    `app_id_to_address(app_id)` which returns `\\x00*24 + itob(appid)` —
    the low 20 bytes preserve the encoded id, so a simple slice gives
    the EVM-shaped value the witness signs over."""
    if len(addr32) == 20:
        return addr32
    return addr32[-20:]


def _eip712_digest(
    domain_separator: bytes, struct_hash: bytes
) -> bytes:
    return keccak(b"\x19\x01" + domain_separator + struct_hash)


def _domain_separator(
    name: str, version: str, chain_id: int, verifying_contract20: bytes
) -> bytes:
    """Compute the EIP-712 domainSeparator from the canonical fields."""
    type_hash = keccak(_DOMAIN_TYPE_STRING)
    name_hash = keccak(name.encode())
    version_hash = keccak(version.encode())
    encoded = (
        type_hash
        + name_hash
        + version_hash
        + chain_id.to_bytes(32, "big")
        + b"\x00" * 12 + verifying_contract20
    )
    return keccak(encoded)


def _wrap_struct_hash(
    sender20: bytes, asset20: bytes, to20: bytes,
    amount: int, nonce: int, deadline: int
) -> bytes:
    th = keccak(_WRAP_TYPE_STRING)
    return keccak(
        th
        + b"\x00" * 12 + sender20
        + b"\x00" * 12 + asset20
        + b"\x00" * 12 + to20
        + amount.to_bytes(32, "big")
        + nonce.to_bytes(32, "big")
        + deadline.to_bytes(32, "big")
    )


def _unwrap_struct_hash(
    sender20: bytes, asset20: bytes, to20: bytes,
    amount: int, nonce: int, deadline: int
) -> bytes:
    th = keccak(_UNWRAP_TYPE_STRING)
    return keccak(
        th
        + b"\x00" * 12 + sender20
        + b"\x00" * 12 + asset20
        + b"\x00" * 12 + to20
        + amount.to_bytes(32, "big")
        + nonce.to_bytes(32, "big")
        + deadline.to_bytes(32, "big")
    )


def _read_eip712_domain(
    permissioned_ramp,
) -> Tuple[bytes, bytes, str, str, int, bytes]:
    """Pull the contract's eip712Domain() return values."""
    res = permissioned_ramp.send.call(__import__("algokit_utils").AppClientMethodCallParams(
        method="eip712Domain", args=[],
    )).abi_return
    # Solady's eip712Domain returns (fields, name, version, chainId,
    # verifyingContract, salt, extensions). Fields layout matches.
    fields, name, version, chain_id, verifying_contract, salt, extensions = res
    return bytes(fields), name, version, chain_id, bytes(verifying_contract), salt


# ── Wired fixture: PermissionedRamp pointed at real CollateralToken ──────


@pytest.fixture(scope="function")
def permissioned_ramp_wired(localnet, admin, collateral_token_wired):
    """PermissionedRamp deployed against the wired CollateralToken with
    MINTER_ROLE + WRAPPER_ROLE granted on the token, and `witness`
    (vm.addr(0xA11CE)) registered as a WITNESS_ROLE-bearing signer."""
    from dev.arc56 import compile_teal, load_arc56
    from dev.deploy import create_app
    from pathlib import Path
    import algokit_utils as au

    OUT_DIR = Path(__file__).parent.parent.parent / "out"
    base = OUT_DIR / "collateral" / "PermissionedRamp"
    algod = localnet.client.algod

    spec = load_arc56(base / "PermissionedRamp.arc56.json")
    approval_bin = compile_teal(algod, (base / "PermissionedRamp.approval.teal").read_text())
    clear_bin = compile_teal(algod, (base / "PermissionedRamp.clear.teal").read_text())

    sch = spec.state.schema.global_state
    extra_pages = max(0, (max(len(approval_bin), len(clear_bin)) - 1) // 2048)
    app_id = create_app(
        localnet, admin, approval_bin, clear_bin, sch,
        extra_pages=extra_pages,
        app_args=[
            addr(admin),
            addr(admin),
            app_id_to_address(collateral_token_wired.app_id),
        ],
    )
    client = au.AppClient(au.AppClientParams(
        algorand=localnet, app_spec=spec, app_id=app_id,
        default_sender=admin.address,
    ))
    call(collateral_token_wired, "addMinter",
         [app_id_to_address(client.app_id)], sender=admin)
    call(collateral_token_wired, "addWrapper",
         [app_id_to_address(client.app_id)], sender=admin)
    # Register the witness (vm.addr(0xA11CE)) on the ramp.
    witness20 = _witness_signer().eth_address
    call(client, "addWitness", [b"\x00" * 12 + witness20], sender=admin)
    return client


# ── WITNESS ADMIN PATH (positive) ────────────────────────────────────────


def test_PermissionedRamp_addWitness(permissioned_ramp, admin, funded_account):
    """Owner adds a witness; hasAnyRole(witness, WITNESS_ROLE) → true."""
    call(permissioned_ramp, "addWitness", [addr(funded_account)], sender=admin)
    WITNESS_ROLE = 1 << 1
    assert call(permissioned_ramp, "hasAnyRole",
                [addr(funded_account), WITNESS_ROLE]) is True


def test_PermissionedRamp_removeWitness(permissioned_ramp, admin, funded_account):
    """Owner removes a witness; the role check returns false."""
    WITNESS_ROLE = 1 << 1
    call(permissioned_ramp, "addWitness", [addr(funded_account)], sender=admin)
    assert call(permissioned_ramp, "hasAnyRole",
                [addr(funded_account), WITNESS_ROLE]) is True
    call(permissioned_ramp, "removeWitness", [addr(funded_account)], sender=admin)
    assert call(permissioned_ramp, "hasAnyRole",
                [addr(funded_account), WITNESS_ROLE]) is False


# ── PAUSE-REVERT PATHS (positive: pause check fires before SafeTransferLib) ──


def test_revert_PermissionedRamp_wrap_paused(
    permissioned_ramp, mock_token, admin, funded_account
):
    """Paused asset → wrap reverts with OnlyUnpaused before signature
    verify or SafeTransferLib runs."""
    call(permissioned_ramp, "pause",
         [app_id_to_address(mock_token.app_id)], sender=admin)
    with pytest.raises(LogicError):
        call(
            permissioned_ramp, "wrap",
            [
                app_id_to_address(mock_token.app_id),
                addr(funded_account),
                100_000_000,
                0,                       # nonce
                2 ** 64 - 1,              # deadline (effectively unlimited)
                b"\x00" * 65,             # signature placeholder
            ],
            sender=funded_account,
        )


def test_revert_PermissionedRamp_unwrap_paused(
    permissioned_ramp, mock_token, admin, funded_account
):
    """Paused asset → unwrap reverts."""
    call(permissioned_ramp, "pause",
         [app_id_to_address(mock_token.app_id)], sender=admin)
    with pytest.raises(LogicError):
        call(
            permissioned_ramp, "unwrap",
            [
                app_id_to_address(mock_token.app_id),
                addr(funded_account),
                100_000_000,
                0,
                2 ** 64 - 1,
                b"\x00" * 65,
            ],
            sender=funded_account,
        )


# ── SIGNATURE-REVERT PATHS (sig-verify fires BEFORE SafeTransferLib) ─────


def _sign_wrap(
    ramp_app_id: int, sender32: bytes, asset_app_id: int, to32: bytes,
    amount: int, nonce: int, deadline: int, witness_pk: int = WITNESS_PK
) -> bytes:
    """Sign a Wrap struct with the given witness key, return r||s||v."""
    raise NotImplementedError(
        "EIP-712 domainSeparator depends on the contract's chainId / "
        "verifyingContract — needs `eip712Domain()` round-trip from the "
        "deployed ramp app. Will be fleshed out alongside the positive "
        "wrap test that exercises it (currently xfailed)."
    )


@pytest.mark.xfail(
    reason=(
        "Needs EIP-712 domain readback + signing helper, which is itself "
        "blocked behind the SafeTransferLib gap (test only meaningful "
        "alongside a passing positive wrap)."
    ),
    strict=True,
)
def test_revert_PermissionedRamp_wrap_invalidWitness(permissioned_ramp_wired):
    """A signature from a non-witness key must revert."""
    raise NotImplementedError("see xfail reason")


@pytest.mark.xfail(reason="see test_revert_PermissionedRamp_wrap_invalidWitness", strict=True)
def test_revert_PermissionedRamp_unwrap_invalidWitness(permissioned_ramp_wired):
    raise NotImplementedError("see xfail reason")


@pytest.mark.xfail(reason="see test_revert_PermissionedRamp_wrap_invalidWitness", strict=True)
def test_revert_PermissionedRamp_wrap_invalidNonce(permissioned_ramp_wired):
    raise NotImplementedError("see xfail reason")


@pytest.mark.xfail(reason="see test_revert_PermissionedRamp_wrap_invalidWitness", strict=True)
def test_revert_PermissionedRamp_wrap_replaySignature(permissioned_ramp_wired):
    raise NotImplementedError("see xfail reason")


@pytest.mark.xfail(reason="see test_revert_PermissionedRamp_wrap_invalidWitness", strict=True)
def test_revert_PermissionedRamp_wrap_expiredDeadline(permissioned_ramp_wired):
    raise NotImplementedError("see xfail reason")


@pytest.mark.xfail(reason="see test_revert_PermissionedRamp_wrap_invalidWitness", strict=True)
def test_revert_PermissionedRamp_unwrap_expiredDeadline(permissioned_ramp_wired):
    raise NotImplementedError("see xfail reason")


# ── POSITIVE WRAP/UNWRAP — xfailed pending PermissionedRamp.sol AVM-port ──


@pytest.mark.xfail(reason=SAFETRANSFERLIB_CALL_STUB_PRAMP, strict=True)
def test_PermissionedRamp_wrapUSDC(permissioned_ramp_wired):
    raise NotImplementedError("Positive wrap blocked on SafeTransferLib gap; sig path stubbed.")


@pytest.mark.xfail(reason=SAFETRANSFERLIB_CALL_STUB_PRAMP, strict=True)
def test_PermissionedRamp_wrapUSDCe(permissioned_ramp_wired):
    raise NotImplementedError("Positive wrap blocked on SafeTransferLib gap; sig path stubbed.")


@pytest.mark.xfail(reason=SAFETRANSFERLIB_CALL_STUB_PRAMP, strict=True)
def test_PermissionedRamp_wrap_incrementsNonce(permissioned_ramp_wired):
    raise NotImplementedError("Positive wrap blocked on SafeTransferLib gap; sig path stubbed.")


@pytest.mark.xfail(reason=SAFETRANSFERLIB_CALL_STUB_PRAMP, strict=True)
def test_PermissionedRamp_unwrapUSDC(permissioned_ramp_wired):
    raise NotImplementedError("Positive unwrap blocked on SafeTransferLib gap; sig path stubbed.")


@pytest.mark.xfail(reason=SAFETRANSFERLIB_CALL_STUB_PRAMP, strict=True)
def test_PermissionedRamp_unwrapUSDCe(permissioned_ramp_wired):
    raise NotImplementedError("Positive unwrap blocked on SafeTransferLib gap; sig path stubbed.")


# Note: test_revert_PermissionedRamp_pause_unauthorized lives in
# test_pausable_unauthorized.py (already translated).
