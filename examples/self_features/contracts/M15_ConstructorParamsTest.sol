// SPDX-License-Identifier: MIT
pragma solidity >=0.7.0;

/**
 * M15: Constructor parameters test.
 * Exercises reading constructor args from ApplicationArgs during create.
 * Tests: address param, uint256 param, storing in state, reading back.
 */
contract ConstructorParamsTest {
    address public owner;
    uint256 public threshold;

    constructor(address _owner, uint256 _threshold) {
        owner = _owner;
        threshold = _threshold;
    }

    function getOwner() external view returns (address) {
        return owner;
    }

    function getThreshold() external view returns (uint256) {
        return threshold;
    }

    function isOwner(address _addr) external view returns (bool) {
        return _addr == owner;
    }

    function isAboveThreshold(uint256 _value) external view returns (bool) {
        return _value > threshold;
    }
}
