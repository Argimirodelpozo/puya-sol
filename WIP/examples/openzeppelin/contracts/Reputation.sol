// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

/**
 * @dev Reputation/rating system for entities.
 * Entities are registered with a hash and can receive positive or negative reviews.
 * Net score is computed as positive minus negative count.
 */
abstract contract Reputation {
    address private _admin;
    uint256 private _entityCount;
    uint256 private _totalReviews;

    mapping(uint256 => uint256) internal _entityHash;
    mapping(uint256 => uint256) internal _positiveReviews;
    mapping(uint256 => uint256) internal _negativeReviews;

    constructor() {
        _admin = msg.sender;
        _entityCount = 0;
        _totalReviews = 0;
    }

    function getAdmin() external view returns (address) {
        return _admin;
    }

    function getEntityCount() external view returns (uint256) {
        return _entityCount;
    }

    function getTotalReviews() external view returns (uint256) {
        return _totalReviews;
    }

    function getEntityHash(uint256 entityId) external view returns (uint256) {
        return _entityHash[entityId];
    }

    function getPositiveCount(uint256 entityId) external view returns (uint256) {
        return _positiveReviews[entityId];
    }

    function getNegativeCount(uint256 entityId) external view returns (uint256) {
        return _negativeReviews[entityId];
    }

    function getNetScore(uint256 entityId) external view returns (uint256) {
        return _positiveReviews[entityId] - _negativeReviews[entityId];
    }

    function registerEntity(uint256 entityHash) external returns (uint256) {
        uint256 id = _entityCount;
        _entityHash[id] = entityHash;
        _positiveReviews[id] = 0;
        _negativeReviews[id] = 0;
        _entityCount = id + 1;
        return id;
    }

    function addPositiveReview(uint256 entityId) external {
        require(entityId < _entityCount, "Entity does not exist");
        _positiveReviews[entityId] = _positiveReviews[entityId] + 1;
        _totalReviews = _totalReviews + 1;
    }

    function addNegativeReview(uint256 entityId) external {
        require(entityId < _entityCount, "Entity does not exist");
        _negativeReviews[entityId] = _negativeReviews[entityId] + 1;
        _totalReviews = _totalReviews + 1;
    }
}

contract ReputationTest is Reputation {
    constructor() Reputation() {}

    function initEntity(uint256 entityId) external {
        _entityHash[entityId] = 0;
        _positiveReviews[entityId] = 0;
        _negativeReviews[entityId] = 0;
    }
}
