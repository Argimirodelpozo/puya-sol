// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

contract LazyPages {
    uint256[1025] public pages;
    function read(uint256 i) external view returns (uint256) { return pages[i]; }
    function set(uint256 i, uint256 v) external { pages[i] = v; }
    function add(uint256 i, uint256 v) external { pages[i] += v; }
    function clear() external { delete pages; }
    function clearElement(uint256 i) external { delete pages[i]; }
}
