// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

struct CompactRecord {
    uint8 small;
    int16 delta;
    bool enabled;
    bytes3 tag;
    string label;
    uint16[2] fixedValues;
    uint32[] values;
}

library CompactRecordLib {
    function encoded(CompactRecord memory r) internal pure returns (bytes memory) {
        return abi.encode(r);
    }
}

contract AggregateCompaction {
    mapping(uint256 => CompactRecord) private records;

    function store(uint256 key, CompactRecord memory r) external {
        records[key] = r;
    }

    function copy(uint256 from, uint256 to) external {
        records[to] = records[from];
    }

    function get(uint256 key) external view returns (CompactRecord memory) {
        return records[key];
    }

    function encoded(uint256 key) external view returns (bytes memory) {
        return abi.encode(records[key]);
    }

    function libraryEncoded(uint256 key) external view returns (bytes memory) {
        return CompactRecordLib.encoded(records[key]);
    }

    function changed(uint256 key) external returns (bytes32 beforeWrite, bytes32 afterWrite) {
        beforeWrite = keccak256(abi.encode(records[key]));
        records[key].small = 6;
        afterWrite = keccak256(abi.encode(records[key]));
    }

    function update(uint256 key) internal returns (uint256) {
        records[key].small = 42;
        return 9;
    }

    function sequenced(uint256 key) external returns (bytes memory) {
        return abi.encode(records[key], update(key));
    }

    function memorySnapshots(CompactRecord memory r) external pure returns (bytes32 first, bytes32 second) {
        first = keccak256(CompactRecordLib.encoded(r));
        assembly {
            // Legal dirty memory: typed loads clean these words, unlike
            // untrusted ABI input which must reject noncanonical padding.
            mstore(r, 0x105)
            mstore(add(r, 32), 0xfffe)
            mstore(add(r, 64), 2)
            mstore(add(r, 96), or(shl(232, 0x616263), 1))
            mstore(mload(add(r, 160)), 0x10009)
        }
        second = keccak256(abi.encode(r));
    }
}
