// SPDX-License-Identifier: MIT
pragma solidity >=0.7.0;

/**
 * M41: Caller for struct/tuple return from inner calls (Gap 6 verification).
 */

interface IStructCallee {
    function getPoint(uint256 px, uint256 py) external pure returns (uint256, uint256);
    function getSum(uint256 a, uint256 b) external pure returns (uint256);
}

contract StructCaller {
    address public target;

    constructor(address _target) {
        target = _target;
    }

    /// Call getPoint and return the tuple
    function callGetPoint(uint256 px, uint256 py) external returns (uint256 rx, uint256 ry) {
        (rx, ry) = IStructCallee(target).getPoint(px, py);
    }

    /// Call getSum
    function callGetSum(uint256 a, uint256 b) external returns (uint256) {
        return IStructCallee(target).getSum(a, b);
    }
}
