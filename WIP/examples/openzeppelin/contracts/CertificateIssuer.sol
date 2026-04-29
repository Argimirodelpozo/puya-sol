// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

/**
 * @dev Digital certificate issuance and verification.
 * Admin issues certificates by hash, verifies them, and can revoke them.
 * Tracks total certificates and revoked count separately.
 */
abstract contract CertificateIssuer {
    address private _admin;
    uint256 private _certCount;
    uint256 private _revokedCount;

    mapping(uint256 => uint256) internal _certHash;
    mapping(uint256 => bool) internal _certIssued;
    mapping(uint256 => bool) internal _certRevoked;

    constructor() {
        _admin = msg.sender;
        _certCount = 0;
        _revokedCount = 0;
    }

    function getAdmin() external view returns (address) {
        return _admin;
    }

    function getCertCount() external view returns (uint256) {
        return _certCount;
    }

    function getRevokedCount() external view returns (uint256) {
        return _revokedCount;
    }

    function getCertHash(uint256 certId) external view returns (uint256) {
        return _certHash[certId];
    }

    function issueCertificate(uint256 certHash) external returns (uint256) {
        require(msg.sender == _admin, "Only admin");
        uint256 id = _certCount;
        _certHash[id] = certHash;
        _certIssued[id] = true;
        _certRevoked[id] = false;
        _certCount = id + 1;
        return id;
    }

    function verifyCertificate(uint256 certId) external view returns (bool) {
        require(certId < _certCount, "Certificate does not exist");
        return _certIssued[certId] && !_certRevoked[certId];
    }

    function revokeCertificate(uint256 certId) external {
        require(msg.sender == _admin, "Only admin");
        require(certId < _certCount, "Certificate does not exist");
        require(_certIssued[certId], "Certificate not issued");
        require(!_certRevoked[certId], "Certificate already revoked");
        _certRevoked[certId] = true;
        _revokedCount = _revokedCount + 1;
    }
}

contract CertificateIssuerTest is CertificateIssuer {
    constructor() CertificateIssuer() {}

    function initCert(uint256 certId) external {
        _certHash[certId] = 0;
        _certIssued[certId] = false;
        _certRevoked[certId] = false;
    }
}
