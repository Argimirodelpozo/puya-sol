// SPDX-License-Identifier: MIT
pragma solidity ^0.8.4;

import "../contracts/utils/EnumerableMapLib.sol";

contract EnumerableMapWrapper {
    using EnumerableMapLib for EnumerableMapLib.Uint256ToUint256Map;

    EnumerableMapLib.Uint256ToUint256Map private map;

    function set(uint256 key, uint256 value) external returns (bool) {
        return map.set(key, value);
    }

    function get(uint256 key) external view returns (uint256) {
        return map.get(key);
    }

    function remove(uint256 key) external returns (bool) {
        return map.remove(key);
    }

    function contains(uint256 key) external view returns (bool) {
        return map.contains(key);
    }

    function length() external view returns (uint256) {
        return map.length();
    }
}
