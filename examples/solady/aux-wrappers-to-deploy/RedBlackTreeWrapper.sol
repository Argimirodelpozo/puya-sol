// SPDX-License-Identifier: MIT
pragma solidity ^0.8.4;

import "../contracts/utils/RedBlackTreeLib.sol";

contract RedBlackTreeWrapper {
    using RedBlackTreeLib for RedBlackTreeLib.Tree;

    RedBlackTreeLib.Tree private tree;

    function insert(uint256 value) external {
        tree.insert(value);
    }

    function remove(uint256 value) external {
        tree.remove(value);
    }

    function exists(uint256 value) external view returns (bool) {
        return tree.exists(value);
    }

    function size() external view returns (uint256) {
        return tree.size();
    }
}
