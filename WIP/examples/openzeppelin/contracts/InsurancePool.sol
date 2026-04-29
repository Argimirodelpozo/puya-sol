// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

abstract contract InsurancePool {
    address private _admin;
    uint256 private _policyCount;
    uint256 private _totalPremiums;

    mapping(uint256 => uint256) internal _policyPremium;
    mapping(uint256 => uint256) internal _policyCoverage;
    mapping(uint256 => bool) internal _policyActive;
    mapping(uint256 => uint256) internal _policyClaims;

    constructor() {
        _admin = msg.sender;
        _policyCount = 0;
        _totalPremiums = 0;
    }

    function getAdmin() external view returns (address) {
        return _admin;
    }

    function getPolicyCount() external view returns (uint256) {
        return _policyCount;
    }

    function getTotalPremiums() external view returns (uint256) {
        return _totalPremiums;
    }

    function getPremium(uint256 policyId) external view returns (uint256) {
        return _policyPremium[policyId];
    }

    function getCoverage(uint256 policyId) external view returns (uint256) {
        return _policyCoverage[policyId];
    }

    function isPolicyActive(uint256 policyId) external view returns (bool) {
        return _policyActive[policyId];
    }

    function getClaimCount(uint256 policyId) external view returns (uint256) {
        return _policyClaims[policyId];
    }

    function createPolicy(uint256 premium, uint256 coverage) external returns (uint256) {
        uint256 id = _policyCount;
        _policyPremium[id] = premium;
        _policyCoverage[id] = coverage;
        _policyActive[id] = false;
        _policyClaims[id] = 0;
        _policyCount = id + 1;
        return id;
    }

    function activatePolicy(uint256 policyId) external {
        require(msg.sender == _admin, "Not admin");
        require(!_policyActive[policyId], "Already active");
        _policyActive[policyId] = true;
        _totalPremiums = _totalPremiums + _policyPremium[policyId];
    }

    function fileClaim(uint256 policyId) external {
        require(_policyActive[policyId], "Policy not active");
        _policyClaims[policyId] = _policyClaims[policyId] + 1;
    }
}

contract InsurancePoolTest is InsurancePool {
    constructor() InsurancePool() {}

    function initPolicy(uint256 policyId) external {
        _policyPremium[policyId] = 0;
        _policyCoverage[policyId] = 0;
        _policyActive[policyId] = false;
        _policyClaims[policyId] = 0;
    }
}
