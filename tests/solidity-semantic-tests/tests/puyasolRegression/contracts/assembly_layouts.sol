// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

contract AssemblyLayouts {
    struct Packed {
        uint8 a;
        bool b; bool c; bool d; bool e; bool f; bool g; bool h; bool i; bool j;
        int24 signedValue;
        uint128 wide;
    }
    mapping(uint256 => Packed) private entries;

    function calldataDigest(int24[2] calldata signedValues, bytes4[2] calldata fixedValues,
        bool[9] calldata flags, bytes[] calldata blobs) external pure returns (bytes32 digest, uint256 size) {
        assembly {
            size := sub(calldatasize(), 4)
            calldatacopy(128, 4, size)
            digest := keccak256(128, size)
        }
    }
    function packedWord(uint256 word) external returns (uint256 read, bool matches) {
        Packed storage p = entries[1];
        assembly { sstore(p.slot, word) read := sload(p.slot) }
        matches = p.a == 7 && p.b && p.c && p.d && p.e && p.f && p.g && p.h && p.i && p.j
            && p.signedValue == -7 && p.wide == 2**100;
    }
}
