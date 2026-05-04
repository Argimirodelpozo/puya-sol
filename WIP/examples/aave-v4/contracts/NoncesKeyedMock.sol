// SPDX-License-Identifier: UNLICENSED
pragma solidity ^0.8.20;

import {NoncesKeyed} from './NoncesKeyed.sol';

/// @notice Test-only contract: exposes `_useCheckedNonce` externally.
/// Mirrors the upstream NoncesKeyedMock used in
/// tests/contracts/utils/NoncesKeyed.t.sol.
contract NoncesKeyedMock is NoncesKeyed {
    function useCheckedNonce(address owner, uint256 keyNonce) external {
        _useCheckedNonce(owner, keyNonce);
    }
}
