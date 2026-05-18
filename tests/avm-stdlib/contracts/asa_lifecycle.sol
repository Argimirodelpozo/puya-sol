==== Source: AVM.sol ====
// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

library AVM {
    function asaCreate(
        uint64 total,
        uint8 decimals,
        string memory name,
        string memory symbol
    ) internal returns (uint64) {
        total; decimals; name; symbol;
        revert();
    }

    function asaOptIn(uint64 assetId) internal {
        assetId;
        revert();
    }

    function asaTransfer(
        uint64 assetId,
        address from,
        address to,
        uint256 amount
    ) internal {
        assetId; from; to; amount;
        revert();
    }

    function asaBalance(address holder, uint64 assetId)
        internal view returns (uint256)
    {
        holder; assetId;
        revert();
    }

    function asaTotalSupply(uint64 assetId) internal view returns (uint256) {
        assetId;
        revert();
    }

    function asaDestroy(uint64 assetId) internal {
        assetId;
        revert();
    }
}

==== Source: contract.sol ====
import {AVM} from "AVM.sol";

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
