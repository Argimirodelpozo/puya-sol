==== Source: AVM.sol ====
// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

library Txn {
    function sender() internal view returns (address) { revert(); }
    function fee() internal view returns (uint64) { revert(); }
    function firstValid() internal view returns (uint64) { revert(); }
    function lastValid() internal view returns (uint64) { revert(); }
    function groupIndex() internal view returns (uint64) { revert(); }
    function typeEnum() internal view returns (uint64) { revert(); }
    function applicationId() internal view returns (uint64) { revert(); }
    function onCompletion() internal view returns (uint64) { revert(); }
    function numAppArgs() internal view returns (uint64) { revert(); }
}

library Global {
    function currentApplicationId() internal view returns (uint64) { revert(); }
    function currentApplicationAddress() internal view returns (address) { revert(); }
    function latestTimestamp() internal view returns (uint64) { revert(); }
    function round() internal view returns (uint64) { revert(); }
    function opcodeBudget() internal view returns (uint64) { revert(); }
    function callerApplicationId() internal view returns (uint64) { revert(); }
    function minBalance(address account) internal view returns (uint64) { revert(); }
    function balance(address account) internal view returns (uint64) { revert(); }
}

library Group {
    function size() internal view returns (uint64) { revert(); }
    function index() internal view returns (uint64) { revert(); }
}

==== Source: contract.sol ====
import {Txn, Global, Group} from "AVM.sol";

contract C {
    function txnSender() public view returns (address) { return Txn.sender(); }
    function txnFee() public view returns (uint64) { return Txn.fee(); }
    function txnFirstValid() public view returns (uint64) { return Txn.firstValid(); }
    function txnApplicationId() public view returns (uint64) { return Txn.applicationId(); }
    function txnTypeEnum() public view returns (uint64) { return Txn.typeEnum(); }
    function txnNumAppArgs() public view returns (uint64) { return Txn.numAppArgs(); }

    function globAppId() public view returns (uint64) { return Global.currentApplicationId(); }
    function globAppAddr() public view returns (address) { return Global.currentApplicationAddress(); }
    function globTimestamp() public view returns (uint64) { return Global.latestTimestamp(); }
    function globRound() public view returns (uint64) { return Global.round(); }
    function globBudget() public view returns (uint64) { return Global.opcodeBudget(); }
    function globCallerAppId() public view returns (uint64) { return Global.callerApplicationId(); }
    function globMinBalance(address a) public view returns (uint64) { return Global.minBalance(a); }
    function globBalance(address a) public view returns (uint64) { return Global.balance(a); }

    function groupSize() public view returns (uint64) { return Group.size(); }
    function groupIndex() public view returns (uint64) { return Group.index(); }
}
