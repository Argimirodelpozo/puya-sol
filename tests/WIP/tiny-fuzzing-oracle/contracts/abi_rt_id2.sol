// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// abi round-trip IDENTITY probe (round 2) — harder compound types. Each returns a clean bool
// (decode(encode(x)) re-encodes to keccak-equal bytes as x), computed on-chain so there are no
// fuzzer rendering artifacts. EVM is always true; an AVM false where EVM true is a real bug.
contract C {
    struct Inner { int16 a; uint64 b; }
    struct Outer { Inner inner; bytes tail; address who; }
    struct Packed { uint8 a; int8 b; bool c; uint16 d; int24 e; }

    function _eq(bytes memory p, bytes memory q) internal pure returns (bool) {
        return keccak256(p) == keccak256(q);
    }
    // signed dynamic array
    function idI32Arr(int32[] calldata v) external pure returns (bool) {
        return _eq(abi.encode(abi.decode(abi.encode(v), (int32[]))), abi.encode(v));
    }
    function idI128Arr(int128[] calldata v) external pure returns (bool) {
        return _eq(abi.encode(abi.decode(abi.encode(v), (int128[]))), abi.encode(v));
    }
    // 2D arrays
    function idU64MatDyn(uint64[][] calldata v) external pure returns (bool) {
        return _eq(abi.encode(abi.decode(abi.encode(v), (uint64[][]))), abi.encode(v));
    }
    function idU64MatFixedInner(uint64[2][] calldata v) external pure returns (bool) {
        return _eq(abi.encode(abi.decode(abi.encode(v), (uint64[2][]))), abi.encode(v));
    }
    // bytesN array + bytes array
    function idB32Arr(bytes32[] calldata v) external pure returns (bool) {
        return _eq(abi.encode(abi.decode(abi.encode(v), (bytes32[]))), abi.encode(v));
    }
    function idBytesArr(bytes[] calldata v) external pure returns (bool) {
        return _eq(abi.encode(abi.decode(abi.encode(v), (bytes[]))), abi.encode(v));
    }
    // nested struct (struct-in-struct with a dynamic field)
    function idOuter(Outer calldata v) external pure returns (bool) {
        return _eq(abi.encode(abi.decode(abi.encode(v), (Outer))), abi.encode(v));
    }
    function idOuterArr(Outer[] calldata v) external pure returns (bool) {
        return _eq(abi.encode(abi.decode(abi.encode(v), (Outer[]))), abi.encode(v));
    }
    // packed mixed signed/unsigned sub-word struct
    function idPacked(Packed calldata v) external pure returns (bool) {
        return _eq(abi.encode(abi.decode(abi.encode(v), (Packed))), abi.encode(v));
    }
    function idPackedArr(Packed[] calldata v) external pure returns (bool) {
        return _eq(abi.encode(abi.decode(abi.encode(v), (Packed[]))), abi.encode(v));
    }
    // string array
    function idStrArr(string[] calldata v) external pure returns (bool) {
        return _eq(abi.encode(abi.decode(abi.encode(v), (string[]))), abi.encode(v));
    }
    // multi-value tuple with a dynamic + signed sub-word
    function idTuple(uint256 a, int16 b, bytes calldata c) external pure returns (bool) {
        (uint256 ra, int16 rb, bytes memory rc) = abi.decode(abi.encode(a, b, c), (uint256, int16, bytes));
        return ra == a && rb == b && keccak256(rc) == keccak256(c);
    }
}
