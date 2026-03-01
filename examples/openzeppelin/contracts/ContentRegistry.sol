// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

/**
 * @dev Content/media registry with ratings.
 * An admin publishes content by hash and users can rate it.
 * Average rating is computed as totalRating / raterCount.
 */
abstract contract ContentRegistry {
    address private _admin;
    uint256 private _contentCount;

    mapping(uint256 => uint256) internal _contentHash;
    mapping(uint256 => uint256) internal _totalRating;
    mapping(uint256 => uint256) internal _raterCount;
    mapping(uint256 => bool) internal _contentPublished;

    constructor() {
        _admin = msg.sender;
        _contentCount = 0;
    }

    function getAdmin() external view returns (address) {
        return _admin;
    }

    function getContentCount() external view returns (uint256) {
        return _contentCount;
    }

    function getContentHash(uint256 contentId) external view returns (uint256) {
        return _contentHash[contentId];
    }

    function getTotalRating(uint256 contentId) external view returns (uint256) {
        return _totalRating[contentId];
    }

    function getRaterCount(uint256 contentId) external view returns (uint256) {
        return _raterCount[contentId];
    }

    function isContentPublished(uint256 contentId) external view returns (bool) {
        return _contentPublished[contentId];
    }

    function getAverageRating(uint256 contentId) external view returns (uint256) {
        require(_raterCount[contentId] > 0, "No ratings yet");
        return _totalRating[contentId] / _raterCount[contentId];
    }

    function publishContent(uint256 contentHash) external returns (uint256) {
        require(msg.sender == _admin, "Only admin");
        uint256 id = _contentCount;
        _contentHash[id] = contentHash;
        _totalRating[id] = 0;
        _raterCount[id] = 0;
        _contentPublished[id] = true;
        _contentCount = id + 1;
        return id;
    }

    function rateContent(uint256 contentId, uint256 rating) external {
        require(_contentPublished[contentId], "Content not published");
        require(rating > 0, "Rating must be positive");
        _totalRating[contentId] = _totalRating[contentId] + rating;
        _raterCount[contentId] = _raterCount[contentId] + 1;
    }
}

contract ContentRegistryTest is ContentRegistry {
    constructor() ContentRegistry() {}

    function initContent(uint256 contentId) external {
        _contentHash[contentId] = 0;
        _totalRating[contentId] = 0;
        _raterCount[contentId] = 0;
        _contentPublished[contentId] = false;
    }
}
