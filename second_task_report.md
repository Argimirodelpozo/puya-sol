# Second Task Report: Uniswap solidity-lib Compilation

## Summary

This report documents the effort to compile the [Uniswap solidity-lib](https://github.com/Uniswap/solidity-lib) library collection through the puya-sol C++ frontend, translating Solidity AST to AWST JSON, and invoking the puya backend to produce TEAL for Algorand.

**Result:** All 10 Solidity contracts successfully generate AWST JSON. 7 out of 10 produce TEAL via the puya backend. The generated TEAL has correct ARC4 routing, type encoding, and method dispatch. However, library function bodies are not inlined (they become recursive self-calls), so the TEAL is structurally correct but not functionally complete. All 135 original Mocha/Waffle tests pass. After porting to Solidity 0.8.x, 110 of 135 tests pass (25 failures are all gas cost assertions due to different compiler optimization).

## Repository

- **Source:** https://github.com/Uniswap/solidity-lib
- **Package:** `@uniswap/lib` v4.0.1-alpha
- **License:** GPL-3.0-or-later
- **Cloned to:** `examples/example2/`

## Contracts Overview

### Libraries (7)

| Library | Description | Solidity Version | Lines |
|---------|-------------|-----------------|-------|
| FullMath | Full 512-bit precision multiply/divide | >=0.4.0 → >=0.8.0 | 57 |
| Babylonian | Integer square root (Babylonian method) | >=0.4.0 → >=0.8.0 | 53 |
| BitMath | Most/least significant bit operations | >=0.5.0 → >=0.8.0 | 85 |
| FixedPoint | Q112x112 fixed-point arithmetic | >=0.4.0 → >=0.8.0 | 146 |
| AddressStringUtil | Address to hex string conversion | >=0.5.0 → >=0.8.0 | 35 |
| SafeERC20Namer | ERC20 name/symbol extraction | >=0.5.0 → >=0.8.0 | 94 |
| TransferHelper | Safe ERC20 token transfers | >=0.6.0 → >=0.8.0 | 51 |

### Test Contracts (10)

| Contract | Libraries Used | Echidna? |
|----------|---------------|----------|
| FullMathTest | FullMath | No |
| BabylonianTest | Babylonian | No |
| BitMathTest | BitMath | No |
| FixedPointTest | FixedPoint, FullMath, Babylonian, BitMath | No |
| AddressStringUtilTest | AddressStringUtil | No |
| SafeERC20NamerTest | SafeERC20Namer, AddressStringUtil | No |
| TransferHelperTest | TransferHelper | No |
| FullMathEchidnaTest | FullMath | Yes |
| BabylonianEchidnaTest | Babylonian | Yes |
| BitMathEchidnaTest | BitMath | Yes |

## Build Process

### 1. Solidity Version Compatibility

The Uniswap contracts target Solidity 0.4.0-0.6.12, but our compiler is v0.8.28. A port was required.

**Breaking changes in Solidity 0.8.x that required modifications:**

| Pattern | 0.6.x Syntax | 0.8.x Replacement |
|---------|-------------|-------------------|
| Max value | `uint256(-1)` | `type(uint256).max` |
| Unary negation on unsigned | `-d` | `~d + 1` (in unchecked) |
| Wrapping arithmetic | implicit | `unchecked { ... }` |
| Address to uint | `uint256(addr)` | `uint256(uint160(addr))` |
| Public constructor | `constructor() public` | `constructor()` |
| ABIEncoderV2 | `pragma experimental ABIEncoderV2` | removed (default in 0.8.x) |
| payable transfer | `msg.sender.transfer()` | `payable(msg.sender).transfer()` |

### 2. Port Details

**FullMath.sol** — Most complex port. Three `unchecked` blocks added:
- `fullMul()`: wrapping multiplication `l = x * y`, underflow `h = mm - l`, conditional `h -= 1`
- `fullDiv()`: two's complement negation `d & (~d + 1)` for lowest set bit, Newton-Raphson modular inverse iterations
- `mulDiv()`: underflow handling for `h -= 1` and `l -= mm`

**FixedPoint.sol** — Three functions needed unchecked:
- `mul()`: overflow detection pattern `(z = self._x * y) / y == self._x` wrapped in unchecked (in 0.8.x the multiplication itself reverts before the check)
- `muli()`: absolute value computation `uint256(y < 0 ? -y : y)` wrapped in unchecked (negating `int256.min` overflows in 0.8.x)
- `muluq()`: `type(uint112).max` replacing `uint112(-1)`, etc.

**BitMath.sol** — All `uintN(-1)` patterns → `type(uintN).max`:
- `uint128(-1)` → `type(uint128).max`
- `uint64(-1)` → `type(uint64).max`
- `uint32(-1)` → `type(uint32).max`
- `uint16(-1)` → `type(uint16).max`
- `uint8(-1)` → `type(uint8).max`

**AddressStringUtil.sol** — `uint256(addr)` → `uint256(uint160(addr))`

**SafeERC20NamerTest.sol** — Removed `public` from constructors

**TransferHelperTest.sol** — `msg.sender.transfer()` → `payable(msg.sender).transfer()`

**Echidna tests** — `uint256(-1)` → `type(uint256).max`, `unchecked` for intentional overflow checks

### 3. Waffle Configuration

Updated `node_modules/solc` from 0.6.12 to 0.8.28:
```bash
npm install --save-dev solc@0.8.28
```

Original `waffle.json` unchanged (already points to `./node_modules/solc`).

## Original Test Results

### Pre-port (solc 0.6.12)

```
135 passing (13s)
```

All 135 tests pass with the original contracts and compiler.

### Post-port (solc 0.8.28)

```
110 passing (14s)
25 failing
```

**All 25 failures are gas cost assertions.** No functional/correctness failures. Gas costs differ because:
1. Solidity 0.8.x adds overflow checking on arithmetic operations
2. `unchecked` blocks reduce gas vs checked code but don't match 0.6.x exactly
3. Optimizer produces different output for 0.8.x patterns

Example gas cost differences:
| Function | 0.6.x Gas | 0.8.x Gas | Reason |
|----------|----------|----------|--------|
| BitMath.msb (max uint128) | 367 | 1005 | Overflow checks on comparisons |
| BitMath.lsb (max uint128) | 407 | 1116 | Overflow checks on type().max masks |
| Babylonian.sqrt (max uint) | 798 | 1543 | Overflow checks on shift/divide iterations |
| FixedPoint.sqrt (< 1) | 1173 | 1873 | Additional overflow checking in called libraries |

## Compilation Pipeline Results

### Step 1: Solidity Parsing and Type-Checking

**Status: SUCCESS (all 10 contracts)**

All contracts parse and type-check successfully with v0.8.28 after the port. Import resolution works correctly for relative imports between `contracts/test/` and `contracts/libraries/`.

### Step 2: AWST JSON Generation

**Status: SUCCESS (all 10 contracts)**

| Contract | AWST Nodes | Status |
|----------|-----------|--------|
| FullMathTest | 1 contract | OK |
| BabylonianTest | 1 contract | OK |
| BitMathTest | 1 contract | OK |
| FixedPointTest | 1 contract (5 source units) | OK |
| AddressStringUtilTest | 1 contract | OK |
| SafeERC20NamerTest | 4 contracts | OK |
| TransferHelperTest | 4 contracts | OK |
| FullMathEchidnaTest | 1 contract | OK |
| BabylonianEchidnaTest | 1 contract | OK |
| BitMathEchidnaTest | 1 contract | OK |

All library source units are parsed but skipped during AWST generation ("Skipping library: ..."). Contract nodes are translated with proper ARC4 method configs.

### Step 3: Puya Backend (AWST → TEAL)

**Status: 7 of 10 produce TEAL**

| Contract | TEAL Lines | ARC4 Methods | Status |
|----------|-----------|--------------|--------|
| FullMathTest | 112 | mulDiv, mulDivRoundingUp | TEAL generated |
| BabylonianTest | 64 | sqrt, getGasCostOfSqrt | TEAL generated |
| BitMathTest | 100 | mostSignificantBit, getGasCostOfMostSignificantBit, leastSignificantBit, getGasCostOfLeastSignificantBit | TEAL generated |
| FixedPointTest | 511 | encode, encode144, decode, decode144, mul, muli, muluq, getGasCostOfMuluq, divuq, getGasCostOfDivuq, fraction, getGasCostOfFraction, reciprocal, sqrt, getGasCostOfSqrt | TEAL generated |
| AddressStringUtilTest | 55 | toAsciiString | TEAL generated |
| SafeERC20NamerTest | 78 | tokenSymbol, tokenName | TEAL generated |
| FullMathEchidnaTest | 62 | checkH | TEAL generated (warning: variable h potentially used before assignment) |
| TransferHelperTest | — | — | Deserialization failed (payable/receive features) |
| BabylonianEchidnaTest | — | — | Deserialization failed (assert statement handling) |
| BitMathEchidnaTest | — | — | Unsupported uint64→biguint cast (2**msb exponentiation) |

### What the Generated TEAL Contains

The TEAL correctly implements:
1. **ARC4 method routing** — Method selectors computed from signatures, `match` dispatch
2. **Type encoding** — uint256 mapped to ARC4 uint512 (biguint), struct types as tuples
3. **Argument validation** — Length checks for ARC4-encoded arguments
4. **Return encoding** — ARC4 log return with `0x151f7c75` prefix
5. **Gas cost functions** — `gasleft()` mapped to constant 0 (no Algorand equivalent), returns 0

Example TEAL structure (FullMathTest.mulDivRoundingUp):
```teal
mulDivRoundingUp:
    txna ApplicationArgs 1       // x (uint512)
    ...
    txna ApplicationArgs 2       // y (uint512)
    ...
    txna ApplicationArgs 3       // z (uint512)
    ...
    dig 2
    dig 2
    b*                           // x * y (biguint multiply)
    dig 1
    b%                           // (x * y) % z (biguint modulo)
    pushint 0
    itob
    b>                           // mulmod result > 0
    cover 3
    callsub ...mulDiv            // FullMath.mulDiv(x, y, z)
    swap
    itob
    b+                           // result + (roundUp ? 1 : 0)
    ...
    log                          // ARC4 return
    return
```

### What's Missing: Library Function Inlining

The critical limitation is that **library function bodies are not inlined into the TEAL**. When the frontend encounters a library call like `FullMath.mulDiv(x, y, z)`, it generates a `SubroutineCallExpression` targeting the contract's own `mulDiv` method, creating infinite recursion.

For example, `BabylonianTest.sqrt()`:
```teal
// BabylonianTest.sqrt(num) -> uint256
BabylonianTest.sqrt:
    proto 1 1
    frame_dig -1         // load num
    callsub BabylonianTest.sqrt  // ← recursive self-call! Should be Babylonian.sqrt body
    retsub
```

**What would be needed for functional correctness:**
1. Traverse library ASTs and generate AWST subroutine nodes
2. Map library function calls to these subroutines
3. Handle library function visibility (internal/private)
4. Inline or link the library subroutines in the TEAL output

## New puya-sol Fixes Applied

### 1. `mulmod(x, y, z)` → biguint `(x * y) % z`

The `mulmod` Solidity builtin was falling through to the generic function call handler, producing an unresolvable `InstanceMethodTarget`. Fixed by adding explicit handling that translates to:
```
BigUIntBinaryOperation(op="%",
  left=BigUIntBinaryOperation(op="*", left=x, right=y),
  right=z)
```

This preserves full-precision arithmetic since AVM biguint has no 256-bit limit.

### 2. `addmod(x, y, z)` → biguint `(x + y) % z`

Same pattern as mulmod but with addition.

### 3. `gasleft()` → constant 0

Algorand has no gas metering concept. Mapped to `IntegerConstant(0)` so gas cost functions compile (they'll always return 0).

**Files modified:** `puya-sol-v2/src/builder/ExpressionTranslator.cpp`

## Algorand Tests

**Status: BLOCKED**

Algorand tests cannot be meaningfully written because library function bodies are not present in the TEAL output. The contracts compile to valid TEAL with correct ARC4 routing, but calling any method that depends on library logic (which is all of them) would result in infinite recursion or incorrect results.

**What would pass if library inlining were implemented:**
- All pure math tests (Babylonian, BitMath, FullMath, FixedPoint) — these are ideal AVM candidates since they use only integer arithmetic
- AddressStringUtil tests — address manipulation is pure computation

**What would NOT work on Algorand:**
- SafeERC20Namer tests — use `staticcall` for ERC20 name/symbol extraction (no EVM call equivalent on Algorand)
- TransferHelper tests — use `call` with value for token transfers (no EVM call equivalent)

## Architecture

```
Uniswap solidity-lib (0.6.12 syntax)
    |
    v
[Port to 0.8.x]  (uint256(-1) → type(uint256).max, unchecked blocks, etc.)
    |
    v
[Solidity CompilerStack v0.8.28]
    | parseAndAnalyze()
    v
[Solidity AST (typed)]
    |
    v
[puya-sol-v2 C++ Frontend]
    |-- Libraries: SKIPPED (bodies not translated)
    |-- Contracts: TRANSLATED (functions → AWST methods)
    |-- mulmod/addmod: biguint (x*y)%z / (x+y)%z
    |-- gasleft(): constant 0
    v
awst.json + options.json
    |
    v
[puya backend v5.7.1]
    | deserialization: OK (7/10)
    | code generation: OK (7/10)
    v
TEAL (AVM v10)
    |-- ARC4 routing: CORRECT
    |-- Type encoding: CORRECT (uint256 → uint512 biguint)
    |-- Method signatures: CORRECT
    |-- Library function bodies: MISSING (recursive self-calls)
```

## Files Modified

### puya-sol-v2/src/
- `builder/ExpressionTranslator.cpp` — Added `mulmod`, `addmod`, `gasleft` handling

### puya-sol-v2/examples/example2/
- `contracts/libraries/*.sol` — All 7 ported to Solidity >=0.8.0
- `contracts/test/*.sol` — All 10 ported to Solidity >=0.8.0
- `contracts_original/` — Backup of original 0.6.12 contracts

## Known Limitations

1. **Library inlining:** Library function bodies are skipped during AWST generation. Calls become recursive self-calls in TEAL. This is the primary blocker for functional correctness.

2. **`assert` statement:** The Echidna test pattern `assert(condition)` causes deserialization failures in puya for BabylonianEchidnaTest. The `require` pattern works.

3. **Exponentiation with variable exponents:** `uint256(2)**msb` in BitMathEchidnaTest produces a `ReinterpretCast` from uint64 to biguint that puya doesn't support. Needs explicit promotion via `itob`.

4. **`receive`/payable patterns:** TransferHelperTestFakeFallback uses `receive() external payable` and `payable(msg.sender).transfer()` which have no Algorand equivalent and cause deserialization failures.

5. **EVM-specific operations:** `staticcall`, `call{value: ...}`, `abi.encodeWithSelector`, `abi.decode` — used by SafeERC20Namer and TransferHelper — have no direct Algorand equivalents.

6. **Gas cost functions:** Always return 0 since Algorand has no gas metering. This is semantically correct but makes gas benchmarking tests fail.

## Comparison with First Task (TokenVesting)

| Aspect | TokenVesting (Task 1) | Uniswap solidity-lib (Task 2) |
|--------|----------------------|-------------------------------|
| Contract complexity | 1 complex contract with inheritance | 7 pure libraries + 10 test contracts |
| State management | Yes (structs, mappings) | No (pure functions only) |
| AWST generation | 1 contract, 7504 lines | 10 contracts, all successful |
| Puya backend | Deserialization OK, validation fails (struct storage) | 7/10 produce TEAL |
| TEAL generation | Blocked (ARC4Struct needed) | 7 contracts produce TEAL (904 total lines) |
| Blocking issue | Struct types need ARC4 encoding | Library function inlining |
| Original tests | 15/15 pass | 135/135 pass (pre-port), 110/135 pass (post-port) |

The Uniswap compilation went further than TokenVesting — it actually produced TEAL files. The contracts are simpler (no state, no structs, pure functions) which avoids the ARC4Struct storage issue that blocked Task 1. The library inlining issue is a new, different blocker.

## Next Steps

1. **Library function inlining** — Translate library function bodies as AWST Subroutine nodes. Map library calls to these subroutines instead of skipping them.

2. **Variable-exponent power** — Handle `2**n` where `n` is a variable by implementing it as a biguint operation or loop.

3. **Assert statement** — Map Solidity `assert(condition)` to AWST assert nodes correctly.

4. **Algorand tests** — Once library inlining works, the pure math contracts (Babylonian, BitMath, FullMath, FixedPoint) are ideal test candidates for Algorand since they use only integer arithmetic with no EVM-specific features.
