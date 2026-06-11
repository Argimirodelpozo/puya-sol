// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    uint256 public cnt;
    function sz() internal returns (uint256) { cnt++; return 3; }
    // new uint256[](sz()) : sz() must run ONCE. The runtime-sized path lowers
    // to a while-loop; with the raw size expr inlined in the loop condition it
    // re-evaluated per iteration (cnt == iterations+1 instead of 1).
    function newArrOnce() external returns (uint256, uint256) {
        cnt = 0;
        uint256[] memory a = new uint256[](sz());
        return (a.length, cnt);   // expect (3, 1)
    }
    // bool[] runtime-size special case referenced the size twice (len header
    // + byte-length computation) -> cnt was 2.
    function newBoolArrOnce() external returns (uint256, uint256) {
        cnt = 0;
        bool[] memory b = new bool[](sz());
        return (b.length, cnt);   // expect (3, 1)
    }
}
