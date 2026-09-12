pragma solidity ^0.8.20;

library LocationWrites {
    function set(uint256[2] storage target, uint256 value) internal { target[0] = value; }
}

contract ResolvedLocationFacts {
    struct Record { uint256[2] items; uint256 sibling; }
    Record private record;
    uint256[2] private indices;

    function updateSibling() internal returns (uint256) { record.sibling = 9; return 7; }
    function delayedWriteback() external returns (uint256, uint256) {
        record.items[0] = 5;
        record.sibling = 1;
        LocationWrites.set(record.items, updateSibling());
        return (record.items[0], record.sibling);
    }

    function frozenTupleIndex() external returns (uint256, uint256, uint256) {
        uint256 index;
        indices[0] = 3;
        indices[1] = 4;
        (indices[index], index) = (7, 1);
        return (indices[0], indices[1], index);
    }

    function conditionalBlob(bool choose) external pure returns (uint256, uint256) {
        uint256[2] memory first;
        uint256[2] memory second;
        assembly { mstore(first, 1) mstore(second, 2) }
        (choose ? first : second)[0] = 7;
        return (first[0], second[0]);
    }

    function parenthesizedBlob() external pure returns (uint256) {
        uint256[2] memory value;
        assembly { mstore(value, 1) }
        ((value))[0] += 7;
        return value[0];
    }

    function castBlob() external pure returns (bytes1) {
        bytes memory value = hex"1122";
        assembly { pop(mload(value)) }
        bytes(string(value))[0] = 0x33;
        return value[0];
    }
}
