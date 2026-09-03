// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

import "libs/AVM.sol";

// Coverage lane for EVERY AVM.asa* intrinsic (itxn/AsaIntrinsics.cpp): the
// lifecycle create → reads → transfer/clawback → freeze → optIn → destroy.
// The creating app is all four ASA roles and holds the supply.
contract AsaKit {
    uint64 public asset;
    uint64 public coldAsset;

    function bitLength(uint256 value) external pure returns (uint256) {
        return Bits.bitlen(value);
    }

    function create() external returns (uint64) {
        asset = AVM.asaCreate(100000, 2, "Kit Token", "KIT");
        return asset;
    }

    // 5-arg form: default_frozen — holders are frozen on opt-in; only the
    // contract's clawback moves units.
    function createFrozen() external returns (uint64) {
        coldAsset = AVM.asaCreate(500, 0, "Cold", "COLD", true);
        return coldAsset;
    }

    function meta()
        external view
        returns (string memory, string memory, uint8, uint256)
    {
        return (
            AVM.asaName(asset),
            AVM.asaUnitName(asset),
            AVM.asaDecimals(asset),
            AVM.asaTotalSupply(asset)
        );
    }

    function bal(address holder) external view returns (uint256) {
        return AVM.asaBalance(holder, asset);
    }

    function coldBal(address holder) external view returns (uint256) {
        return AVM.asaBalance(holder, coldAsset);
    }

    function send(address to, uint256 amount) external {
        AVM.asaTransfer(asset, address(this), to, amount);
    }

    function sendCold(address to, uint256 amount) external {
        AVM.asaTransfer(coldAsset, address(this), to, amount);
    }

    function claw(address from, address to, uint256 amount) external {
        AVM.asaTransfer(asset, from, to, amount);
    }

    function setFreeze(address holder, bool frozen) external {
        AVM.asaFreeze(asset, holder, frozen);
    }

    function optInto(uint64 assetId) external {
        AVM.asaOptIn(assetId);
    }

    function foreignBal(uint64 assetId) external view returns (uint256) {
        return AVM.asaBalance(address(this), assetId);
    }

    function destroy() external {
        AVM.asaDestroy(asset);
    }
}
