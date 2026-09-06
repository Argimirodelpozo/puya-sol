// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;
contract PushElementReference {
    struct E { uint256 f; uint256 g; }
    struct H { E[] inner; }
    mapping(uint256 => E[]) m;
    E[] arr;
    mapping(uint256 => H) h;
    function boxed(uint256 k) external returns (uint256) { m[k].push().f = 7; return m[k][0].f; }
    function alias_() external returns (uint256) { E[] storage al = arr; al.push().f = 8; return arr[0].f; }
    function chained(uint256 k) external returns (uint256) { h[k].inner.push().f = 9; return h[k].inner[0].f; }
    function popAlias() external returns (uint256) { E[] storage al = arr; al.pop(); return arr.length; }
}
