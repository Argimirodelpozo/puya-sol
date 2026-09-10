// SPDX-License-Identifier: MIT
pragma solidity ^0.8.34;

contract ExpressionBounds {
    function boolSlice(bool[] calldata data, uint256 start, uint256 end) external pure returns (bool, bool) {
        bool[] calldata part = data[start:end];
        return (part[0], part[part.length - 1]);
    }
    function slice(bytes calldata data, uint256 start, uint256 end) external pure returns (bytes memory) {
        return data[start:end];
    }
    function discardSlice(bytes calldata data, uint256 start, uint256 end) external pure returns (uint256) {
        data[start:end];
        return 7;
    }
    function blobIndex(uint256 index, bool write) external pure returns (uint256) {
        uint256[2] memory values = [uint256(42), uint256(55)];
        assembly { mstore(values, 42) }
        if (write) values[index] = 77;
        return values[index];
    }
    function arraySlice(uint256[] calldata data, uint256 start, uint256 end) external pure returns (uint256) {
        uint256[] calldata part = data[start:end];
        return part.length == 0 ? 0 : part[0] + part[part.length - 1];
    }
    function nestedSlice(int8[] calldata data, uint256 start, uint256 end, uint256 index) external pure returns (int256) {
        return data[start:end][:][index];
    }
    function whole(uint256[] calldata data) external pure returns (uint256) {
        uint256[] calldata part = data[:];
        return part.length == 0 ? 0 : part[0] + part[part.length - 1];
    }
}
