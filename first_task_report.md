# First Task Report: TokenVesting Contract Compilation

## Summary

This report documents the effort to compile the [sample_solidity_project](https://github.com/hknio/sample_solidity_project) TokenVesting contract through the puya-sol C++ frontend, translating Solidity AST to AWST JSON, and invoking the puya backend to produce TEAL for Algorand.

**Result:** AWST JSON generation succeeded. Puya backend deserialization succeeded. TEAL generation is blocked by semantic validation errors related to struct types requiring ARC4 encoding.

## Build Process

### 1. Repository Setup

- **Solidity compiler:** Checked out tag `v0.8.28` (develop branch had an infinite loop bug in `parseAndAnalyze()` with complex contracts using events + virtual functions + inheritance; confirmed in v0.8.33 too)
- **Puya compiler:** Updated to latest from `algorandfoundation/puya`, `uv sync` run
- **Example project:** Cloned to `examples/example1/`, OpenZeppelin v4.5.0 installed via npm

### 2. Solidity Library Build

All 6 static libraries built from `solidity/build/`:
- libsolidity, libyul, libevmasm, liblangutil, libsmtutil, libsolutil

### 3. puya-sol-v2 Build

Built successfully with CMake + make. Linked against Solidity static libraries, nlohmann-json, and fmt.

```bash
cd puya-sol-v2/build && cmake .. -DCMAKE_BUILD_TYPE=RelWithDebInfo && make -j$(nproc)
```

## Compilation Pipeline Results

### Step 1: Solidity Parsing and Type-Checking

**Status: SUCCESS**

```
Source: .../contracts/TokenVesting.sol
Source units: 9 (TokenVesting + 8 OpenZeppelin contracts)
Parse and type-check successful!
```

Key fixes applied:
- **Pragma relaxation:** Regex replaces `pragma solidity 0.8.11;` with `pragma solidity >=0.8.0;` for compiler compatibility
- **Import resolution:** Used `addIncludePath(nodeModules)` for `@openzeppelin/` imports (remappings didn't work with FileReader)
- **Source unit naming:** Used `cliPathToSourceUnitName()` for proper source key

### Step 2: AWST JSON Generation

**Status: SUCCESS**

Generated 7,504-line AWST JSON file containing:
- 1 Contract node (TokenVesting)
- 8 skipped nodes (abstract contracts, interfaces, libraries)
- All state variables mapped to AppStorageDefinitions
- All functions translated with ARC4 method configs

Key fixes applied:
- **Absolute paths:** Source locations use absolute file paths (puya requires this)
- **Use-after-move bug:** Fixed `base->wtype` accessed after `std::move(base)` in array push handling
- **Enum serialization:** Changed from names (`"eq"`, `"add"`) to Python StrEnum values (`"=="`, `"+"`)
- **WType source_location:** Added `source_location` field to ReferenceArray and WTuple serialization
- **Box key types:** BoxValueExpression uses `box_key` WType, AppStateExpression uses `state_key`
- **Tuple names:** Empty return parameter names serialized as `null` instead of `["", ""]`
- **RationalNumber types:** Added TypeMapper case for Solidity RationalNumberType
- **Integer constants:** Ensured IntegerConstant always gets uint64/biguint WType
- **Address comparisons:** Route account-type comparisons through BytesComparisonExpression
- **address(0):** Convert to Algorand zero-address constant (AAAA...Y5HFKQ)
- **Type promotion:** Added uint64 to biguint promotion via `itob` intrinsic for mixed operations
- **BoxPrefixedKeyExpression:** Added for mapping accesses (prefix + key)
- **ArrayExtend:** Used instead of ArrayConcat for ReferenceArray push operations
- **Emit simplification:** Events converted to log intrinsics (full ARC-28 emit requires ARC4Struct)

### Step 3: Puya Backend

**Status: PARTIAL - Deserialization succeeds, semantic validation fails**

```
error: type is not suitable for storage
error: expression is not valid as an assignment target - object is immutable
```

The AWST JSON is correctly deserialized by puya. Two categories of semantic errors remain:

1. **Struct storage types:** Solidity structs are mapped to WTuple (named tuples), but puya requires ARC4Struct for storage. This requires significant changes to the type system to use ARC4 encoding for stored structs.

2. **Immutable struct fields:** WTuples in puya are immutable value types. Solidity code like `schedule.released += amount` tries to mutate a field in-place. The correct AWST pattern is to create a new tuple with the updated field (copy-on-write).

These are architectural issues requiring:
- ARC4Struct type generation from Solidity structs
- ARC4 encode/decode for storage reads/writes
- Copy-on-write struct field mutation pattern

## Original Hardhat Tests

**Status: ALL PASSING**

```
  15 passing (5s)

  - Can add a new plan
  - Can add a recurring plan
  - Can get the newly added plan
  - Can't create an invalid plan
  - Can revoke a plan
  - Can setup a lock instance
  - Can't assign a revoked plan
  - Can't assign a new vesting lock to the same address
  - Can't assign a new vesting lock to zero address
  - Can't assign a new vesting lock with an invalid start parameter
  - Can't assign a new vesting lock with an insufficient allowance
  - Can't assign a new vesting lock with an invalid payment plan
  - Releasable amount is 0 when period not started
  - Release reverts with error if no tokens are available
  - Releasable amount is 0 when cliff period is active
  - Multiple token releases after cliff period
  - Single token release after cliff period
```

Required fix: Commented out `mumbai` network config (referenced undefined env var).

## Algorand Tests

**Status: BLOCKED**

Cannot translate or run Algorand tests because TEAL generation is blocked by the semantic validation errors described above.

## Architecture Overview

```
TokenVesting.sol
    |
    v
[Solidity CompilerStack v0.8.28]
    | parseAndAnalyze()
    v
[Solidity AST (typed)]
    |
    v
[puya-sol-v2 C++ Frontend]
    |-- AWSTBuilder.cpp       (top-level driver)
    |-- ContractTranslator.cpp (contract → Contract node)
    |-- ExpressionTranslator.cpp (expressions → AWST expressions)
    |-- StatementTranslator.cpp  (statements → AWST statements)
    |-- TypeMapper.cpp         (Solidity types → AWST WTypes)
    |-- StorageMapper.cpp      (state vars → AppStorageDefinitions)
    |-- AWSTSerializer.cpp     (AWST → JSON serialization)
    v
awst.json (7,504 lines)  +  options.json
    |
    v
[puya backend]
    | deserialization: OK
    | validation: FAILS (struct types)
    v
TEAL (not yet generated)
```

## Known Limitations

1. **Struct types:** Solidity structs need ARC4Struct encoding for storage, not WTuple
2. **Struct mutations:** Field-level assignment needs copy-on-write pattern
3. **Events:** Simplified to log intrinsics; full ARC-28 event emission needs ARC4Struct
4. **Inheritance:** Base contract methods not inlined (only modifier inlining done)
5. **Library calls:** Solidity libraries (e.g., Strings) are skipped
6. **msg.sender:** Mapped to `Txn.Sender` but may need additional Algorand-specific handling
7. **ERC20 interaction:** Solidity's IERC20 calls need mapping to ASA operations

## Files Modified

### puya-sol-v2/src/
- `main.cpp` - Pragma relaxation, FileReader wrapping, include paths, absolute paths
- `builder/ExpressionTranslator.cpp` - Use-after-move fix, type promotion, address handling, BoxPrefixedKey
- `builder/ContractTranslator.cpp` - Empty tuple names handling
- `builder/StatementTranslator.cpp` - Emit simplification
- `builder/StorageMapper.cpp` - Box key type selection
- `builder/StorageMapper.h` - Updated makeKeyExpr signature
- `builder/TypeMapper.cpp` - RationalNumber type mapping
- `json/AWSTSerializer.cpp` - Enum value fixes, source_location fields, AppStorageKind values, ArrayExtend, BoxPrefixedKey
- `awst/Node.h` - Added BoxPrefixedKeyExpression, ArrayExtend nodes

### solidity/
- Checked out `v0.8.28` tag (from develop branch)

### examples/example1/
- `hardhat.config.ts` - Commented out mumbai network config
