// SPDX-License-Identifier: GPL-3.0
pragma solidity ^0.8.0;

/// @title Minimal Factory mock for SushiSwap V2 Pair testing.
/// Provides feeTo() and migrator() that the Pair reads during mint/burn.
contract FactoryMock {
    address public feeTo;
    address public migrator;

    function setFeeTo(address _feeTo) external {
        feeTo = _feeTo;
    }

    function setMigrator(address _migrator) external {
        migrator = _migrator;
    }
}
