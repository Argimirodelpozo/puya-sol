// SPDX-License-Identifier: MIT
pragma solidity ^0.8.4;

import "../contracts/utils/CREATE3.sol";

contract CREATE3Wrapper {
    function predictDeterministicAddress(bytes32 salt) external view returns (address) {
        return CREATE3.predictDeterministicAddress(salt);
    }
}
