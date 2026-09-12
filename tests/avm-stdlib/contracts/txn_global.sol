// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

import {Txn, Global, Group} from "libs/AVM.sol";

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
