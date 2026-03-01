// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

/**
 * @dev Warranty management system on the Algorand AVM.
 * Admin registers products with expiry timestamps, users can claim
 * warranties, and admin can void warranties.
 */
abstract contract Warranty {
    address private _admin;
    uint256 private _productCount;

    mapping(uint256 => uint256) internal _productExpiry;
    mapping(uint256 => bool) internal _productVoided;
    mapping(uint256 => uint256) internal _productClaims;

    constructor() {
        _admin = msg.sender;
        _productCount = 0;
    }

    function getAdmin() public view returns (address) {
        return _admin;
    }

    function getProductCount() public view returns (uint256) {
        return _productCount;
    }

    function getExpiryTime(uint256 productId) public view returns (uint256) {
        return _productExpiry[productId];
    }

    function isWarrantyValid(uint256 productId) public view returns (bool) {
        require(productId < _productCount, "Warranty: product does not exist");
        return !_productVoided[productId];
    }

    function isWarrantyVoided(uint256 productId) public view returns (bool) {
        return _productVoided[productId];
    }

    function getClaimCount(uint256 productId) public view returns (uint256) {
        return _productClaims[productId];
    }

    function registerProduct(uint256 expiryTimestamp) public returns (uint256) {
        require(msg.sender == _admin, "Warranty: not admin");
        require(expiryTimestamp > 0, "Warranty: invalid expiry");
        uint256 productId = _productCount;
        _productExpiry[productId] = expiryTimestamp;
        _productVoided[productId] = false;
        _productClaims[productId] = 0;
        _productCount = productId + 1;
        return productId;
    }

    function claimWarranty(uint256 productId) public {
        require(productId < _productCount, "Warranty: product does not exist");
        require(!_productVoided[productId], "Warranty: warranty voided");
        _productClaims[productId] = _productClaims[productId] + 1;
    }

    function voidWarranty(uint256 productId) public {
        require(msg.sender == _admin, "Warranty: not admin");
        require(productId < _productCount, "Warranty: product does not exist");
        require(!_productVoided[productId], "Warranty: already voided");
        _productVoided[productId] = true;
    }
}

contract WarrantyTest is Warranty {
    constructor() Warranty() {}

    function initProduct(uint256 productId) public {
        _productExpiry[productId] = 0;
        _productVoided[productId] = false;
        _productClaims[productId] = 0;
    }
}
