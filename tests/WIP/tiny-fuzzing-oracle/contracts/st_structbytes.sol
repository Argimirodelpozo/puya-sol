// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    struct S { uint256 n; bytes data; bytes32 h; int128 sig; }
    S public s;                                  // struct getter with a bytes field + signed field
    function setS(uint256 n, bytes calldata d, bytes32 h, int128 sg) external { s = S(n, d, h, sg); }
    function grow(bytes calldata d) external { s.data = bytes.concat(s.data, d); }
}
