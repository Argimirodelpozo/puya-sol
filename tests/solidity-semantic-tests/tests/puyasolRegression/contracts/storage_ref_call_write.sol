// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;
contract StorageRefCallWrite {
    struct P { uint256 n; uint256 m; }
    mapping(uint256 => P) ps;
    P single;
    function _p(uint256 id) internal view returns (P storage) { return ps[id]; }
    function bump(uint256 id) external { _p(id).n += 1; }
    function set(uint256 id, uint256 v) external { _p(id).m = v; }
    function read(uint256 id) external view returns (uint256, uint256) { return (_p(id).n, _p(id).m); }
}
