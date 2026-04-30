"""v2 Order struct utilities.

Mirrors `src/exchange/libraries/Structs.sol::Order`:

    struct Order {
        uint256 salt;
        address maker;
        address signer;
        uint256 tokenId;
        uint256 makerAmount;
        uint256 takerAmount;
        Side side;              // enum (uint8)
        SignatureType signatureType; // enum (uint8)
        uint256 timestamp;
        bytes32 metadata;
        bytes32 builder;
        bytes signature;
    }

`make_order_tuple(...)` returns the order as a Python tuple in the field
order the ABI expects so it can be passed straight to `AppClient.send.call`.

The struct hash uses `keccak256(abi.encode(_ORDER_TYPEHASH, salt, maker,
signer, tokenId, makerAmount, takerAmount, side, signatureType, timestamp,
metadata, builder))`. Per the AVM port adaptation (`src/exchange/mixins/
Hashing.sol`), this matches the contract's `_createStructHash` exactly.

Wiring the struct hash into the contract's full `hashOrder` (which feeds
into `_hashTypedData` with the cached EIP-712 domain) needs the
contract's `eip712Domain()` view — done lazily in tests when an order
needs to be signed against a specific deployed exchange.
"""
from dataclasses import dataclass, field
from enum import IntEnum

from eth_utils import keccak
from eth_abi import encode as abi_encode


class Side(IntEnum):
    BUY = 0
    SELL = 1


class SignatureType(IntEnum):
    EOA = 0
    POLY_PROXY = 1
    POLY_GNOSIS_SAFE = 2
    POLY_1271 = 3


# Computed offline:
#   keccak256("Order(uint256 salt,address maker,address signer,uint256 tokenId,
#             uint256 makerAmount,uint256 takerAmount,uint8 side,
#             uint8 signatureType,uint256 timestamp,bytes32 metadata,bytes32 builder)")
ORDER_TYPEHASH = keccak(
    b"Order(uint256 salt,address maker,address signer,uint256 tokenId,"
    b"uint256 makerAmount,uint256 takerAmount,uint8 side,uint8 signatureType,"
    b"uint256 timestamp,bytes32 metadata,bytes32 builder)"
)


@dataclass
class Order:
    """Plain-Python Order. Addresses are 20-byte eth-style; convert to 32-byte
    AVM form via `dev.addrs.app_id_to_address` or zero-pad when constructing
    test tuples (the orch ABI expects 32-byte addresses)."""
    salt: int = 1
    maker: bytes = b"\x00" * 20
    signer: bytes = b"\x00" * 20
    tokenId: int = 0
    makerAmount: int = 0
    takerAmount: int = 0
    side: Side = Side.BUY
    signatureType: SignatureType = SignatureType.EOA
    timestamp: int = 0
    metadata: bytes = b"\x00" * 32
    builder: bytes = b"\x00" * 32
    signature: bytes = b""

    def struct_hash(self) -> bytes:
        """Compute the EIP-712 struct hash for this order (32 bytes).

        Mirrors `Hashing._createStructHash` after the AVM port adaptation
        (`keccak256(abi.encode(...))`).
        """
        return keccak(
            abi_encode(
                [
                    "bytes32",  # ORDER_TYPEHASH
                    "uint256",  # salt
                    "address",  # maker (20 bytes)
                    "address",  # signer
                    "uint256",  # tokenId
                    "uint256",  # makerAmount
                    "uint256",  # takerAmount
                    "uint8",    # side
                    "uint8",    # signatureType
                    "uint256",  # timestamp
                    "bytes32",  # metadata
                    "bytes32",  # builder
                ],
                [
                    ORDER_TYPEHASH,
                    self.salt,
                    self.maker,
                    self.signer,
                    self.tokenId,
                    self.makerAmount,
                    self.takerAmount,
                    int(self.side),
                    int(self.signatureType),
                    self.timestamp,
                    self.metadata,
                    self.builder,
                ],
            )
        )

    def to_abi_tuple(self) -> tuple:
        """Pack as a tuple of native Python types. Addresses are 32 raw
        bytes. Used for puya-sol semantics where bytes-typed args go
        through directly. Most tests want `to_abi_list()` instead — the
        algokit composer expects `uint8[32]` to come in as `list[int]`.
        """
        def addr32(a: bytes) -> bytes:
            if len(a) == 32:
                return a
            if len(a) == 20:
                return b"\x00" * 12 + a
            raise ValueError(f"address must be 20 or 32 bytes, got {len(a)}")

        return (
            self.salt,
            addr32(self.maker),
            addr32(self.signer),
            self.tokenId,
            self.makerAmount,
            self.takerAmount,
            int(self.side),
            int(self.signatureType),
            self.timestamp,
            self.metadata,
            self.builder,
            self.signature,
        )

    def to_abi_list(self) -> list:
        """Pack as a list, in the shape `algokit_utils.AppClient.send.call`
        wants for the v2 Order ABI shape. Addresses + bytes32 fields
        become `list[int]` (one byte each); the dynamic `signature` stays
        as raw bytes."""
        def addr32_as_ints(a: bytes) -> list:
            if len(a) == 20:
                a = b"\x00" * 12 + a
            if len(a) != 32:
                raise ValueError(f"address must be 20 or 32 bytes, got {len(a)}")
            return list(a)

        return [
            self.salt,
            addr32_as_ints(self.maker),
            addr32_as_ints(self.signer),
            self.tokenId,
            self.makerAmount,
            self.takerAmount,
            int(self.side),
            int(self.signatureType),
            self.timestamp,
            list(self.metadata) if len(self.metadata) == 32 else [0] * 32,
            list(self.builder) if len(self.builder) == 32 else [0] * 32,
            self.signature,
        ]


def make_order(
    *,
    salt: int = 1,
    maker: bytes,
    signer: bytes | None = None,
    token_id: int = 0,
    maker_amount: int = 0,
    taker_amount: int = 0,
    side: Side = Side.BUY,
    signature_type: SignatureType = SignatureType.EOA,
    timestamp: int = 0,
) -> Order:
    """Build an unsigned `Order`. `signer` defaults to `maker` (EOA pattern)."""
    return Order(
        salt=salt,
        maker=maker,
        signer=signer if signer is not None else maker,
        tokenId=token_id,
        makerAmount=maker_amount,
        takerAmount=taker_amount,
        side=side,
        signatureType=signature_type,
        timestamp=timestamp,
    )


def hash_order_via_contract(client, order: Order, *, extra_fee: int = 50_000) -> bytes:
    """Call `client.hashOrder(order)` on the deployed exchange and return
    the canonical 32-byte hash. Uses the contract's domain separator and
    typehash, so we don't have to re-implement EIP-712 in Python.

    `client` is an `algokit_utils.AppClient` for the orch.
    """
    import algokit_utils as au
    res = client.send.call(
        au.AppClientMethodCallParams(
            method="hashOrder", args=[order.to_abi_list()],
            extra_fee=au.AlgoAmount(micro_algo=extra_fee),
        ),
    ).abi_return
    if isinstance(res, str):
        # algokit decodes byte[32] as a hex string (0x...).
        res = bytes.fromhex(res[2:] if res.startswith("0x") else res)
    return bytes(res) if not isinstance(res, bytes) else res


def sign_order(client, order: Order, signer) -> Order:
    """Hash `order` via the contract, sign with `signer` (EthSigner), and
    return a new Order with the 65-byte `r ‖ s ‖ v` signature filled in.

    `client` — orch AppClient.
    `signer` — `dev.signing.EthSigner`.
    """
    h = hash_order_via_contract(client, order)
    sig = signer.sign_hash(h)
    return Order(
        salt=order.salt,
        maker=order.maker,
        signer=order.signer,
        tokenId=order.tokenId,
        makerAmount=order.makerAmount,
        takerAmount=order.takerAmount,
        side=order.side,
        signatureType=order.signatureType,
        timestamp=order.timestamp,
        metadata=order.metadata,
        builder=order.builder,
        signature=sig,
    )


def hash_typed_data(struct_hash: bytes, domain_separator: bytes) -> bytes:
    """Compose the final EIP-712 hash: keccak256("\\x19\\x01" ++ ds ++ struct).

    Use the contract's `eip712Domain()` view to get the actual domain
    separator at sign time — solady caches it but the reachable view
    returns the same canonical value.
    """
    if len(domain_separator) != 32:
        raise ValueError("domain_separator must be 32 bytes")
    if len(struct_hash) != 32:
        raise ValueError("struct_hash must be 32 bytes")
    return keccak(b"\x19\x01" + domain_separator + struct_hash)
