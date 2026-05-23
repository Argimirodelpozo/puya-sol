// SPDX-License-Identifier: BUSL-1.1
pragma solidity ^0.8.15;

import { CTHelpers } from "./CTHelpers.sol";

/// @title Helpers
/// @author Polymarket
/// @notice Helper functions for the CTF
library CTFHelpers {
    /// @notice Returns the positionIds corresponding to _conditionId
    /// @param _collateral  - the collateral address
    /// @param _conditionId - the conditionId
    /// @return positionIds - length 2 array of position ids
    function positionIds(address _collateral, bytes32 _conditionId) internal view returns (uint256[] memory) {
        uint256[] memory positionIds_ = new uint256[](2);

        // YES
        positionIds_[0] = CTHelpers.getPositionId(_collateral, CTHelpers.getCollectionId(bytes32(0), _conditionId, 1));
        // NO
        positionIds_[1] = CTHelpers.getPositionId(_collateral, CTHelpers.getCollectionId(bytes32(0), _conditionId, 2));

        return positionIds_;
    }

    /// @notice returns the partition for a binary conditional token
    /// @return partition_ - the partition [1,2] = [0b01, 0b10]
    function partition() internal pure returns (uint256[] memory partition_) {
        // AVM-PORT-ADAPTATION: the original assembly version returns a
        // memory pointer that puya-sol can't reify into ARC4-encoded
        // bytes when the array is then passed to an inner-call's
        // ApplicationArgs. Allocating via Solidity's standard new+index
        // path lets puya-sol track it as a proper arc4 dynamic array.
        partition_ = new uint256[](2);
        partition_[0] = 1;
        partition_[1] = 2;
    }
}
