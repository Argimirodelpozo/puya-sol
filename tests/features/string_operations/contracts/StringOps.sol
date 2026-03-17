// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

contract StringOps {
    string public storedString;

    function setString(string memory s) external {
        storedString = s;
    }

    function getString() external view returns (string memory) {
        return storedString;
    }

    function returnLiteral() external pure returns (string memory) {
        return "hello world";
    }

    function stringLength(string memory s) external pure returns (uint256) {
        return bytes(s).length;
    }

    function compareStrings(string memory a, string memory b) external pure returns (bool) {
        return keccak256(bytes(a)) == keccak256(bytes(b));
    }

    function concatStrings(string memory a, string memory b) external pure returns (string memory) {
        return string(abi.encodePacked(a, b));
    }
}
