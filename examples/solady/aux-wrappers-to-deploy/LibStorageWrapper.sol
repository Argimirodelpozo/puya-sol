// SPDX-License-Identifier: MIT
pragma solidity ^0.8.4;

import "../contracts/utils/LibStorage.sol";

contract LibStorageWrapper {
    using LibStorage for LibStorage.Bump;

    LibStorage.Bump private _bump;

    function getSlot() external view returns (bytes32) {
        return _bump.slot();
    }

    function doInvalidate() external {
        _bump.invalidate();
    }
}
