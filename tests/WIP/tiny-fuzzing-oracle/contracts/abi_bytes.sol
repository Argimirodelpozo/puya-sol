// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    function rtUB(uint256 a, bytes calldata b) external pure returns (uint256, bytes memory) {
        return abi.decode(abi.encode(a, b), (uint256, bytes));        // encode/decode round-trip
    }
    function rtStr(string calldata s) external pure returns (string memory) {
        return abi.decode(abi.encode(s), (string));
    }
    function rtArr(uint256[] calldata a) external pure returns (uint256[] memory) {
        return abi.decode(abi.encode(a), (uint256[]));
    }
    function packMixed(uint8 a, bytes1 b, uint16 c) external pure returns (bytes memory) {
        return abi.encodePacked(a, b, c);
    }
}
