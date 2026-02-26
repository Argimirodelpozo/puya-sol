// SPDX-License-Identifier: MIT
pragma solidity >=0.7.0;

contract StructStorageTest {
    struct Config {
        uint256 threshold;
        bool active;
    }

    Config public config;

    function setConfig(uint256 threshold, bool active) external {
        config = Config(threshold, active);
    }

    function updateThreshold(uint256 newThreshold) external {
        config.threshold = newThreshold;
    }

    function getThreshold() external view returns (uint256) {
        return config.threshold;
    }

    function isActive() external view returns (bool) {
        return config.active;
    }
}
