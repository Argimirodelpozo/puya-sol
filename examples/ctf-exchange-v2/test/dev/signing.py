"""EOA ECDSA signing helpers.

The v2 orchestrator's `_verifyECDSASignature` (after the AVM workaround in
src/exchange/mixins/Signatures.sol) expects a 65-byte signature laid out
as `r ‖ s ‖ v` and recovers the signer via the AVM `ecdsa_pk_recover`
opcode (the EVM ecrecover precompile shape). This matches Foundry's
`vm.sign(pk, hash)` output, so we mirror that on the Python side using
`eth_account`.

Foundry test references use deterministic private keys (`uint256(0xB0B)`,
`uint256(0xCA414)`) and read addresses via `vm.addr(pk)`. We do the same:
private keys are 32-byte big-endian integers, `EthSigner.from_pk_int(int)`
gives a signer object whose `.eth_address` matches `vm.addr(int)` exactly.
"""
from dataclasses import dataclass

from eth_account import Account


# Foundry's bobPK / carlaPK in BaseExchangeTest.
BOB_PK = 0xB0B
CARLA_PK = 0xCA414
# Dave appears in Preapproved.t.sol::test_matchOrders_preapproved_partialFill
# as a third-party SELL maker filling the remainder of bob's BUY.
DAVE_PK = 0xDA7E


@dataclass(frozen=True)
class EthSigner:
    """An eth-style EOA. Holds a 32-byte private key + derived address.

    `.eth_address` is the 20-byte Ethereum address used as `maker`/`signer`
    in Order structs. `.eth_address_padded32` is the same address
    left-padded with 12 zero bytes so it slots into AVM's 32-byte
    address representation.
    """
    pk_int: int  # 32-byte big-endian private key as a Python int

    @property
    def pk_bytes(self) -> bytes:
        return self.pk_int.to_bytes(32, "big")

    @property
    def _account(self):
        return Account.from_key(self.pk_bytes)

    @property
    def eth_address(self) -> bytes:
        """20-byte Ethereum address (`vm.addr(pk)` equivalent)."""
        return bytes.fromhex(self._account.address[2:])

    @property
    def eth_address_padded32(self) -> bytes:
        """20-byte eth address with 12 leading zero bytes for 32-byte slot."""
        return b"\x00" * 12 + self.eth_address

    def sign_hash(self, hash32: bytes) -> bytes:
        """Sign a 32-byte EIP-712 hash, return `r ‖ s ‖ v` (65 bytes).

        Uses `Account.unsafe_sign_hash` so no Ethereum message prefix is
        applied — the orchestrator hashes the EIP-712 struct itself and
        then verifies against the raw signature, which is exactly what
        Foundry's `vm.sign(pk, hash)` does.
        """
        if len(hash32) != 32:
            raise ValueError(f"hash must be 32 bytes, got {len(hash32)}")
        signed = Account.unsafe_sign_hash(hash32, private_key=self.pk_bytes)
        return (
            signed.r.to_bytes(32, "big")
            + signed.s.to_bytes(32, "big")
            + bytes([signed.v])
        )

    @classmethod
    def from_pk_int(cls, pk_int: int) -> "EthSigner":
        return cls(pk_int=pk_int)


# Convenience: pre-built signers matching Foundry's bob/carla.
def bob() -> EthSigner:
    return EthSigner.from_pk_int(BOB_PK)


def carla() -> EthSigner:
    return EthSigner.from_pk_int(CARLA_PK)


def dave() -> EthSigner:
    return EthSigner.from_pk_int(DAVE_PK)
