// SPDX-License-Identifier: MIT
pragma solidity ^0.8.4;

import "../contracts/utils/LibMap.sol";

contract LibMapWrapper {
    using LibMap for LibMap.Uint8Map;

    LibMap.Uint8Map private map;

    function set(uint256 index, uint8 value) external {
        map.set(index, value);
    }

    function get(uint256 index) external view returns (uint8) {
        return map.get(index);
    }
}
