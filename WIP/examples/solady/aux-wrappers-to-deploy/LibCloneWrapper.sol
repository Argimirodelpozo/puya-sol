// SPDX-License-Identifier: MIT
pragma solidity ^0.8.4;

import "../contracts/utils/LibClone.sol";

contract LibCloneWrapper {
    function clone(address implementation) external returns (address) {
        return LibClone.clone(implementation);
    }

    function predictDeterministicAddress(address implementation, bytes32 salt, address deployer) external pure returns (address) {
        return LibClone.predictDeterministicAddress(implementation, salt, deployer);
    }
}
