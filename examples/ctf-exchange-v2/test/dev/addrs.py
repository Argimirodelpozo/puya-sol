"""Address conversion helpers between AVM addresses and app ids."""
from algosdk import encoding


def addr(account_or_sk) -> bytes:
    """Return the 32-byte raw address for an algokit/algosdk account."""
    return encoding.decode_address(account_or_sk.address)


def algod_addr_for_app(app_id: int) -> str:
    """The base32 address algod uses to fund an application."""
    return encoding.encode_address(
        encoding.checksum(b"appID" + app_id.to_bytes(8, "big")))


def app_id_to_address(app_id: int) -> bytes:
    """puya-sol's address convention: 24 zero bytes + itob(app_id) → 32 bytes.

    Used wherever a Solidity `address` must reference another deployed app
    (constructor args, fixture inputs, etc).
    """
    return b"\x00" * 24 + app_id.to_bytes(8, "big")


def algod_addr_bytes_for_app(app_id: int) -> bytes:
    """The 32-byte raw address algod uses for an application's account.
    This is what `msg.sender` resolves to inside inner-call execution
    (where the app is the caller). For txn-side allowance/approval
    lookups that match those inner calls, use *this* — not
    `app_id_to_address`."""
    return encoding.checksum(b"appID" + app_id.to_bytes(8, "big"))
