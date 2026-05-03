// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

/// @notice Algorand Virtual Machine intrinsics.
///
/// These functions are NOT executable on EVM. They are recognized by the
/// puya-sol compiler and replaced at call-resolution time with the
/// corresponding AVM operations (asset_holding_get, asset_params_get,
/// acfg / axfer inner transactions).
///
/// Bodies revert as a safety net so accidental EVM use fails fast.
library AVM {
    /// Create a new ASA owned and clawback-controlled by the contract.
    /// Returns the new ASA's id.
    function asaCreate(
        uint64 total,
        uint8 decimals,
        string memory name,
        string memory symbol
    ) internal returns (uint64) {
        total; decimals; name; symbol;
        revert("AVM.asaCreate: requires puya-sol");
    }

    /// Read `holder`'s balance of `assetId`. Returns 0 if not opted in.
    function asaBalance(address holder, uint64 assetId)
        internal view returns (uint256)
    {
        holder; assetId;
        revert("AVM.asaBalance: requires puya-sol");
    }

    /// Read total supply of `assetId`.
    function asaTotalSupply(uint64 assetId) internal view returns (uint256) {
        assetId;
        revert("AVM.asaTotalSupply: requires puya-sol");
    }

    /// Clawback `amount` of `assetId` from `from` to `to`. Reverts if
    /// either party has not opted in.
    function asaTransfer(
        uint64 assetId,
        address from,
        address to,
        uint256 amount
    ) internal {
        assetId; from; to; amount;
        revert("AVM.asaTransfer: requires puya-sol");
    }
}
