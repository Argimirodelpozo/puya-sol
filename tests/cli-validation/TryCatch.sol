// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

interface IValue {
    function value() external returns (uint256);
}

contract TryCatch {
    function read(IValue target) external returns (uint256) {
        try target.value() returns (uint256 result) {
            return result;
        } catch {
            return 0;
        }
    }
}
