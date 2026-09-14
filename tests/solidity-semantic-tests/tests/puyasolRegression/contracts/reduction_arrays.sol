// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

contract ReductionArrays {
    uint64[16] private a;
    uint64[16] private b;
    uint64[4] private small;
    bool[16] private flags;
    string[8] private labels;
    string[4] private shortLabels;
    struct Wide { uint256[8] words; uint128 tag; }
    Wide private x;
    Wide private y;

    function fixedCopy(uint64[16] memory values) external returns (bytes32) {
        a = values;
        b = a;
        delete a;
        return keccak256(abi.encode(b, a));
    }

    function shortCopy() external returns (bytes32) {
        for (uint256 i; i < 16; ++i) b[i] = uint64(i + 1);
        small[0] = 11;
        small[3] = 44;
        b = small;
        return keccak256(abi.encode(b));
    }

    function boolCopy(bool[16] memory values) external returns (bytes32) {
        flags = values;
        bool[16] memory snapshot = flags;
        delete flags;
        return keccak256(abi.encode(snapshot, flags));
    }

    function dynamicChildren() external returns (bytes32 beforeClear, bytes32 afterClear) {
        for (uint256 i; i < 8; ++i) labels[i] = "stale";
        shortLabels[0] = "first";
        shortLabels[3] = "last";
        labels = shortLabels;
        beforeClear = keccak256(abi.encode(labels));
        delete labels;
        afterClear = keccak256(abi.encode(labels));
    }

    function decoded(bytes memory input) external pure returns (bytes32 first, bytes32 second) {
        (uint16[8] memory values, string[8] memory texts) = abi.decode(input, (uint16[8], string[8]));
        first = keccak256(abi.encode(values, texts));
        texts[7] = "changed";
        second = keccak256(abi.encode(values, texts));
    }

    function dirty(uint16[16] memory values) external pure returns (bytes32) {
        assembly { mstore(add(values, 480), 0x10009) }
        return keccak256(abi.encode(values));
    }

    function tupleCopy() external returns (uint256, uint256, uint128, uint128) {
        x.words[0] = 11;
        x.words[7] = 17;
        x.tag = 13;
        y.words[0] = 22;
        y.words[7] = 27;
        y.tag = 23;
        (x, y) = (y, x);
        return (x.words[0], y.words[7], x.tag, y.tag);
    }

    function clearStruct() external returns (uint256, uint128) {
        x.words[7] = 17;
        x.tag = 13;
        delete x;
        return (x.words[7], x.tag);
    }
}
