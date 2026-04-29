// SPDX-License-Identifier: BUSL-1.1
pragma solidity ^0.8.0;

import { Side } from "../libraries/Structs.sol";

/// @title Events
abstract contract Events {
    /*//////////////////////////////////////////////////////////////
                            EVENTS
    //////////////////////////////////////////////////////////////*/

    /// @notice Emitted whenever a maker order's fill is recorded.
    /// AVM-PORT-ADAPTATION: declared without `indexed` topics — AVM's
    /// `op.log` is data-only (no topic concept). puya-sol lowers
    /// Solidity-level `emit` to a single `op.log` payload of
    /// `selector(4) ++ arc4_encode(args)` per ARC-28. The original
    /// audited Solidity used inline-asm `log4` so it could split fields
    /// across topics and data; on AVM the per-app-call log byte budget
    /// is the same in both forms (everything's data) and `emit` is the
    /// shape puya-sol can lower.
    event OrderFilled(
        bytes32 orderHash,
        address maker,
        address taker,
        uint8 side,
        uint256 tokenId,
        uint256 makerAmountFilled,
        uint256 takerAmountFilled,
        uint256 fee,
        bytes32 builder,
        bytes32 metadata
    );

    /// @notice Emitted once per `matchOrders` call, summarizing the
    /// taker side of the trade.
    event OrdersMatched(
        bytes32 takerOrderHash,
        address takerOrderMaker,
        uint8 side,
        uint256 tokenId,
        uint256 makerAmountFilled,
        uint256 takerAmountFilled
    );

    /// @notice Emitted whenever an order's fee is paid to the fee receiver.
    event FeeCharged(address receiver, uint256 amount);

    /*//////////////////////////////////////////////////////////////
                            STRUCTS
    //////////////////////////////////////////////////////////////*/

    /// @notice Parameters for the OrderFilled event
    struct OrderFilledParams {
        bytes32 orderHash; // 0x00
        address maker; // 0x20
        address taker; // 0x40
        Side side; // 0x60
        uint256 tokenId; // 0x80
        uint256 makerAmountFilled; // 0xa0
        uint256 takerAmountFilled; // 0xc0
        uint256 fee; // 0xe0
        bytes32 builder; // 0x100
        bytes32 metadata; // 0x120
    }

    /*//////////////////////////////////////////////////////////////
                        EMIT FUNCTIONS
    //////////////////////////////////////////////////////////////*/

    /// @dev Emits the OrderFilled event.
    function _emitOrderFilledEvent(OrderFilledParams memory p) internal {
        emit OrderFilled(
            p.orderHash,
            p.maker,
            p.taker,
            uint8(p.side),
            p.tokenId,
            p.makerAmountFilled,
            p.takerAmountFilled,
            p.fee,
            p.builder,
            p.metadata
        );
    }

    /// @dev Emits the OrderFilled event for the taker side, then the
    /// summary OrdersMatched event.
    function _emitTakerFilledEvents(OrderFilledParams memory p) internal {
        _emitOrderFilledEvent(p);
        emit OrdersMatched(
            p.orderHash,
            p.maker,
            uint8(p.side),
            p.tokenId,
            p.makerAmountFilled,
            p.takerAmountFilled
        );
    }

    /// @dev Emits the FeeCharged event.
    function _emitFeeCharged(address receiver, uint256 amount) internal {
        emit FeeCharged(receiver, amount);
    }
}
