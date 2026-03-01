// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

/**
 * @dev Document management/notary system.
 * Documents are stored by hash and can be verified or revoked by admin.
 */
abstract contract DocumentStore {
    address private _admin;
    uint256 private _documentCount;
    uint256 private _verifiedCount;

    mapping(uint256 => uint256) internal _docHash;
    mapping(uint256 => bool) internal _docVerified;
    mapping(uint256 => bool) internal _docRevoked;

    constructor() {
        _admin = msg.sender;
        _documentCount = 0;
        _verifiedCount = 0;
    }

    function getAdmin() external view returns (address) {
        return _admin;
    }

    function getDocumentCount() external view returns (uint256) {
        return _documentCount;
    }

    function getVerifiedCount() external view returns (uint256) {
        return _verifiedCount;
    }

    function getDocumentHash(uint256 docId) external view returns (uint256) {
        return _docHash[docId];
    }

    function isDocumentVerified(uint256 docId) external view returns (bool) {
        return _docVerified[docId];
    }

    function isDocumentRevoked(uint256 docId) external view returns (bool) {
        return _docRevoked[docId];
    }

    function storeDocument(uint256 docHash) external returns (uint256) {
        uint256 id = _documentCount;
        _docHash[id] = docHash;
        _docVerified[id] = false;
        _docRevoked[id] = false;
        _documentCount = id + 1;
        return id;
    }

    function verifyDocument(uint256 docId) external {
        require(msg.sender == _admin, "Not admin");
        require(docId < _documentCount, "Document does not exist");
        require(_docRevoked[docId] == false, "Document is revoked");
        require(_docVerified[docId] == false, "Already verified");
        _docVerified[docId] = true;
        _verifiedCount = _verifiedCount + 1;
    }

    function revokeDocument(uint256 docId) external {
        require(msg.sender == _admin, "Not admin");
        require(docId < _documentCount, "Document does not exist");
        require(_docRevoked[docId] == false, "Already revoked");
        _docRevoked[docId] = true;
        if (_docVerified[docId] == true) {
            _verifiedCount = _verifiedCount - 1;
        }
    }
}

contract DocumentStoreTest is DocumentStore {
    constructor() DocumentStore() {}

    function initDocument(uint256 docId) external {
        _docHash[docId] = 0;
        _docVerified[docId] = false;
        _docRevoked[docId] = false;
    }
}
