// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    uint256 public cnt;
    uint256[] arr;
    function v(uint256 x) internal returns (uint256) { cnt++; return x; }
    // arr.push(v(7)) : v once -> cnt 1, arr=[7]
    function pushOnce() external returns (uint256, uint256) {
        cnt = 0; delete arr; arr.push(v(7)); return (arr[0], cnt);
    }
    // abi.encode(v(1), v(2)) then decode : each once -> cnt 2
    function encodeOnce() external returns (uint256, uint256, uint256) {
        cnt = 0;
        bytes memory e = abi.encode(v(1), v(2));
        (uint256 a, uint256 b) = abi.decode(e, (uint256, uint256));
        return (a, b, cnt);   // expect (1,2,2)
    }
    // keccak256(abi.encodePacked(v(5))) : v once -> cnt 1
    function packOnce() external returns (uint256) {
        cnt = 0; keccak256(abi.encodePacked(v(5))); return cnt;  // expect 1
    }
    // arr[i] where i=v(0) (side-effecting index read) once
    function idxReadOnce() external returns (uint256, uint256) {
        cnt = 0; delete arr; arr.push(42); uint256 x = arr[v(0)]; return (x, cnt); // (42,1)
    }
}
