// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

abstract contract ConfigManager {
    address private _admin;
    uint256 private _configCount;
    uint256 private _updateCount;

    mapping(uint256 => uint256) internal _configValue;
    mapping(uint256 => bool) internal _configExists;

    constructor() {
        _admin = msg.sender;
        _configCount = 0;
        _updateCount = 0;
    }

    function getAdmin() external view returns (address) {
        return _admin;
    }

    function getConfigCount() external view returns (uint256) {
        return _configCount;
    }

    function getUpdateCount() external view returns (uint256) {
        return _updateCount;
    }

    function getConfig(uint256 configKey) external view returns (uint256) {
        return _configValue[configKey];
    }

    function isConfigSet(uint256 configKey) external view returns (bool) {
        return _configExists[configKey];
    }

    function setConfig(uint256 configKey, uint256 configValue) external {
        require(msg.sender == _admin, "Not admin");
        if (!_configExists[configKey]) {
            _configExists[configKey] = true;
            _configCount = _configCount + 1;
        }
        _configValue[configKey] = configValue;
        _updateCount = _updateCount + 1;
    }
}

contract ConfigManagerTest is ConfigManager {
    constructor() ConfigManager() {}

    function initConfig(uint256 configKey) external {
        _configValue[configKey] = 0;
        _configExists[configKey] = false;
    }
}
