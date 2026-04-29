// SPDX-License-Identifier: MIT
pragma solidity ^0.8.4;

import "../contracts/utils/EnumerableSetLib.sol";

contract EnumerableSetWrapper {
    using EnumerableSetLib for EnumerableSetLib.Uint256Set;

    EnumerableSetLib.Uint256Set private set;

    function add(uint256 value) external returns (bool) {
        return set.add(value);
    }

    function remove(uint256 value) external returns (bool) {
        return set.remove(value);
    }

    function contains(uint256 value) external view returns (bool) {
        return set.contains(value);
    }

    function length() external view returns (uint256) {
        return set.length();
    }

    function at(uint256 index) external view returns (uint256) {
        return set.at(index);
    }
}
