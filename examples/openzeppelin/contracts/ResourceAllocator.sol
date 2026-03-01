// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

/**
 * @dev Resource pool allocation and release tracking.
 * Admin allocates resources by amount and can release them.
 * Tracks total allocated amount and active allocation count.
 */
abstract contract ResourceAllocator {
    address private _admin;
    uint256 private _allocationCount;
    uint256 private _totalAllocated;

    mapping(uint256 => uint256) internal _allocationAmount;
    mapping(uint256 => bool) internal _allocationActive;

    constructor() {
        _admin = msg.sender;
        _allocationCount = 0;
        _totalAllocated = 0;
    }

    function getAdmin() external view returns (address) {
        return _admin;
    }

    function getAllocationCount() external view returns (uint256) {
        return _allocationCount;
    }

    function getTotalAllocated() external view returns (uint256) {
        return _totalAllocated;
    }

    function getAllocation(uint256 allocId) external view returns (uint256) {
        return _allocationAmount[allocId];
    }

    function isActive(uint256 allocId) external view returns (bool) {
        return _allocationActive[allocId];
    }

    function allocate(uint256 amount) external returns (uint256) {
        require(msg.sender == _admin, "Only admin");
        require(amount > 0, "Amount must be positive");
        uint256 id = _allocationCount;
        _allocationAmount[id] = amount;
        _allocationActive[id] = true;
        _allocationCount = id + 1;
        _totalAllocated = _totalAllocated + amount;
        return id;
    }

    function release(uint256 allocId) external {
        require(msg.sender == _admin, "Only admin");
        require(allocId < _allocationCount, "Allocation does not exist");
        require(_allocationActive[allocId], "Allocation already released");
        _allocationActive[allocId] = false;
        _totalAllocated = _totalAllocated - _allocationAmount[allocId];
    }
}

contract ResourceAllocatorTest is ResourceAllocator {
    constructor() ResourceAllocator() {}

    function initAllocation(uint256 allocId) external {
        _allocationAmount[allocId] = 0;
        _allocationActive[allocId] = false;
    }
}
