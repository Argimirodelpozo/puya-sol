// SPDX-License-Identifier: MIT
pragma solidity >=0.8.28;
contract TransientCapacity {
    uint256 transient a;
    uint256 transient b;
    uint256 transient c;
    uint256 transient d;
    uint256 transient e;
    uint256 transient f;
    function set(uint256 value) external { f = value; }
}
