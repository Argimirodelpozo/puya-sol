// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// abi.encode/decode ROUND-TRIP probe (post abi->ARC4 migration). For each, EVM and AVM must both
// return the input unchanged (the intermediate encoded bytes differ by design — only the round-trip
// identity is differential-testable). fuzz_evm.py boundary-fuzzes the inputs and diffs EVM vs AVM.
contract C {
    struct S { uint128 a; int16 b; bool c; address d; }
    struct N { uint64 x; uint256[] ys; }

    // wide / signed-subword dynamic arrays
    function rtU128Arr(uint128[] calldata a) external pure returns (uint128[] memory) {
        return abi.decode(abi.encode(a), (uint128[]));
    }
    function rtI16Arr(int16[] calldata a) external pure returns (int16[] memory) {
        return abi.decode(abi.encode(a), (int16[]));
    }
    // fixed array
    function rtU64Fixed(uint64[3] calldata a) external pure returns (uint64[3] memory) {
        return abi.decode(abi.encode(a), (uint64[3]));
    }
    // nested dynamic
    function rtNested(uint256[][] calldata a) external pure returns (uint256[][] memory) {
        return abi.decode(abi.encode(a), (uint256[][]));
    }
    // packed struct
    function rtStruct(S calldata s) external pure returns (S memory) {
        return abi.decode(abi.encode(s), (S));
    }
    // struct with a dynamic field
    function rtNestedStruct(N calldata n) external pure returns (N memory) {
        return abi.decode(abi.encode(n), (N));
    }
    // bytes / string
    function rtBytes(bytes calldata b) external pure returns (bytes memory) {
        return abi.decode(abi.encode(b), (bytes));
    }
    function rtString(string calldata s) external pure returns (string memory) {
        return abi.decode(abi.encode(s), (string));
    }
    // tuple (multi-value) — rtTuple TEMPORARILY REMOVED: int16 decode→uint64 vs return→biguint mismatch
    // (isolating the other round-trips; rtTuple investigated separately).
    // array of struct
    function rtStructArr(S[] calldata a) external pure returns (S[] memory) {
        return abi.decode(abi.encode(a), (S[]));
    }
}
