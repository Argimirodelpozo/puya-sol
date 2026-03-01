// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

/**
 * @dev Entity rating with weighted scores.
 * Admin registers entities by hash. Users submit reviews with a score.
 * Average score computed as totalScore / reviewCount per entity.
 */
abstract contract RatingSystem {
    address private _admin;
    uint256 private _entityCount;
    uint256 private _globalReviews;

    mapping(uint256 => uint256) internal _entityHash;
    mapping(uint256 => uint256) internal _totalScore;
    mapping(uint256 => uint256) internal _reviewCount;

    constructor() {
        _admin = msg.sender;
        _entityCount = 0;
        _globalReviews = 0;
    }

    function getAdmin() external view returns (address) {
        return _admin;
    }

    function getEntityCount() external view returns (uint256) {
        return _entityCount;
    }

    function getGlobalReviews() external view returns (uint256) {
        return _globalReviews;
    }

    function getEntityHash(uint256 entityId) external view returns (uint256) {
        return _entityHash[entityId];
    }

    function getReviewCount(uint256 entityId) external view returns (uint256) {
        return _reviewCount[entityId];
    }

    function registerEntity(uint256 entityHash) external returns (uint256) {
        require(msg.sender == _admin, "Only admin");
        uint256 id = _entityCount;
        _entityHash[id] = entityHash;
        _totalScore[id] = 0;
        _reviewCount[id] = 0;
        _entityCount = id + 1;
        return id;
    }

    function submitReview(uint256 entityId, uint256 score) external {
        require(entityId < _entityCount, "Entity does not exist");
        require(score > 0, "Score must be positive");
        _totalScore[entityId] = _totalScore[entityId] + score;
        _reviewCount[entityId] = _reviewCount[entityId] + 1;
        _globalReviews = _globalReviews + 1;
    }

    function getAverageScore(uint256 entityId) external view returns (uint256) {
        require(_reviewCount[entityId] > 0, "No reviews yet");
        return _totalScore[entityId] / _reviewCount[entityId];
    }
}

contract RatingSystemTest is RatingSystem {
    constructor() RatingSystem() {}

    function initEntity(uint256 entityId) external {
        _entityHash[entityId] = 0;
        _totalScore[entityId] = 0;
        _reviewCount[entityId] = 0;
    }
}
