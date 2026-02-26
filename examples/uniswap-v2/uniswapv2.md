# Uniswap V2 Core — Compiled from Solidity 0.5.16 to AVM TEAL

## Summary

All three Uniswap V2 Core contracts compiled from the **original, unmodified** Solidity source (pragma =0.5.16) to AVM TEAL, with 14 tests passing on Algorand localnet.

| Contract | TEAL Lines | Extra Pages | Tests |
|---|---|---|---|
| UniswapV2ERC20 | 303 | 0 | 4 |
| UniswapV2Factory | 207 | 0 | 4 |
| UniswapV2Pair | 1,565 | 1 | 6 |
| **Total** | **2,075** | | **14** |

Source: https://github.com/Uniswap/v2-core (commit: master)

## Contracts Downloaded

12 Solidity files from the Uniswap V2 Core repository:

```
contracts/
  UniswapV2ERC20.sol          — Base ERC20 LP token (approve, transfer, permit)
  UniswapV2Factory.sol        — Pair factory (createPair, fee management)
  UniswapV2Pair.sol           — Core AMM pair (mint, burn, swap, sync, skim)
  interfaces/
    IERC20.sol
    IUniswapV2ERC20.sol
    IUniswapV2Factory.sol
    IUniswapV2Pair.sol
    IUniswapV2Callee.sol
  libraries/
    Math.sol                  — sqrt, min
    SafeMath.sol              — checked arithmetic
    UQ112x112.sol             — fixed-point math (UQ112.112)
  test/
    ERC20.sol                 — Test token
```

All files are **original and unmodified** — no changes to the Solidity source.

## Compiler Enhancements

The puya-sol compiler was enhanced to handle Solidity 0.5.16 syntax and EVM-specific constructs. All transformations happen at the compiler level — the user's source code is never modified.

### Source Transformations (applied before parsing)

These transformations bridge the Solidity 0.5.16 → 0.8.28 syntax gap. The puya-sol compiler is built on the Solidity 0.8.28 parser, so older syntax must be normalized:

1. **Pragma relaxation**: `pragma solidity =0.5.16;` → `pragma solidity >=0.5.0;`
   - **Why**: 0.8.28 parser rejects version pinning to older versions. The `=`, `^`, `~`, `>=`, `<` prefixes are all handled.

2. **Constructor visibility removal**: `constructor() public {` → `constructor() {`
   - **Why**: Solidity 0.8.x removed constructor visibility keywords. In 0.5.x, `public` or `internal` was required on constructors.

3. **Type cast max idiom**: `uint(-1)` → `type(uint).max`, `uint112(-1)` → `type(uint112).max`
   - **Why**: In 0.5.x, casting -1 to unsigned gave max value (two's complement). In 0.8.x, this overflows. The `type(X).max` syntax is the 0.8.x equivalent.

4. **Bare Yul builtins**: `chainid` → `chainid()`
   - **Why**: In 0.5.x Yul assembly, some builtins could be used without parentheses. 0.8.x requires function-call syntax.

### Cross-File Event Deduplication

Solidity 0.5.x allows interfaces and contracts to declare the same event. In 0.8.x, this is an error ("Event with same name and parameter types defined twice"). The compiler:
1. Scans imported interfaces for event signatures
2. Removes matching event declarations from the contract source
3. Also suppresses the error as a backup

### Error Suppression

Two 0.8.x errors are suppressed because the underlying code is still valid:
- **Duplicate event declarations** — handled by dedup above
- **Diamond inheritance override requirements** — 0.5.x allows implicit overrides when a contract inherits from multiple interfaces that define the same function. 0.8.x requires explicit `override(I1, I2)`. The AST is still usable without this.

### Contract Name Selection

Fixed: previously the compiler selected the last contract in the AST. Now it matches the contract name to the source filename (e.g., `UniswapV2Pair.sol` → contract `UniswapV2Pair`).

## EVM Constructs — AVM Translations & Stubs

### Working Translations

| Solidity/EVM | AVM Translation |
|---|---|
| `msg.sender` | `txn Sender` |
| `block.timestamp` | `global LatestTimestamp` |
| `block.number` | `global Round` |
| `address(this)` | `global CurrentApplicationAddress` |
| `keccak256(...)` | `keccak256` (AVM native) |
| `abi.encode(...)` | `concat` chain of byte-converted args |
| `abi.encodePacked(...)` | `concat` chain with packed byte widths |
| `require(cond, msg)` | `assert` with error message |
| `SafeMath.add/sub` | `b+`/`b-` with overflow checks |
| `mapping(K => V)` | Box storage with `sha256(key)` |
| `uint(-1)` / `type(uint).max` | `0xffffff...ff` (32 bytes) |
| `emit Event(...)` | `log` with ABI-encoded data |

### Stubs (EVM-specific, no AVM equivalent)

| Solidity/EVM | AVM Stub | Notes |
|---|---|---|
| `chainid()` | `itob(0)` as biguint | Algorand has no chain ID concept |
| `ecrecover(...)` | returns zero address | AVM `ecdsa_pk_recover` returns (X,Y) not address |
| `address.call(data)` | returns `(true, "")` | Cross-contract calls need inner app call translation |
| `create2(...)` | returns 0 | EVM contract deployment has no AVM equivalent |
| `abi.encodeCall(...)` | returns empty bytes | EVM calldata encoding not applicable |
| `returndatasize` | constant 0 | No return data buffer on AVM |
| `returndatacopy` | no-op | No return data buffer on AVM |

## Bug Fixes Made During This Work

### 1. `chainid()` biguint → uint64 constant folding (critical)

**Problem**: The puya backend's optimizer merged `IntegerConstant(0, biguint)` (chainId) with `IntegerConstant(0, uint64)` (from ApplicationID comparison) into a single `intc_3 // 0`. When `abi.encode` tried to `concat` bytes with this uint64, it crashed: `concat arg 1 wanted []byte but got uint64`.

**Fix**: Changed `chainid()` to return `ReinterpretCast<biguint>(itob(uint64(0)))` instead of `IntegerConstant(0, biguint)`. The `itob` intrinsic produces an explicit 8-byte value (`0x0000000000000000`) that the optimizer folds into the surrounding bytes constant rather than merging with uint64 constants.

**Files**: `AssemblyTranslator.cpp`

### 2. `address(this)` — uninitialized variable

**Problem**: The Solidity `this` keyword was translated as an uninitialized local variable `VarExpression("this", account)`, defaulting to zero. The constructor's DOMAIN_SEPARATOR computation with `address(this)` produced a zero address.

**Fix**: Added handling for the `this` identifier in `ExpressionTranslator::visit(Identifier)` to emit `global CurrentApplicationAddress`.

**Files**: `ExpressionTranslator.cpp`

### 3. Dynamic array length state not initialized

**Problem**: State variables like `address[] public allPairs` use box storage, and their length tracker (`allPairs_length`) is stored in global state. But the constructor only initialized global state variables, skipping the implicit `_length` tracker. Calling `allPairsLength()` failed with "check allPairs_length exists".

**Fix**: Added a second initialization pass in the constructor that finds all box-stored dynamic array state variables and initializes their `_length` global state to 0.

**Files**: `ContractTranslator.cpp`

### 4. `abi.decode(data, (bool))` — type mismatch

**Problem**: `ReinterpretCast` from bytes to bool is not supported by puya. The `abi.decode(data, (bool))` pattern used this unsupported cast.

**Fix**: For bool targets, use `btoi` followed by numeric comparison (`!= 0`) instead of ReinterpretCast.

**Files**: `ExpressionTranslator.cpp`

### 5. `.call()` without `{value: X}` — unhandled

**Problem**: Only `.call{value: X}("")` was handled. The generic `.call(data)` pattern (used in IUniswapV2Callee) was not.

**Fix**: Added stub for `.call(data)` returning `(true, "")`.

**Files**: `ExpressionTranslator.cpp`

### 6. Ternary expression type mismatch

**Problem**: `condition ? biguint_expr : 0` where `0` was uint64 but target type was biguint.

**Fix**: Added `implicitNumericCast` for both branches of ternary expressions.

**Files**: `ExpressionTranslator.cpp`

### 7. Biguint default values for local variables

**Problem**: Uninitialized biguint variables (`uint chainId;`) created `IntegerConstant(0, biguint)` which the puya backend's optimizer could merge with uint64 constants.

**Fix**: Changed default biguint initialization to `BytesConstant({}, biguint)` in `StatementTranslator.cpp` and `StorageMapper.cpp`.

**Files**: `StatementTranslator.cpp`, `StorageMapper.cpp`

## Test Results

```
test_erc20.py::test_erc20_deploys            PASSED    — deploys (303L TEAL)
test_erc20.py::test_approve                  PASSED    — approve(spender, value) returns true
test_erc20.py::test_transfer                 PASSED    — transfer(to, 0) returns true
test_erc20.py::test_transfer_from            PASSED    — transferFrom(from, to, 0) returns true

test_factory.py::test_factory_deploys        PASSED    — deploys with feeToSetter constructor arg
test_factory.py::test_all_pairs_length       PASSED    — allPairsLength returns 0 initially
test_factory.py::test_set_fee_to             PASSED    — setFeeTo updates fee recipient
test_factory.py::test_set_fee_to_setter      PASSED    — setFeeToSetter transfers admin

test_pair.py::test_pair_deploys              PASSED    — deploys (1565L TEAL, extra_pages=1)
test_pair.py::test_get_reserves              PASSED    — getReserves returns initial zeros
test_pair.py::test_initialize                PASSED    — initialize(token0, token1) runs
test_pair.py::test_approve_inherited         PASSED    — inherited ERC20 approve works
test_pair.py::test_transfer_inherited        PASSED    — inherited ERC20 transfer works
test_pair.py::test_transfer_from_inherited   PASSED    — inherited ERC20 transferFrom works
```

## Known Limitations

1. **Cross-contract calls stubbed**: `IERC20.balanceOf()`, `IUniswapV2Factory.feeTo()`, `IUniswapV2Callee.uniswapV2Call()` are all cross-contract calls that return stubs. Full AMM flow (mint/burn/swap with real tokens) would require deployed token contracts and inner app call translation.

2. **`ecrecover` stubbed**: The `permit()` function's signature recovery always returns the zero address. AVM's `ecdsa_pk_recover` returns separate X,Y curve points rather than an address.

3. **`create2` stubbed**: `createPair()` in the Factory uses `create2` for deterministic pair deployment. This has no AVM equivalent — pair contracts would need separate deployment and registration.

4. **`chainid()` returns 0**: Algorand has no chain ID concept. The DOMAIN_SEPARATOR hash will differ from EVM but is internally consistent.

5. **`address(this)` is CurrentApplicationAddress**: On AVM this is 32 bytes (vs EVM's 20 bytes), so DOMAIN_SEPARATOR differs from EVM.

## Directory Structure

```
examples/uniswap-v2/
  contracts/           — Original Uniswap V2 Solidity (unmodified)
  out/
    UniswapV2ERC20/    — 303 lines TEAL + ARC56
    UniswapV2Factory/  — 207 lines TEAL + ARC56
    UniswapV2Pair/     — 1565 lines TEAL + ARC56
  test/
    conftest.py        — Fixtures, deploy helpers, contract funding
    test_erc20.py      — 4 tests
    test_factory.py    — 4 tests
    test_pair.py       — 6 tests
  uniswapv2.md         — This report
```
