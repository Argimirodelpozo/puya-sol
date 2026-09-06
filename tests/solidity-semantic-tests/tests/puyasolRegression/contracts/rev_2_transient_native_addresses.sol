// SPDX-License-Identifier: MIT
pragma solidity >=0.8.28;
import {Txn} from "libs/AVM.sol";

type NativeTransientAddress is address;

contract NativeTransientAddresses {
    address transient direct;
    uint96 transient neighbor;
    NativeTransientAddress transient wrapped;
    uint96 transient wrappedNeighbor;

    function raw() internal {
        assembly { tstore(direct.slot, tload(direct.slot)) }
    }

    function check() external returns (bool, bool, bool, bool) {
        // Explicit AVM source bypasses the EVM profile's msg.sender projection.
        address sender = Txn.sender();
        direct = sender;
        wrapped = NativeTransientAddress.wrap(sender);
        neighbor = type(uint96).max;
        wrappedNeighbor = 123;
        bool before = direct == sender && NativeTransientAddress.unwrap(wrapped) == sender;
        raw();
        return (before, direct != sender, NativeTransientAddress.unwrap(wrapped) == sender,
            neighbor == type(uint96).max && wrappedNeighbor == 123);
    }
}
