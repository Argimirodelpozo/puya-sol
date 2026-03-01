// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

/**
 * @dev Software license management.
 * Admin issues licenses linked to a product hash, then can activate or revoke them.
 * Tracks active license count separately.
 */
abstract contract LicenseManager {
    address private _admin;
    uint256 private _licenseCount;
    uint256 private _activeLicenseCount;

    mapping(uint256 => uint256) internal _licenseProduct;
    mapping(uint256 => bool) internal _licenseActive;
    mapping(uint256 => bool) internal _licenseRevoked;

    constructor() {
        _admin = msg.sender;
        _licenseCount = 0;
        _activeLicenseCount = 0;
    }

    function getAdmin() external view returns (address) {
        return _admin;
    }

    function getLicenseCount() external view returns (uint256) {
        return _licenseCount;
    }

    function getActiveLicenseCount() external view returns (uint256) {
        return _activeLicenseCount;
    }

    function getProductHash(uint256 licenseId) external view returns (uint256) {
        return _licenseProduct[licenseId];
    }

    function isLicenseActive(uint256 licenseId) external view returns (bool) {
        return _licenseActive[licenseId];
    }

    function isLicenseRevoked(uint256 licenseId) external view returns (bool) {
        return _licenseRevoked[licenseId];
    }

    function issueLicense(uint256 productHash) external returns (uint256) {
        require(msg.sender == _admin, "Only admin");
        uint256 id = _licenseCount;
        _licenseProduct[id] = productHash;
        _licenseActive[id] = false;
        _licenseRevoked[id] = false;
        _licenseCount = id + 1;
        return id;
    }

    function activateLicense(uint256 licenseId) external {
        require(msg.sender == _admin, "Only admin");
        require(licenseId < _licenseCount, "License does not exist");
        require(!_licenseRevoked[licenseId], "License is revoked");
        require(!_licenseActive[licenseId], "License already active");
        _licenseActive[licenseId] = true;
        _activeLicenseCount = _activeLicenseCount + 1;
    }

    function revokeLicense(uint256 licenseId) external {
        require(msg.sender == _admin, "Only admin");
        require(licenseId < _licenseCount, "License does not exist");
        require(!_licenseRevoked[licenseId], "License already revoked");
        if (_licenseActive[licenseId]) {
            _licenseActive[licenseId] = false;
            _activeLicenseCount = _activeLicenseCount - 1;
        }
        _licenseRevoked[licenseId] = true;
    }
}

contract LicenseManagerTest is LicenseManager {
    constructor() LicenseManager() {}

    function initLicense(uint256 licenseId) external {
        _licenseProduct[licenseId] = 0;
        _licenseActive[licenseId] = false;
        _licenseRevoked[licenseId] = false;
    }
}
