// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract MemParam {
    struct S { uint256 x; uint256 y; }
    function _mutArr(uint256[] memory a) internal pure { a[0] = 11; }
    function _mutStruct(S memory s) internal pure { s.x = 11; }
    // EVM: memory args are references → callee mutations propagate to the caller.
    function arrParam() external pure returns (uint256) {
        uint256[] memory x = new uint256[](1); x[0] = 5;
        _mutArr(x); return x[0];        // EVM: 11
    }
    function structParam() external pure returns (uint256) {
        S memory s = S(5, 0);
        _mutStruct(s); return s.x;      // EVM: 11
    }
    function noMutation() external pure returns (uint256) {
        uint256[] memory x = new uint256[](1); x[0] = 7;
        _readArr(x); return x[0];       // EVM: 7 (callee doesn't mutate)
    }
    function _readArr(uint256[] memory a) internal pure returns (uint256) { return a[0]; }
}
