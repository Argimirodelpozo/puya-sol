// SPDX-License-Identifier: MIT
pragma solidity ^0.8.34;

contract ExpressionAssignmentResults {
    struct Cell { uint256 value; uint256 other; }
    Cell private stored;
    uint256[2] private fixedValues;
    uint256[2] private otherFixed;
    uint256[] private dynamicValues;
    bytes private storedBytes;

    function blob(bytes memory input) external pure returns (bytes memory) {
        bytes memory target = new bytes(1);
        assembly { mstore8(add(target, 32), 0x7f) }
        return (target = input);
    }
    function structure(uint256 value) external returns (uint256, uint256) {
        Cell memory result = (stored = Cell(value, 9));
        return (result.value, stored.other);
    }
    function fixedArray(uint256 value) external returns (uint256, uint256) {
        uint256[2] memory result = (fixedValues = [value, uint256(11)]);
        return (result[0], fixedValues[1]);
    }
    function dynamicArray(uint256 value) external returns (uint256, uint256) {
        uint256[] memory input = new uint256[](2);
        input[0] = value; input[1] = 13;
        uint256[] memory result = (dynamicValues = input);
        return (result[0], dynamicValues[1]);
    }
    function byteArray(bytes memory input) external returns (bytes memory) {
        return (storedBytes = input);
    }
    function storageReference(uint256 value) external returns (uint256) {
        Cell storage ref = (stored = Cell(value, 9));
        ref.value += 1;
        return stored.value;
    }

    function fixedStorageReference(uint256 value) external returns (uint256, uint256) {
        fixedValues = [value, uint256(11)];
        uint256[2] storage ref = (otherFixed = fixedValues);
        ref[0] += 1;
        return (otherFixed[0], fixedValues[0]);
    }

    function reboundReference(uint256 value) external returns (uint256, uint256) {
        fixedValues = [value, uint256(11)];
        otherFixed = [uint256(7), uint256(13)];
        uint256[2] storage ref = fixedValues;
        uint256[2] storage rebound = (ref = otherFixed);
        rebound[0] += 1;
        return (otherFixed[0], fixedValues[0]);
    }
}
