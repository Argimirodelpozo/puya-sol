// SPDX-License-Identifier: MIT
pragma solidity ^0.8.4;

import "../contracts/utils/LibTransient.sol";

contract LibTransientWrapper {
    using LibTransient for LibTransient.TUint256;

    LibTransient.TUint256 private _tval;

    function setTransient(uint256 value) external {
        _tval.set(value);
    }

    function getTransient() external view returns (uint256) {
        return _tval.get();
    }
}
