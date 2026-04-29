// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

abstract contract Certificate {
    address private _issuer;
    uint256 private _certCount;
    uint256 private _totalIssued;
    uint256 private _totalRevoked;

    mapping(uint256 => address) internal _certRecipient;
    mapping(uint256 => uint256) internal _certScore;
    mapping(uint256 => uint256) internal _certIssuedAt;
    mapping(uint256 => bool) internal _certRevoked;
    mapping(uint256 => bool) internal _certExists;

    constructor() {
        _issuer = msg.sender;
        _certCount = 0;
        _totalIssued = 0;
        _totalRevoked = 0;
    }

    function getIssuer() external view returns (address) {
        return _issuer;
    }

    function getCertCount() external view returns (uint256) {
        return _certCount;
    }

    function getTotalIssued() external view returns (uint256) {
        return _totalIssued;
    }

    function getTotalRevoked() external view returns (uint256) {
        return _totalRevoked;
    }

    function getCertRecipient(uint256 certId) external view returns (address) {
        return _certRecipient[certId];
    }

    function getCertScore(uint256 certId) external view returns (uint256) {
        return _certScore[certId];
    }

    function getCertIssuedAt(uint256 certId) external view returns (uint256) {
        return _certIssuedAt[certId];
    }

    function isCertRevoked(uint256 certId) external view returns (bool) {
        return _certRevoked[certId];
    }

    function certExists(uint256 certId) external view returns (bool) {
        return _certExists[certId];
    }

    function issueCert(address recipient, uint256 score, uint256 issuedAt) external returns (uint256) {
        require(score <= 100, "Score must be <= 100");

        uint256 certId = _certCount;
        _certRecipient[certId] = recipient;
        _certScore[certId] = score;
        _certIssuedAt[certId] = issuedAt;
        _certRevoked[certId] = false;
        _certExists[certId] = true;

        _certCount = certId + 1;
        _totalIssued = _totalIssued + 1;

        return certId;
    }

    function revokeCert(uint256 certId) external {
        require(_certExists[certId], "Certificate does not exist");
        require(!_certRevoked[certId], "Certificate already revoked");

        _certRevoked[certId] = true;
        _totalRevoked = _totalRevoked + 1;
    }

    function isValid(uint256 certId) external view returns (bool) {
        return _certExists[certId] && !_certRevoked[certId];
    }
}

contract CertificateTest is Certificate {
    constructor() Certificate() {}

    function initCert(uint256 certId) external {
        _certRecipient[certId] = address(0);
        _certScore[certId] = 0;
        _certIssuedAt[certId] = 0;
        _certRevoked[certId] = false;
        _certExists[certId] = false;
    }
}
