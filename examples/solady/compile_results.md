# Solady Compilation Results

Solady v0.1.x — All 75 main source files attempted (excluding ext/, g/, legacy/ variants).
Source unmodified. Wrappers in `aux-wrappers-to-deploy/` make libraries/abstract contracts deployable.

## Successfully Compiled (23 contracts)

| # | Contract | Source Type | Size | Notes |
|---|----------|-------------|------|-------|
| 1 | Ownable | abstract | 5.5KB | auth/Ownable.sol |
| 2 | ReentrancyGuard | abstract | small | utils/ReentrancyGuard.sol |
| 3 | LibBit | library | split | utils/LibBit.sol (needed --split-contracts) |
| 4 | DateTimeLib | library | small | utils/DateTimeLib.sol |
| 5 | SafeCastLib | library | 1.8KB | utils/SafeCastLib.sol |
| 6 | ERC2981 | abstract | small | tokens/ERC2981.sol |
| 7 | ERC6909 | abstract | small | tokens/ERC6909.sol |
| 8 | P256 | library | small | utils/P256.sol |
| 9 | LibPRNG | library | small | utils/LibPRNG.sol |
| 10 | GasBurnerLib | library | small | utils/GasBurnerLib.sol |
| 11 | Receiver | abstract | small | accounts/Receiver.sol |
| 12 | MinHeapLib | library | small | utils/MinHeapLib.sol |
| 13 | EnumerableSetLib | library | small | utils/EnumerableSetLib.sol |
| 14 | EnumerableMapLib | library | small | utils/EnumerableMapLib.sol |
| 15 | DynamicBufferLib | library | small | utils/DynamicBufferLib.sol |
| 16 | LibMap | library | small | utils/LibMap.sol |
| 17 | EIP712 | abstract | small | utils/EIP712.sol |
| 18 | Lifebuoy | contract | small | utils/Lifebuoy.sol |
| 19 | LibRLP | library | small | utils/LibRLP.sol |
| 20 | SafeTransferLib | library | small | utils/SafeTransferLib.sol |
| 21 | LibCall | library | small | utils/LibCall.sol |
| 22 | CREATE3 | library | small | utils/CREATE3.sol |
| 23 | ERC4626 | abstract | split | tokens/ERC4626.sol (was crashing, fixed) |

## Failed — Frontend Errors (puya-sol C++ compiler)

### `unsupported Yul literal kind` (4 contracts)
Solady uses string Yul literals (e.g., `"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"`) which our assembly translator doesn't handle.
- Base64, Base58, ECDSA, WebAuthn

### `calldataload with non-constant offset` (5 contracts)
Solady iterates over calldata arrays using computed offsets in assembly.
- ERC1155, Multicallable, LibBitmap, ERC7821, Timelock

### `keccak256 with non-constant offset/length` (7 contracts)
Solady computes keccak256 over dynamic memory regions.
- EfficientHashLib, LibString, BlockHashLib, DynamicArrayLib, Milady, LibBytes, LibClone, SSTORE2

### `multi-variable assignment in assembly` (3 contracts)
Yul functions returning multiple values via `let a, b := func()`.
- LibSort, SemVerLib, MerkleTreeLib

### `call with non-constant address` (2 contracts)
Dynamic `call()` to non-precompile addresses.
- ERC1271, SignatureCheckerLib

### `bool→bytes cast` (1 contract)
FixedPointMathLib casts bool to bytes in assembly.
- FixedPointMathLib

### ~~`double free / crash` (5 contracts)~~ — FIXED
Use-after-free bug in `AssemblyBuilder::m_ownedTypes` — WType objects freed when AssemblyBuilder
destructed, but AWST nodes still held raw pointers. Fix: moved ownership to `TypeMapper::createType()`.
- ~~ERC4626~~ → now compiles successfully (moved to "Compiled" list)
- ERC20, WETH, ERC20Votes → no longer crash, but have puya backend error (`bytes comparison between different wtypes: account and bytes`)
- RedBlackTreeLib → no longer crashes, but has infinite loop on recursive sload

## Failed — Backend Errors (puya Python compiler)

### `incompatible types on assignment` (2 contracts)
Assembly produces biguint but target expects array type.
- OwnableRoles, EnumerableRoles

### `not all paths return a value` (1 contract)
Assembly-only function with revert paths confuses the return value checker.
- ERC721

### `IfElse condition type mismatch` (4 contracts)
bytes[32] used as boolean condition in assembly-generated AWST.
- Initializable, CallContextChecker, UUPSUpgradeable, LibZip

### `keccak256 non-constant in LibTransient` (1 contract)
- LibTransient

### `puya backend type error in LibStorage` (1 contract)
- LibStorage

## Not Attempted (EVM-only patterns)

These contracts are fundamentally EVM-specific (proxy patterns, bytecode deployment, delegatecall):
- ERC4337, ERC4337Factory, ERC6551, ERC6551Proxy, EIP7702Proxy
- ERC1967Factory, ERC1967FactoryConstants
- DeploylessPredeployQueryer, UpgradeableBeacon
- LibEIP7702, LibERC6551, LibERC7579
- MetadataReaderLib (reads external contract metadata via staticcall)

## Summary

| Category | Count |
|----------|-------|
| Compiled successfully | 23 |
| Failed (frontend — assembly limitations) | 22 |
| Failed (backend — type/path errors) | 13 |
| ~~Failed (crash — splitter bug)~~ | ~~0~~ (FIXED) |
| Not attempted (EVM-only) | ~15 |
| **Total source files** | **~75** |

### Key Insight
Solady is **extremely assembly-heavy** — almost every function body is pure Yul inline assembly,
including EVM-specific operations like `sload`/`sstore` with computed storage slots, dynamic
`calldataload`, and string literals. The 22 contracts that compile do so because their assembly
is simpler (constants, basic arithmetic, fixed-offset memory operations).

The main blockers are:
1. **String Yul literals** — easy fix in assembly translator
2. **Dynamic calldataload/keccak256** — fundamental EVM memory model issue
3. **Multi-variable Yul returns** — parser/translator limitation
4. ~~**Splitter crash**~~ — FIXED (use-after-free in AssemblyBuilder type ownership)
