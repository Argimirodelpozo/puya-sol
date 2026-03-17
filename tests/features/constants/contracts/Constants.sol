// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

uint256 constant FILE_LEVEL_CONST = 12345;
uint256 constant COMPUTED_CONST = 2 ** 128;
bytes32 constant HASH_CONST = keccak256("hello");

contract Constants {
    // Contract-level constants
    uint256 public constant MAX_SUPPLY = 1000000 * 10**18;
    uint256 public constant PRECISION = 1e18;
    uint8 public constant DECIMALS = 18;
    bool public constant IS_ACTIVE = true;
    address public constant ZERO_ADDRESS = address(0);
    bytes32 public constant EMPTY_HASH = bytes32(0);

    // Immutable (set once in constructor)
    uint256 public immutable deployTime;
    address public immutable deployer;

    // Constant expressions
    uint256 public constant TWO_POW_128 = 2 ** 128;
    uint256 public constant MASK_160 = type(uint160).max;
    uint256 public constant MAX_UINT = type(uint256).max;
    uint64 public constant MAX_UINT64 = type(uint64).max;

    constructor() {
        deployTime = block.timestamp;
        deployer = msg.sender;
    }

    // Use file-level constant
    function getFileLevelConst() external pure returns (uint256) {
        return FILE_LEVEL_CONST;
    }

    // Use computed constant
    function getComputedConst() external pure returns (uint256) {
        return COMPUTED_CONST;
    }

    // Use contract constant in expression
    function withPrecision(uint256 amount) external pure returns (uint256) {
        return amount * PRECISION / 100;
    }

    // Constant used in require
    function checkMax(uint256 supply) external pure returns (bool) {
        require(supply <= MAX_SUPPLY, "exceeds max");
        return true;
    }

    // Immutable reads
    function getDeployTime() external view returns (uint256) {
        return deployTime;
    }

    function getDeployer() external view returns (address) {
        return deployer;
    }

    // type(T).max / type(T).min
    function getMaxUint256() external pure returns (uint256) {
        return type(uint256).max;
    }

    function getMaxUint64() external pure returns (uint64) {
        return type(uint64).max;
    }

    // Constant bytes32 comparison
    function isEmptyHash(bytes32 h) external pure returns (bool) {
        return h == EMPTY_HASH;
    }

    // Constant in arithmetic
    function toWei(uint256 amount) external pure returns (uint256) {
        return amount * 10**DECIMALS;
    }
}
