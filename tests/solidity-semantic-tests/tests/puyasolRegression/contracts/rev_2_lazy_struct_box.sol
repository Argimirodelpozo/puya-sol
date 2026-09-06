// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

contract LazyStructBox {
    struct Large { uint256 first; uint256[129] padding; uint16 last; }
    mapping(uint256 => Large) public roots;
    function read(uint256 k) external view returns (uint256, uint256, uint16) {
        Large storage r = roots[k];
        return (r.first, r.padding[128], r.last);
    }
    function set(uint256 k) external { roots[k].first = 11; roots[k].padding[128] = 22; roots[k].last = 33; }
    function clear(uint256 k) external { delete roots[k]; }
}
