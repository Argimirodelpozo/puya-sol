// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// abi round-trip IDENTITY probe — each fn returns a clean bool (does decode(encode(x)) re-encode to the
// same bytes as x?), computed ON-CHAIN via keccak so there are NO fuzzer rendering artifacts. EVM is
// always true (round-trip is identity); a false on AVM where EVM is true is an unambiguous real bug.
// (Compares keccak of abi.encode WITHIN each side — the ARC4-vs-EVM byte difference cancels.)
contract C {
    struct S { uint128 a; int16 b; bool c; address d; }
    struct M { S inner; uint64 x; bytes tail; }

    function _eq(bytes memory p, bytes memory q) internal pure returns (bool) {
        return keccak256(p) == keccak256(q);
    }
    function idU128Arr(uint128[] calldata v) external pure returns (bool) {
        return _eq(abi.encode(abi.decode(abi.encode(v), (uint128[]))), abi.encode(v));
    }
    function idI16Arr(int16[] calldata v) external pure returns (bool) {
        return _eq(abi.encode(abi.decode(abi.encode(v), (int16[]))), abi.encode(v));
    }
    function idU64Fixed(uint64[3] calldata v) external pure returns (bool) {
        return _eq(abi.encode(abi.decode(abi.encode(v), (uint64[3]))), abi.encode(v));
    }
    function idNested(uint256[][] calldata v) external pure returns (bool) {
        return _eq(abi.encode(abi.decode(abi.encode(v), (uint256[][]))), abi.encode(v));
    }
    function idStruct(S calldata v) external pure returns (bool) {
        return _eq(abi.encode(abi.decode(abi.encode(v), (S))), abi.encode(v));
    }
    function idStructArr(S[] calldata v) external pure returns (bool) {
        return _eq(abi.encode(abi.decode(abi.encode(v), (S[]))), abi.encode(v));
    }
    function idStructFixed(S[2] calldata v) external pure returns (bool) {
        return _eq(abi.encode(abi.decode(abi.encode(v), (S[2]))), abi.encode(v));
    }
    function idMulti(uint128[][2] calldata v) external pure returns (bool) {
        return _eq(abi.encode(abi.decode(abi.encode(v), (uint128[][2]))), abi.encode(v));
    }
    function idNestedStruct(M calldata v) external pure returns (bool) {
        return _eq(abi.encode(abi.decode(abi.encode(v), (M))), abi.encode(v));
    }
    function idBytes(bytes calldata v) external pure returns (bool) {
        return _eq(abi.encode(abi.decode(abi.encode(v), (bytes))), abi.encode(v));
    }
    function idString(string calldata v) external pure returns (bool) {
        return _eq(abi.encode(abi.decode(abi.encode(v), (string))), abi.encode(v));
    }
    function idBytesArr(bytes[] calldata v) external pure returns (bool) {
        return _eq(abi.encode(abi.decode(abi.encode(v), (bytes[]))), abi.encode(v));
    }
}
