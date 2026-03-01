// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

/**
 * @dev Simple registry that maps names (uint256 hashes) to addresses and vice versa.
 * Supports registration, transfer, and lookup.
 */
contract RegistryTest {
    address private _owner;
    uint256 private _registrationCount;
    uint256 private _registrationFee;

    mapping(uint256 => address) private _nameToOwner;
    mapping(uint256 => uint256) private _nameToExpiry;
    mapping(address => uint256) private _primaryName;

    constructor() {
        _owner = msg.sender;
        _registrationFee = 100;
    }

    function owner() external view returns (address) {
        return _owner;
    }

    function registrationCount() external view returns (uint256) {
        return _registrationCount;
    }

    function registrationFee() external view returns (uint256) {
        return _registrationFee;
    }

    function setRegistrationFee(uint256 fee) external {
        require(msg.sender == _owner, "Not owner");
        _registrationFee = fee;
    }

    function register(uint256 nameHash, address registrant, uint256 duration) external {
        require(_nameToOwner[nameHash] == address(0), "Name taken");
        require(duration > 0, "Duration must be > 0");

        _nameToOwner[nameHash] = registrant;
        _nameToExpiry[nameHash] = duration;
        _registrationCount += 1;
    }

    function ownerOfName(uint256 nameHash) external view returns (address) {
        return _nameToOwner[nameHash];
    }

    function expiryOf(uint256 nameHash) external view returns (uint256) {
        return _nameToExpiry[nameHash];
    }

    function primaryName(address account) external view returns (uint256) {
        return _primaryName[account];
    }

    function setPrimaryName(address account, uint256 nameHash) external {
        require(_nameToOwner[nameHash] == account, "Not name owner");
        _primaryName[account] = nameHash;
    }

    function transferName(uint256 nameHash, address from, address to) external {
        require(_nameToOwner[nameHash] == from, "Not name owner");
        _nameToOwner[nameHash] = to;

        // Clear primary name if it was set for the sender
        if (_primaryName[from] == nameHash) {
            _primaryName[from] = 0;
        }
    }

    function renewName(uint256 nameHash, uint256 additionalDuration) external {
        require(_nameToOwner[nameHash] != address(0), "Name not registered");
        _nameToExpiry[nameHash] += additionalDuration;
    }

    function releaseName(uint256 nameHash) external {
        require(_nameToOwner[nameHash] != address(0), "Name not registered");
        address nameOwner = _nameToOwner[nameHash];

        if (_primaryName[nameOwner] == nameHash) {
            _primaryName[nameOwner] = 0;
        }

        _nameToOwner[nameHash] = address(0);
        _nameToExpiry[nameHash] = 0;
        _registrationCount -= 1;
    }
}
