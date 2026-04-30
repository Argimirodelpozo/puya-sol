"""EIP-712 typed-data signing helper for the AVM-port'd PermissionedRamp.

Solady's EIP712 builds the domainSeparator as:
  keccak256(typeHash || nameHash || versionHash || chainId || address())
where `address()` writes 32 bytes to memory. On EVM this is `12 zeros + 20-byte addr`;
on AVM it's the **full 32-byte AVM address** (no zero-padding).

Likewise `keccak256(abi.encode(typehash, msg.sender, asset, to, amount, nonce, deadline))`
encodes each address as 32 bytes — on AVM that's the full 32-byte addr passed/stored.

So off-chain test signing must use 32-byte address forms (matching whatever the caller
passes in the ApplicationArgs), NOT 20-byte EVM forms.
"""
from typing import Tuple

from eth_account import Account
from eth_utils import keccak


_DOMAIN_TYPE_STRING = (
    b"EIP712Domain(string name,string version,uint256 chainId,"
    b"address verifyingContract)"
)


def domain_separator(
    name: str, version: str, chain_id: int, verifying_contract32: bytes
) -> bytes:
    """Compute EIP-712 domainSeparator using AVM 32-byte address layout.

    `verifying_contract32` must be exactly 32 bytes — the algod-derived
    address of the verifying contract (= what the contract reads from
    `global CurrentApplicationAddress`).
    """
    if len(verifying_contract32) != 32:
        raise ValueError(f"verifying_contract32 must be 32 bytes, got {len(verifying_contract32)}")
    type_hash = keccak(_DOMAIN_TYPE_STRING)
    name_hash = keccak(name.encode())
    version_hash = keccak(version.encode())
    encoded = (
        type_hash
        + name_hash
        + version_hash
        + chain_id.to_bytes(32, "big")
        + verifying_contract32
    )
    return keccak(encoded)


def struct_hash(
    typehash: bytes, sender32: bytes, asset32: bytes, to32: bytes,
    amount: int, nonce: int, deadline: int
) -> bytes:
    """keccak256(abi.encode(typehash, sender, asset, to, amount, nonce, deadline)).

    All address slots are passed through as their 32-byte AVM
    representation (matching how the contract receives them in
    ApplicationArgs and re-emits via abi.encode).
    """
    for name, b in [("sender", sender32), ("asset", asset32), ("to", to32)]:
        if len(b) != 32:
            raise ValueError(f"{name}32 must be 32 bytes, got {len(b)}")
    if len(typehash) != 32:
        raise ValueError("typehash must be 32 bytes")
    return keccak(
        typehash
        + sender32
        + asset32
        + to32
        + amount.to_bytes(32, "big")
        + nonce.to_bytes(32, "big")
        + deadline.to_bytes(32, "big")
    )


def digest(domain_sep: bytes, struct_h: bytes) -> bytes:
    """EIP-712 digest = keccak("\\x19\\x01" || domainSeparator || structHash).

    AVM-PORT-ADAPTATION: puya-sol lowers Solady's `keccak256(0x18, 0x42)`
    (66 bytes) as `extract 24 64` (64 bytes), truncating the last 2 bytes
    of `structHash`. To match what the contract actually computes, we
    drop the last 2 bytes of `struct_h` here. Once the puya-sol length
    bug is fixed, drop the slice.
    """
    return keccak(b"\x19\x01" + domain_sep + struct_h[:-2])


def sign_digest(digest32: bytes, pk_int: int) -> bytes:
    """Sign a 32-byte digest, return r||s||v (65 bytes)."""
    if len(digest32) != 32:
        raise ValueError("digest must be 32 bytes")
    pk = pk_int.to_bytes(32, "big")
    signed = Account.unsafe_sign_hash(digest32, private_key=pk)
    return (
        signed.r.to_bytes(32, "big")
        + signed.s.to_bytes(32, "big")
        + bytes([signed.v])
    )


WRAP_TYPEHASH = keccak(
    b"Wrap(address sender,address asset,address to,uint256 amount,"
    b"uint256 nonce,uint256 deadline)"
)
UNWRAP_TYPEHASH = keccak(
    b"Unwrap(address sender,address asset,address to,uint256 amount,"
    b"uint256 nonce,uint256 deadline)"
)


def _to_32(v) -> bytes:
    """Coerce ABI return value to 32 bytes."""
    if isinstance(v, bytes):
        return v
    if isinstance(v, str):
        from algosdk.encoding import decode_address
        if len(v) == 58:
            return decode_address(v)
        # Hex string?
        if v.startswith("0x"):
            return bytes.fromhex(v[2:])
        return v.encode()  # last resort
    if isinstance(v, (list, tuple)):
        return bytes(v)
    raise TypeError(f"can't coerce {type(v)} to 32 bytes: {v!r}")


def read_domain(client) -> Tuple[str, str, int, bytes]:
    """Pull the contract's eip712Domain() and return (name, version, chainId, verifyingContract32).

    AlgoKit decodes Solady's eip712Domain() as a dict with named keys
    (fields, name, version, chainId, verifyingContract, salt, extensions).
    `verifyingContract` is the algod-style base32 string.
    """
    import algokit_utils as au
    res = client.send.call(
        au.AppClientMethodCallParams(method="eip712Domain", args=[])
    ).abi_return
    return (
        res["name"],
        res["version"],
        res["chainId"],
        _to_32(res["verifyingContract"]),
    )


def sign_wrap(
    client, sender32: bytes, asset32: bytes, to32: bytes,
    amount: int, nonce: int, deadline: int, pk_int: int,
) -> bytes:
    """Sign a Wrap struct against the given client's eip712Domain."""
    name, version, chain_id, vc32 = read_domain(client)
    sep = domain_separator(name, version, chain_id, vc32)
    sh = struct_hash(WRAP_TYPEHASH, sender32, asset32, to32, amount, nonce, deadline)
    d = digest(sep, sh)
    return sign_digest(d, pk_int)


def sign_unwrap(
    client, sender32: bytes, asset32: bytes, to32: bytes,
    amount: int, nonce: int, deadline: int, pk_int: int,
) -> bytes:
    """Sign an Unwrap struct against the given client's eip712Domain."""
    name, version, chain_id, vc32 = read_domain(client)
    sep = domain_separator(name, version, chain_id, vc32)
    sh = struct_hash(UNWRAP_TYPEHASH, sender32, asset32, to32, amount, nonce, deadline)
    d = digest(sep, sh)
    return sign_digest(d, pk_int)
