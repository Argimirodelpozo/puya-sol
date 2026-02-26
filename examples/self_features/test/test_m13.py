"""
M13: SelfVerificationRoot abstract contract integration tests.
Exercises: abstract contract inheritance, abi.encodePacked with mixed types
(uint8, bytes31, uint256, bytes), bytes.concat, msg.sender check, cross-library
integration, constructor inheritance, state from base + derived classes.
"""

import pytest
import algokit_utils as au
from algokit_utils.models.account import SigningAccount

from conftest import deploy_contract


@pytest.fixture(scope="module")
def root_client(
    localnet: au.AlgorandClient, account: SigningAccount
) -> au.AppClient:
    # Constructor needs hub address (32 bytes) — use zero address for localnet
    hub_address = bytes(32)
    return deploy_contract(
        localnet, account, "SelfVerificationRootTest",
        app_args=[hub_address],
    )


def to_bytes(val) -> bytes:
    """Convert ABI byte[] return (list of ints) to bytes."""
    if isinstance(val, (bytes, bytearray)):
        return bytes(val)
    if isinstance(val, list):
        return bytes(val)
    return val


# --- Scope and state tests ---


@pytest.mark.localnet
def test_scope_is_zero(root_client: au.AppClient) -> None:
    """scope() should return 0 (Poseidon not available on localnet)."""
    result = root_client.send.call(
        au.AppClientMethodCallParams(method="testScope")
    )
    assert result.abi_return == 0


@pytest.mark.localnet
def test_scope_from_base(root_client: au.AppClient) -> None:
    """scope() is inherited from SelfVerificationRoot base."""
    result = root_client.send.call(
        au.AppClientMethodCallParams(method="scope")
    )
    assert result.abi_return == 0


# --- testBuildBaseInput (abi.encodePacked with mixed types) ---


@pytest.mark.localnet
def test_build_base_input_empty_payload(root_client: au.AppClient) -> None:
    """abi.encodePacked(uint8(2), bytes31(0), scope, proofPayload) with empty payload."""
    scope_val = 0
    proof_payload = b""
    result = root_client.send.call(
        au.AppClientMethodCallParams(
            method="testBuildBaseInput",
            args=[proof_payload],
        )
    )
    # Expected: 1 byte (2) + 31 zero bytes + 32 bytes scope (0) + 0 bytes proof
    expected = bytes([2]) + b"\x00" * 31 + scope_val.to_bytes(32, "big")
    assert to_bytes(result.abi_return) == expected


@pytest.mark.localnet
def test_build_base_input_with_payload(root_client: au.AppClient) -> None:
    """abi.encodePacked with proof data appended."""
    proof_payload = b"\xde\xad\xbe\xef"
    result = root_client.send.call(
        au.AppClientMethodCallParams(
            method="testBuildBaseInput",
            args=[proof_payload],
        )
    )
    # scope is 0 on localnet
    expected = bytes([2]) + b"\x00" * 31 + (0).to_bytes(32, "big") + proof_payload
    assert to_bytes(result.abi_return) == expected


# --- testBytesConcat ---


@pytest.mark.localnet
def test_bytes_concat(root_client: au.AppClient) -> None:
    result = root_client.send.call(
        au.AppClientMethodCallParams(
            method="testBytesConcat",
            args=[b"\xde\xad", b"\xbe\xef"],
        )
    )
    assert to_bytes(result.abi_return) == b"\xde\xad\xbe\xef"


@pytest.mark.localnet
def test_bytes_concat_empty(root_client: au.AppClient) -> None:
    result = root_client.send.call(
        au.AppClientMethodCallParams(
            method="testBytesConcat",
            args=[b"", b"\xab\xcd"],
        )
    )
    assert to_bytes(result.abi_return) == b"\xab\xcd"


# --- testConfigId (keccak256 + abi.encodePacked with bytes32) ---


@pytest.mark.localnet
def test_config_id(root_client: au.AppClient) -> None:
    chain_id = (1).to_bytes(32, "big")
    user_id = (12345).to_bytes(32, "big")
    result = root_client.send.call(
        au.AppClientMethodCallParams(
            method="testConfigId",
            args=[chain_id, user_id],
        )
    )
    from Crypto.Hash import keccak
    k = keccak.new(digest_bits=256)
    k.update(chain_id + user_id)
    expected = k.digest()
    assert to_bytes(result.abi_return) == expected


# --- getConfigId (overridden virtual function from base) ---


@pytest.mark.localnet
def test_get_config_id_override(root_client: au.AppClient) -> None:
    """getConfigId is virtual in base, overridden in derived. Test the override."""
    chain_id = (42).to_bytes(32, "big")
    user_id = (999).to_bytes(32, "big")
    dummy_data = b""
    result = root_client.send.call(
        au.AppClientMethodCallParams(
            method="getConfigId",
            args=[chain_id, user_id, dummy_data],
        )
    )
    from Crypto.Hash import keccak
    k = keccak.new(digest_bits=256)
    k.update(chain_id + user_id)
    expected = k.digest()
    assert to_bytes(result.abi_return) == expected


# --- Cross-library integration ---


@pytest.mark.localnet
def test_passport_older_than(root_client: au.AppClient) -> None:
    charcodes = bytearray(93)
    charcodes[88] = ord("2")  # olderThanStart for E_PASSPORT
    charcodes[89] = ord("5")
    result = root_client.send.call(
        au.AppClientMethodCallParams(
            method="testPassportOlderThan",
            args=[bytes(charcodes)],
        )
    )
    assert result.abi_return == 25


@pytest.mark.localnet
def test_extract_string(root_client: au.AppClient) -> None:
    charcodes = b"\x00\x00HELLO\x00\x00"
    result = root_client.send.call(
        au.AppClientMethodCallParams(
            method="testExtractString",
            args=[charcodes, 2, 6],
        )
    )
    assert result.abi_return == "HELLO"


# --- SelfUtils.stringToBigInt ---


@pytest.mark.localnet
def test_string_to_big_int(root_client: au.AppClient) -> None:
    s = "Hello"
    expected = 0
    for c in s.encode("ascii"):
        expected = (expected << 8) | c
    result = root_client.send.call(
        au.AppClientMethodCallParams(
            method="testStringToBigInt",
            args=[s],
        )
    )
    assert result.abi_return == expected


# --- Formatter.substring ---


@pytest.mark.localnet
def test_substring(root_client: au.AppClient) -> None:
    result = root_client.send.call(
        au.AppClientMethodCallParams(
            method="testSubstring",
            args=["Hello World", 0, 5],
        )
    )
    assert result.abi_return == "Hello"


@pytest.mark.localnet
def test_substring_middle(root_client: au.AppClient) -> None:
    result = root_client.send.call(
        au.AppClientMethodCallParams(
            method="testSubstring",
            args=["Hello World", 6, 11],
        )
    )
    assert result.abi_return == "World"
