// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

import {AVM} from "libs/AVM.sol";

contract C {
    uint64 public createdAsa;

    function createIt() public returns (uint64) {
        uint64 id = AVM.asaCreate(1000, 0, "Test Token", "TEST");
        createdAsa = id;
        return id;
    }

    function optInSelf(uint64 assetId) public {
        AVM.asaOptIn(assetId);
    }

    function totalSupplyOf(uint64 assetId) public view returns (uint256) {
        return AVM.asaTotalSupply(assetId);
    }

    function balanceOfSelf(uint64 assetId) public view returns (uint256) {
        return AVM.asaBalance(address(this), assetId);
    }

    function balanceOf(address holder, uint64 assetId) public view returns (uint256) {
        return AVM.asaBalance(holder, assetId);
    }

    function sendTo(uint64 assetId, address to, uint256 amount) public {
        AVM.asaTransfer(assetId, address(this), to, amount);
    }

    function destroyIt(uint64 assetId) public {
        AVM.asaDestroy(assetId);
    }
}
