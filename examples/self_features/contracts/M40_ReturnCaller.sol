// SPDX-License-Identifier: MIT
pragma solidity >=0.7.0;

/**
 * M40: Caller contract for cross-contract return value test (Gap 1).
 * Uses .call(abi.encodeCall(...)) and decodes the return data.
 */

interface IReturnCallee {
    function getNumber() external view returns (uint256);
    function add(uint256 a, uint256 b) external pure returns (uint256);
    function isEven(uint256 n) external pure returns (bool);
}

contract ReturnCaller {
    address public target;

    constructor(address _target) {
        target = _target;
    }

    /// Calls getNumber() via .call and decodes the uint256 return
    function callGetNumber() external returns (uint256) {
        (bool success, bytes memory data) = target.call(
            abi.encodeCall(IReturnCallee.getNumber, ())
        );
        require(success, "call failed");
        return abi.decode(data, (uint256));
    }

    /// Calls add(a, b) via .call and decodes the uint256 return
    function callAdd(uint256 a, uint256 b) external returns (uint256) {
        (bool success, bytes memory data) = target.call(
            abi.encodeCall(IReturnCallee.add, (a, b))
        );
        require(success, "call failed");
        return abi.decode(data, (uint256));
    }

    /// Calls isEven(n) via .call and decodes the bool return
    function callIsEven(uint256 n) external returns (bool) {
        (bool success, bytes memory data) = target.call(
            abi.encodeCall(IReturnCallee.isEven, (n))
        );
        require(success, "call failed");
        return abi.decode(data, (bool));
    }
}
