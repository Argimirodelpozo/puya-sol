// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    uint256 public cnt;
    function mkStr() internal returns (string memory) { cnt++; return "hello"; }
    function mkArr() internal returns (uint256[] memory a) { cnt++; a = new uint256[](2); a[0]=7; a[1]=9; }
    // abi.encode(dynamic) must evaluate the dynamic arg ONCE.
    function encStrOnce() external returns (uint256, bytes32) {
        cnt = 0;
        bytes memory e = abi.encode(mkStr());
        return (cnt, keccak256(e));   // cnt expect 1
    }
    function encArrOnce() external returns (uint256, uint256) {
        cnt = 0;
        bytes memory e = abi.encode(mkArr());
        (uint256[] memory back) = abi.decode(e, (uint256[]));
        return (cnt, back[1]);        // expect (1, 9)
    }
}
