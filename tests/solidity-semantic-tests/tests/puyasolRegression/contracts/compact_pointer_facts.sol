// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

contract CompactPointerFacts {
    function value(uint64 n) external pure returns (uint64) { return n + 7; }
    function addressOf(address target) external pure returns (uint256) {
        function(uint64) external pure returns (uint64) pointer = CompactPointerFacts(target).value;
        return uint256(uint160(pointer.address));
    }
    function cross(address target, uint64 n) external pure returns (uint64) {
        function(uint64) external pure returns (uint64) pointer = CompactPointerFacts(target).value;
        return pointer(n);
    }
    function self(uint64 n) external view returns (uint64) {
        function(uint64) external pure returns (uint64) pointer = this.value;
        return pointer(n);
    }
}
