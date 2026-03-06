// SPDX-License-Identifier: UNLICENSED
pragma solidity ^0.8.20;

import {ReserveFlags, ReserveFlagsMap} from './ReserveFlagsMap.sol';

contract ReserveFlagsMapWrapper {
    using ReserveFlagsMap for ReserveFlags;

    function create(
        bool initPaused,
        bool initFrozen,
        bool initBorrowable,
        bool initReceiveSharesEnabled
    ) external pure returns (uint8) {
        return ReserveFlags.unwrap(
            ReserveFlagsMap.create({
                initPaused: initPaused,
                initFrozen: initFrozen,
                initBorrowable: initBorrowable,
                initReceiveSharesEnabled: initReceiveSharesEnabled
            })
        );
    }

    function setPaused(uint8 flags, bool status) external pure returns (uint8) {
        return ReserveFlags.unwrap(ReserveFlagsMap.setPaused(ReserveFlags.wrap(flags), status));
    }

    function setFrozen(uint8 flags, bool status) external pure returns (uint8) {
        return ReserveFlags.unwrap(ReserveFlagsMap.setFrozen(ReserveFlags.wrap(flags), status));
    }

    function setBorrowable(uint8 flags, bool status) external pure returns (uint8) {
        return ReserveFlags.unwrap(ReserveFlagsMap.setBorrowable(ReserveFlags.wrap(flags), status));
    }

    function setReceiveSharesEnabled(uint8 flags, bool status) external pure returns (uint8) {
        return ReserveFlags.unwrap(ReserveFlagsMap.setReceiveSharesEnabled(ReserveFlags.wrap(flags), status));
    }

    function paused(uint8 flags) external pure returns (bool) {
        return ReserveFlagsMap.paused(ReserveFlags.wrap(flags));
    }

    function frozen(uint8 flags) external pure returns (bool) {
        return ReserveFlagsMap.frozen(ReserveFlags.wrap(flags));
    }

    function borrowable(uint8 flags) external pure returns (bool) {
        return ReserveFlagsMap.borrowable(ReserveFlags.wrap(flags));
    }

    function receiveSharesEnabled(uint8 flags) external pure returns (bool) {
        return ReserveFlagsMap.receiveSharesEnabled(ReserveFlags.wrap(flags));
    }

    function PAUSED_MASK() external pure returns (uint8) {
        return ReserveFlagsMap.PAUSED_MASK;
    }

    function FROZEN_MASK() external pure returns (uint8) {
        return ReserveFlagsMap.FROZEN_MASK;
    }

    function BORROWABLE_MASK() external pure returns (uint8) {
        return ReserveFlagsMap.BORROWABLE_MASK;
    }

    function RECEIVE_SHARES_ENABLED_MASK() external pure returns (uint8) {
        return ReserveFlagsMap.RECEIVE_SHARES_ENABLED_MASK;
    }
}
