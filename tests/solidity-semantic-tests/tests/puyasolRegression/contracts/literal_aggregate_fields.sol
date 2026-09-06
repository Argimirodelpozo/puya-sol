// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;
contract LiteralAggregateFields {
    struct S { bytes3 b; uint8 n; }
    struct T { address a; }
    struct Pool { uint256 total; mapping(address => uint256) owed; }
    Pool[] public pools;
    S public s;
    function mk() external pure returns (S memory) { return S(0x010203, 7); }
    function mkNamed() external pure returns (S memory) { return S({b: 0x010203, n: 9}); }
    function mkT() external pure returns (T memory) { return T(0x1111111111111111111111111111111111111111); }
    function arr() external pure returns (bytes3[2] memory a) { a = [bytes3(0x010203), 0x040506]; }
    function store() external { s = S(0x0a0b0c, 3); }
    function addPool(uint256 t) external { pools.push().total = t; }
    function poolTotal(uint256 i) external view returns (uint256) { return pools[i].total; }
}
