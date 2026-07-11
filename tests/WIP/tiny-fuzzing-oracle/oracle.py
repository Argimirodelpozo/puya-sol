"""EVM-semantics reference model for the codec_probe.sol fixture.

No EVM is run — each function returns the value the EVM would produce for the
matching Solidity function, computed directly in Python. The fuzzer diffs the AVM
result against these. `REVERT` is a sentinel for a checked-arithmetic / div-by-zero
trap. All functions here are the *spec*; a mismatch means the AVM diverged.

This is the cheap spike form (no py-evm). If it pays off, the same diff loop swaps
this module for a live evmone/py-evm oracle to cover arbitrary contracts.
"""

REVERT = "REVERT"
U256 = 1 << 256
I256_MIN = -(1 << 255)
I256_MAX = (1 << 255) - 1


def trunc_signed(x: int, bits: int) -> int:
    """int256(intN(x)): take the low N bits, interpret two's-complement signed."""
    v = x & ((1 << bits) - 1)
    if v & (1 << (bits - 1)):
        v -= 1 << bits
    return v


def trunc_unsigned(x: int, bits: int) -> int:
    return x & ((1 << bits) - 1)


# --- sub-word truncate + widen (intN sign-extension class) ---
def int24RT(x):  return trunc_signed(x, 24)
def int8RT(x):   return trunc_signed(x, 8)
def int128RT(x): return trunc_signed(x, 128)
def uint24RT(x): return trunc_unsigned(x, 24)
def abiRTInt128(x): return trunc_signed(x, 128)  # round-trip is identity on int128


# --- checked 256-bit arithmetic ---
def addU256(a, b):
    s = a + b
    return s if s < U256 else REVERT


def subU256(a, b):
    return a - b if a >= b else REVERT


def mulU256(a, b):
    p = a * b
    return p if p < U256 else REVERT


def addU8(a, b):
    # uint8(a) + uint8(b) is CHECKED uint8 arithmetic → reverts when sum > 255.
    s = (a & 0xFF) + (b & 0xFF)
    return s if s <= 0xFF else REVERT


# --- signed division / modulo (EVM: truncate toward zero) ---
def divI256(a, b):
    if b == 0:
        return REVERT
    if a == I256_MIN and b == -1:  # checked overflow
        return REVERT
    q = abs(a) // abs(b)
    return -q if (a < 0) != (b < 0) else q


def modI256(a, b):
    if b == 0:
        return REVERT
    # EVM smod: result takes the sign of the dividend.
    r = abs(a) % abs(b)
    return -r if a < 0 else r


# --- shifts (Solidity 0.8: `>>` on a signed type is arithmetic = EVM sar) ---
def shlU256(x, s):
    return (x << s) & (U256 - 1) if s < 256 else 0


def shrU256(x, s):
    return x >> s if s < 256 else 0


def sarI256(x, s):
    # Python `>>` on a signed int floors toward -inf, exactly like EVM sar; for
    # s>=256 it already saturates to 0 (x>=0) or -1 (x<0).
    return x >> s if s < 256 else (-1 if x < 0 else 0)


# --- addmod / mulmod: intermediate is 512-bit; m==0 REVERTS ---
# NB: the raw EVM ADDMOD/MULMOD opcode returns 0 for m==0, but the Solidity
# builtin inserts `assert(k != 0)` (since 0.5.0) → Panic(0x12). We model the AVM,
# which compiles *Solidity*, so the oracle must follow Solidity, not the opcode.
# (This row was the spike's first lesson: model the language, not the VM.)
def addmodU(a, b, m):
    return REVERT if m == 0 else (a + b) % m


def mulmodU(a, b, m):
    return REVERT if m == 0 else (a * b) % m


# --- checked exponentiation ---
def expU(b, e):
    if e == 0:
        return 1            # x**0 == 1 (incl. 0**0)
    if b <= 1:
        return b            # 0**e==0 (e>0), 1**e==1
    if e >= 256:
        return REVERT       # b>=2 ⇒ b**256 >= 2**256, overflow
    p = pow(b, e)
    return p if p < U256 else REVERT


# --- unchecked wrapping add ---
def uncheckedAdd(a, b):
    return (a + b) & (U256 - 1)


# Map ABI method signature → oracle fn and the bit-width of each return for sanity.
ORACLE = {
    "int24RT(int256)": int24RT,
    "int8RT(int256)": int8RT,
    "int128RT(int256)": int128RT,
    "uint24RT(uint256)": uint24RT,
    "addU256(uint256,uint256)": addU256,
    "subU256(uint256,uint256)": subU256,
    "mulU256(uint256,uint256)": mulU256,
    "addU8(uint256,uint256)": addU8,
    "divI256(int256,int256)": divI256,
    "modI256(int256,int256)": modI256,
    "abiRTInt128(int256)": abiRTInt128,
    "shlU256(uint256,uint256)": shlU256,
    "shrU256(uint256,uint256)": shrU256,
    "sarI256(int256,uint256)": sarI256,
    "addmodU(uint256,uint256,uint256)": addmodU,
    "mulmodU(uint256,uint256,uint256)": mulmodU,
    "expU(uint256,uint256)": expU,
    "uncheckedAdd(uint256,uint256)": uncheckedAdd,
}
