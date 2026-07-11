// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// abi codec EDGE cases — empty / single-element / nested-empty dynamic types. On-chain keccak identity
// round-trips (decode(encode(x)) re-encodes equal). EVM always true; AVM false where EVM true = bug.
contract C {
    struct S { uint128 a; bytes b; uint64[] c; }

    function _eq(bytes memory p, bytes memory q) internal pure returns (bool) {
        return keccak256(p) == keccak256(q);
    }
    // dynamic types that may be empty (fuzzer drives empty + a few elements)
    function rtBytes(bytes calldata v) external pure returns (bool) {
        return _eq(abi.encode(abi.decode(abi.encode(v), (bytes))), abi.encode(v));
    }
    function rtU64Arr(uint64[] calldata v) external pure returns (bool) {
        return _eq(abi.encode(abi.decode(abi.encode(v), (uint64[]))), abi.encode(v));
    }
    function rtNestedArr(uint64[][] calldata v) external pure returns (bool) {
        return _eq(abi.encode(abi.decode(abi.encode(v), (uint64[][]))), abi.encode(v));
    }
    function rtBytesArr(bytes[] calldata v) external pure returns (bool) {
        return _eq(abi.encode(abi.decode(abi.encode(v), (bytes[]))), abi.encode(v));
    }
    // struct with mixed static + two dynamic fields (either may be empty)
    function rtStruct(S calldata v) external pure returns (bool) {
        return _eq(abi.encode(abi.decode(abi.encode(v), (S))), abi.encode(v));
    }
    function rtStructArr(S[] calldata v) external pure returns (bool) {
        return _eq(abi.encode(abi.decode(abi.encode(v), (S[]))), abi.encode(v));
    }
    // double-encode then double-decode (abi.encode of bytes-that-are-already-encoded)
    function rtDouble(uint256 a, uint256 b) external pure returns (bool) {
        bytes memory inner = abi.encode(a, b);
        bytes memory outer = abi.encode(inner);
        bytes memory back = abi.decode(outer, (bytes));
        (uint256 ra, uint256 rb) = abi.decode(back, (uint256, uint256));
        return ra == a && rb == b;
    }
    // length-of-round-tripped dynamic array equals input length
    function rtArrLen(uint256[] calldata v) external pure returns (bool) {
        return abi.decode(abi.encode(v), (uint256[])).length == v.length;
    }
}
