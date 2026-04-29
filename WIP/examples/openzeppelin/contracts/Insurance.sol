// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

abstract contract Insurance {
    address public admin;
    uint256 public policyCount;
    uint256 public premiumRate;
    uint256 public totalPremiums;
    uint256 public totalPayouts;

    mapping(uint256 => address) private _policyHolder;
    mapping(uint256 => uint256) private _policyCoverage;
    mapping(uint256 => uint256) private _policyPremium;
    mapping(uint256 => bool) private _policyActive;
    mapping(uint256 => bool) private _policyClaimed;

    constructor(uint256 premiumRate_) {
        admin = msg.sender;
        premiumRate = premiumRate_;
    }

    function createPolicy(address holder, uint256 coverage) public returns (uint256) {
        policyCount = policyCount + 1;
        uint256 policyId = policyCount;
        uint256 premium = coverage * premiumRate / 1000;
        _policyHolder[policyId] = holder;
        _policyCoverage[policyId] = coverage;
        _policyPremium[policyId] = premium;
        _policyActive[policyId] = false;
        _policyClaimed[policyId] = false;
        return policyId;
    }

    function activatePolicy(uint256 policyId) public {
        _policyActive[policyId] = true;
        totalPremiums = totalPremiums + _policyPremium[policyId];
    }

    function fileClaim(uint256 policyId) public returns (uint256) {
        require(_policyActive[policyId], "Policy not active");
        require(!_policyClaimed[policyId], "Already claimed");
        _policyClaimed[policyId] = true;
        uint256 coverage = _policyCoverage[policyId];
        totalPayouts = totalPayouts + coverage;
        return coverage;
    }

    function cancelPolicy(uint256 policyId) public {
        require(msg.sender == admin, "Not admin");
        _policyActive[policyId] = false;
    }

    function getPolicyHolder(uint256 policyId) public view returns (address) {
        return _policyHolder[policyId];
    }

    function getPolicyCoverage(uint256 policyId) public view returns (uint256) {
        return _policyCoverage[policyId];
    }

    function getPolicyPremium(uint256 policyId) public view returns (uint256) {
        return _policyPremium[policyId];
    }

    function isPolicyActive(uint256 policyId) public view returns (bool) {
        return _policyActive[policyId];
    }

    function isPolicyClaimed(uint256 policyId) public view returns (bool) {
        return _policyClaimed[policyId];
    }

    function getTotalPremiums() public view returns (uint256) {
        return totalPremiums;
    }

    function getTotalPayouts() public view returns (uint256) {
        return totalPayouts;
    }

    function getPolicyCount() public view returns (uint256) {
        return policyCount;
    }

    function getAdmin() public view returns (address) {
        return admin;
    }
}

contract InsuranceTest is Insurance {
    constructor() Insurance(50) {}
}
