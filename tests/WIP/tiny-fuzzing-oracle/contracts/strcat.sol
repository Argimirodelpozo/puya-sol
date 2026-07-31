// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;
contract SC {
    function viaLocal(string memory n) external pure returns (string memory) {
        string memory s = string.concat("x", n);
        return s;
    }
    function direct(string memory n) external pure returns (string memory) {
        return string.concat("a", n, "b");
    }
    function chained(string memory a, string memory b) external pure returns (string memory) {
        string memory s = string.concat(a, "-", b);
        string memory t = string.concat(s, s);
        return t;
    }
    function lenOf(string memory n) external pure returns (uint256) {
        string memory s = string.concat("pre", n);
        return bytes(s).length;
    }
    function bcat(bytes memory a) external pure returns (bytes memory) {
        return bytes.concat(a, hex"ff");
    }
    function toBytes(string memory n) external pure returns (bytes memory) {
        return bytes(string.concat("z", n));
    }
}
