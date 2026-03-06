"""Uniswap V4 Hooks library — adapted from Hooks.t.sol

Tests for:
- Hooks.hasPermission(address self, uint256 flag) -> bool  (Helper32)
- Hooks.isValidHookAddress(address self, uint64 fee) -> bool  (Helper49)
- Hooks.beforeInitialize(address self, PoolKey key, uint256 sqrtPriceX96) -> void  (Helper43)

Permission flags live in the lowest bits of the hook address.
"""
import pytest
from helpers import grouped_call
import algokit_utils as au
from algosdk import encoding

# ─── Hooks permission flags ───────────────────────────────────────────────────

BEFORE_INITIALIZE_FLAG            = 1 << 13   # 0x2000
AFTER_INITIALIZE_FLAG             = 1 << 12   # 0x1000
BEFORE_ADD_LIQUIDITY_FLAG         = 1 << 11   # 0x0800
BEFORE_SWAP_FLAG                  = 1 << 7    # 0x0080
AFTER_SWAP_FLAG                   = 1 << 6    # 0x0040
BEFORE_SWAP_RETURNS_DELTA_FLAG    = 1 << 3    # 0x0008
AFTER_SWAP_RETURNS_DELTA_FLAG     = 1 << 2    # 0x0004

DYNAMIC_FEE_FLAG = 0x800000


# ─── Helpers ──────────────────────────────────────────────────────────────────

def make_hook_address(uint160_value: int) -> str:
    """Convert a uint160 flag value to an Algorand address string.
    The 20-byte value is left-padded to 32 bytes for the Algorand public key."""
    pk = (b'\x00' * 12) + uint160_value.to_bytes(20, "big")
    return encoding.encode_address(pk)


ZERO_ADDRESS = encoding.encode_address(bytes(32))


def make_pool_key(hooks_addr=None):
    """Return a minimal PoolKey tuple: (currency0, currency1, fee, tickSpacing, hooks).
    PoolKey fields are uint8[32] (not address type) in the ARC56 spec."""
    h = hooks_addr if hooks_addr is not None else [0]*32
    return [[0]*32, [0]*32, 0, 0, h]


# ─── hasPermission tests (Helper32) ──────────────────────────────────────────

@pytest.mark.localnet
def test_hasPermission_zero_address_returns_false(helper32, orchestrator, algod_client, account):
    """Zero address has no permission bits set."""
    r = grouped_call(helper32, "Hooks.hasPermission", [ZERO_ADDRESS, BEFORE_INITIALIZE_FLAG], orchestrator, algod_client, account)
    assert r == 0


@pytest.mark.localnet
def test_hasPermission_before_initialize_flag_set(helper32, orchestrator, algod_client, account):
    """Address with BEFORE_INITIALIZE bit set returns True."""
    addr = make_hook_address(BEFORE_INITIALIZE_FLAG)
    r = grouped_call(helper32, "Hooks.hasPermission", [addr, BEFORE_INITIALIZE_FLAG], orchestrator, algod_client, account)
    assert r != 0


@pytest.mark.localnet
def test_hasPermission_after_initialize_flag_set(helper32, orchestrator, algod_client, account):
    """Address with AFTER_INITIALIZE bit returns True."""
    addr = make_hook_address(AFTER_INITIALIZE_FLAG)
    r = grouped_call(helper32, "Hooks.hasPermission", [addr, AFTER_INITIALIZE_FLAG], orchestrator, algod_client, account)
    assert r != 0


@pytest.mark.localnet
def test_hasPermission_flag_not_set_returns_false(helper32, orchestrator, algod_client, account):
    """Address with only BEFORE_INITIALIZE returns False for AFTER_INITIALIZE."""
    addr = make_hook_address(BEFORE_INITIALIZE_FLAG)
    r = grouped_call(helper32, "Hooks.hasPermission", [addr, AFTER_INITIALIZE_FLAG], orchestrator, algod_client, account)
    assert r == 0


@pytest.mark.localnet
def test_hasPermission_multiple_flags_set(helper32, orchestrator, algod_client, account):
    """Address with multiple flags returns True for each."""
    addr = make_hook_address(BEFORE_INITIALIZE_FLAG | BEFORE_SWAP_FLAG | AFTER_SWAP_FLAG)
    for flag in (BEFORE_INITIALIZE_FLAG, BEFORE_SWAP_FLAG, AFTER_SWAP_FLAG):
        r = grouped_call(helper32, "Hooks.hasPermission", [addr, flag], orchestrator, algod_client, account)
        assert r != 0, f"expected True for flag {flag:#x}"


@pytest.mark.localnet
def test_hasPermission_before_swap_flag(helper32, orchestrator, algod_client, account):
    """Address with BEFORE_SWAP bit returns True."""
    addr = make_hook_address(BEFORE_SWAP_FLAG)
    r = grouped_call(helper32, "Hooks.hasPermission", [addr, BEFORE_SWAP_FLAG], orchestrator, algod_client, account)
    assert r != 0


# ─── isValidHookAddress tests (Helper49) ─────────────────────────────────────

@pytest.mark.localnet
def test_isValidHookAddress_zero_address_static_fee(helper49, orchestrator, algod_client, account):
    """Zero address is valid with static fee."""
    r = grouped_call(helper49, "Hooks.isValidHookAddress", [ZERO_ADDRESS, 3000], orchestrator, algod_client, account)
    assert r != 0


@pytest.mark.localnet
def test_isValidHookAddress_zero_address_dynamic_fee_invalid(helper49, orchestrator, algod_client, account):
    """Zero address with dynamic fee flag is invalid."""
    r = grouped_call(helper49, "Hooks.isValidHookAddress", [ZERO_ADDRESS, DYNAMIC_FEE_FLAG], orchestrator, algod_client, account)
    assert r == 0


@pytest.mark.localnet
def test_isValidHookAddress_hook_with_permission_bits_valid(helper49, orchestrator, algod_client, account):
    """Non-zero hook with at least one permission flag is valid."""
    addr = make_hook_address(BEFORE_INITIALIZE_FLAG)
    r = grouped_call(helper49, "Hooks.isValidHookAddress", [addr, 3000], orchestrator, algod_client, account)
    assert r != 0


@pytest.mark.localnet
def test_isValidHookAddress_delta_without_action_invalid(helper49, orchestrator, algod_client, account):
    """BEFORE_SWAP_RETURNS_DELTA without BEFORE_SWAP is invalid."""
    addr = make_hook_address(BEFORE_SWAP_RETURNS_DELTA_FLAG)
    r = grouped_call(helper49, "Hooks.isValidHookAddress", [addr, 3000], orchestrator, algod_client, account)
    assert r == 0


@pytest.mark.localnet
def test_isValidHookAddress_delta_with_action_valid(helper49, orchestrator, algod_client, account):
    """BEFORE_SWAP + BEFORE_SWAP_RETURNS_DELTA together is valid."""
    addr = make_hook_address(BEFORE_SWAP_FLAG | BEFORE_SWAP_RETURNS_DELTA_FLAG)
    r = grouped_call(helper49, "Hooks.isValidHookAddress", [addr, 3000], orchestrator, algod_client, account)
    assert r != 0


# ─── beforeInitialize tests (Helper43) ───────────────────────────────────────

@pytest.mark.localnet
def test_beforeInitialize_zero_address_is_noop(helper43, orchestrator, algod_client, account):
    """Zero-address hook is a no-op (no external call)."""
    key = make_pool_key()
    grouped_call(helper43, "Hooks.beforeInitialize", [ZERO_ADDRESS, key, 79228162514264337593543950336], orchestrator, algod_client, account)
