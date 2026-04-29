// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

/**
 * @dev Energy consumption tracking.
 * Admin registers meters by hash. Readings are recorded per meter,
 * tracking total consumption per meter and a global consumption sum.
 */
abstract contract EnergyMeter {
    address private _admin;
    uint256 private _meterCount;
    uint256 private _globalConsumption;

    mapping(uint256 => uint256) internal _meterHash;
    mapping(uint256 => uint256) internal _meterConsumption;
    mapping(uint256 => uint256) internal _meterReadings;

    constructor() {
        _admin = msg.sender;
        _meterCount = 0;
        _globalConsumption = 0;
    }

    function getAdmin() external view returns (address) {
        return _admin;
    }

    function getMeterCount() external view returns (uint256) {
        return _meterCount;
    }

    function getGlobalConsumption() external view returns (uint256) {
        return _globalConsumption;
    }

    function getMeterHash(uint256 meterId) external view returns (uint256) {
        return _meterHash[meterId];
    }

    function getTotalConsumption(uint256 meterId) external view returns (uint256) {
        return _meterConsumption[meterId];
    }

    function getReadingCount(uint256 meterId) external view returns (uint256) {
        return _meterReadings[meterId];
    }

    function registerMeter(uint256 meterHash) external returns (uint256) {
        require(msg.sender == _admin, "Only admin");
        uint256 id = _meterCount;
        _meterHash[id] = meterHash;
        _meterConsumption[id] = 0;
        _meterReadings[id] = 0;
        _meterCount = id + 1;
        return id;
    }

    function recordReading(uint256 meterId, uint256 readingValue) external {
        require(meterId < _meterCount, "Meter does not exist");
        require(readingValue > 0, "Reading must be positive");
        _meterConsumption[meterId] = _meterConsumption[meterId] + readingValue;
        _meterReadings[meterId] = _meterReadings[meterId] + 1;
        _globalConsumption = _globalConsumption + readingValue;
    }
}

contract EnergyMeterTest is EnergyMeter {
    constructor() EnergyMeter() {}

    function initMeter(uint256 meterId) external {
        _meterHash[meterId] = 0;
        _meterConsumption[meterId] = 0;
        _meterReadings[meterId] = 0;
    }
}
