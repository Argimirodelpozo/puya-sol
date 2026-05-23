// SPDX-License-Identifier: MIT
pragma solidity ^0.8.15;

import { BaseExchangeTest } from "./BaseExchangeTest.sol";
import { Order, Side, SignatureType } from "../libraries/OrderStructs.sol";
import { IERC20 } from "openzeppelin-contracts/token/ERC20/IERC20.sol";
import { IERC1155 } from "openzeppelin-contracts/token/ERC1155/IERC1155.sol";

/// @notice Gas snapshot tests for matchOrders
/// @dev Run with: forge test --match-contract GasSnapshots_Test --gas-report
/// @dev Snapshots are written to snapshots/GasSnapshots_Test.json
contract GasSnapshots_Test is BaseExchangeTest {

    /*//////////////////////////////////////////////////////////////
                        COMPLEMENTARY (BUY vs SELL)
    //////////////////////////////////////////////////////////////*/

    function test_complementary_1maker() public {
        (Order memory takerOrder, Order[] memory makerOrders, uint256 takerFillAmount, uint256[] memory fillAmounts) = _prepareComplementary(1);

        vm.prank(bob);
        vm.startSnapshotGas("complementary_1maker");
        exchange.matchOrders(takerOrder, makerOrders, takerFillAmount, fillAmounts);
        vm.stopSnapshotGas();
    }

    function test_complementary_5makers() public {
        (Order memory takerOrder, Order[] memory makerOrders, uint256 takerFillAmount, uint256[] memory fillAmounts) = _prepareComplementary(5);

        vm.prank(bob);
        vm.startSnapshotGas("complementary_5makers");
        exchange.matchOrders(takerOrder, makerOrders, takerFillAmount, fillAmounts);
        vm.stopSnapshotGas();
    }

    function test_complementary_10makers() public {
        (Order memory takerOrder, Order[] memory makerOrders, uint256 takerFillAmount, uint256[] memory fillAmounts) = _prepareComplementary(10);

        vm.prank(bob);
        vm.startSnapshotGas("complementary_10makers");
        exchange.matchOrders(takerOrder, makerOrders, takerFillAmount, fillAmounts);
        vm.stopSnapshotGas();
    }

    function test_complementary_20makers() public {
        (Order memory takerOrder, Order[] memory makerOrders, uint256 takerFillAmount, uint256[] memory fillAmounts) = _prepareComplementary(20);

        vm.prank(bob);
        vm.startSnapshotGas("complementary_20makers");
        exchange.matchOrders(takerOrder, makerOrders, takerFillAmount, fillAmounts);
        vm.stopSnapshotGas();
    }

    /*//////////////////////////////////////////////////////////////
                            MINT (BUY vs BUY)
    //////////////////////////////////////////////////////////////*/

    function test_mint_1maker() public {
        (Order memory takerOrder, Order[] memory makerOrders, uint256 takerFillAmount, uint256[] memory fillAmounts) = _prepareMint(1);

        vm.prank(bob);
        vm.startSnapshotGas("mint_1maker");
        exchange.matchOrders(takerOrder, makerOrders, takerFillAmount, fillAmounts);
        vm.stopSnapshotGas();
    }

    function test_mint_5makers() public {
        (Order memory takerOrder, Order[] memory makerOrders, uint256 takerFillAmount, uint256[] memory fillAmounts) = _prepareMint(5);

        vm.prank(bob);
        vm.startSnapshotGas("mint_5makers");
        exchange.matchOrders(takerOrder, makerOrders, takerFillAmount, fillAmounts);
        vm.stopSnapshotGas();
    }

    function test_mint_10makers() public {
        (Order memory takerOrder, Order[] memory makerOrders, uint256 takerFillAmount, uint256[] memory fillAmounts) = _prepareMint(10);

        vm.prank(bob);
        vm.startSnapshotGas("mint_10makers");
        exchange.matchOrders(takerOrder, makerOrders, takerFillAmount, fillAmounts);
        vm.stopSnapshotGas();
    }

    function test_mint_20makers() public {
        (Order memory takerOrder, Order[] memory makerOrders, uint256 takerFillAmount, uint256[] memory fillAmounts) = _prepareMint(20);

        vm.prank(bob);
        vm.startSnapshotGas("mint_20makers");
        exchange.matchOrders(takerOrder, makerOrders, takerFillAmount, fillAmounts);
        vm.stopSnapshotGas();
    }

    /*//////////////////////////////////////////////////////////////
                          MERGE (SELL vs SELL)
    //////////////////////////////////////////////////////////////*/

    function test_merge_1maker() public {
        (Order memory takerOrder, Order[] memory makerOrders, uint256 takerFillAmount, uint256[] memory fillAmounts) = _prepareMerge(1);

        vm.prank(bob);
        vm.startSnapshotGas("merge_1maker");
        exchange.matchOrders(takerOrder, makerOrders, takerFillAmount, fillAmounts);
        vm.stopSnapshotGas();
    }

    function test_merge_5makers() public {
        (Order memory takerOrder, Order[] memory makerOrders, uint256 takerFillAmount, uint256[] memory fillAmounts) = _prepareMerge(5);

        vm.prank(bob);
        vm.startSnapshotGas("merge_5makers");
        exchange.matchOrders(takerOrder, makerOrders, takerFillAmount, fillAmounts);
        vm.stopSnapshotGas();
    }

    function test_merge_10makers() public {
        (Order memory takerOrder, Order[] memory makerOrders, uint256 takerFillAmount, uint256[] memory fillAmounts) = _prepareMerge(10);

        vm.prank(bob);
        vm.startSnapshotGas("merge_10makers");
        exchange.matchOrders(takerOrder, makerOrders, takerFillAmount, fillAmounts);
        vm.stopSnapshotGas();
    }

    function test_merge_20makers() public {
        (Order memory takerOrder, Order[] memory makerOrders, uint256 takerFillAmount, uint256[] memory fillAmounts) = _prepareMerge(20);

        vm.prank(bob);
        vm.startSnapshotGas("merge_20makers");
        exchange.matchOrders(takerOrder, makerOrders, takerFillAmount, fillAmounts);
        vm.stopSnapshotGas();
    }

    /*//////////////////////////////////////////////////////////////
                    COMBO: COMPLEMENTARY + MINT
                (Taker BUY YES, half SELL YES + half BUY NO)
    //////////////////////////////////////////////////////////////*/

    function test_combo_complementary_mint_10makers() public {
        (Order memory takerOrder, Order[] memory makerOrders, uint256 takerFillAmount, uint256[] memory fillAmounts) = _prepareComboComplementaryMint(10);

        vm.prank(bob);
        vm.startSnapshotGas("combo_complementary_mint_10makers");
        exchange.matchOrders(takerOrder, makerOrders, takerFillAmount, fillAmounts);
        vm.stopSnapshotGas();
    }

    function test_combo_complementary_mint_20makers() public {
        (Order memory takerOrder, Order[] memory makerOrders, uint256 takerFillAmount, uint256[] memory fillAmounts) = _prepareComboComplementaryMint(20);

        vm.prank(bob);
        vm.startSnapshotGas("combo_complementary_mint_20makers");
        exchange.matchOrders(takerOrder, makerOrders, takerFillAmount, fillAmounts);
        vm.stopSnapshotGas();
    }

    /*//////////////////////////////////////////////////////////////
                    COMBO: COMPLEMENTARY + MERGE
                (Taker SELL YES, half BUY YES + half SELL NO)
    //////////////////////////////////////////////////////////////*/

    function test_combo_complementary_merge_10makers() public {
        (Order memory takerOrder, Order[] memory makerOrders, uint256 takerFillAmount, uint256[] memory fillAmounts) = _prepareComboComplementaryMerge(10);

        vm.prank(bob);
        vm.startSnapshotGas("combo_complementary_merge_10makers");
        exchange.matchOrders(takerOrder, makerOrders, takerFillAmount, fillAmounts);
        vm.stopSnapshotGas();
    }

    function test_combo_complementary_merge_20makers() public {
        (Order memory takerOrder, Order[] memory makerOrders, uint256 takerFillAmount, uint256[] memory fillAmounts) = _prepareComboComplementaryMerge(20);

        vm.prank(bob);
        vm.startSnapshotGas("combo_complementary_merge_20makers");
        exchange.matchOrders(takerOrder, makerOrders, takerFillAmount, fillAmounts);
        vm.stopSnapshotGas();
    }

    /*//////////////////////////////////////////////////////////////
                              SETUP HELPERS
    //////////////////////////////////////////////////////////////*/

    function _prepareComplementary(uint256 numMakers) internal returns (
        Order memory takerOrder,
        Order[] memory makerOrders,
        uint256 takerFillAmount,
        uint256[] memory fillAmounts
    ) {
        uint256 usdcPerMaker = 10_000_000;
        uint256 tokensPerMaker = 20_000_000;
        uint256 totalUsdc = usdcPerMaker * numMakers;
        uint256 totalTokens = tokensPerMaker * numMakers;

        _dealUsdcAndApprove(bob, address(exchange), totalUsdc);
        _dealOutcomeTokensAndApprove(carla, address(exchange), yes, totalTokens);

        takerOrder = _createAndSignOrderWithSalt(bobPK, yes, totalUsdc, totalTokens, Side.BUY, 1);
        makerOrders = new Order[](numMakers);
        fillAmounts = new uint256[](numMakers);

        for (uint256 i = 0; i < numMakers; i++) {
            makerOrders[i] = _createAndSignOrderWithSalt(carlaPK, yes, tokensPerMaker, usdcPerMaker, Side.SELL, i + 100);
            fillAmounts[i] = tokensPerMaker;
        }

        takerFillAmount = totalUsdc;
    }

    function _prepareMint(uint256 numMakers) internal returns (
        Order memory takerOrder,
        Order[] memory makerOrders,
        uint256 takerFillAmount,
        uint256[] memory fillAmounts
    ) {
        uint256 usdcPerMaker = 10_000_000;
        uint256 tokensPerMaker = 20_000_000;
        uint256 totalUsdc = usdcPerMaker * numMakers;
        uint256 totalTokens = tokensPerMaker * numMakers;
        uint256 takerUsdc = totalTokens / 2;

        _dealUsdcAndApprove(bob, address(exchange), takerUsdc);
        _dealUsdcAndApprove(carla, address(exchange), totalUsdc);

        takerOrder = _createAndSignOrderWithSalt(bobPK, yes, takerUsdc, totalTokens, Side.BUY, 1);
        makerOrders = new Order[](numMakers);
        fillAmounts = new uint256[](numMakers);

        for (uint256 i = 0; i < numMakers; i++) {
            makerOrders[i] = _createAndSignOrderWithSalt(carlaPK, no, usdcPerMaker, tokensPerMaker, Side.BUY, i + 100);
            fillAmounts[i] = usdcPerMaker;
        }

        takerFillAmount = takerUsdc;
    }

    function _prepareMerge(uint256 numMakers) internal returns (
        Order memory takerOrder,
        Order[] memory makerOrders,
        uint256 takerFillAmount,
        uint256[] memory fillAmounts
    ) {
        uint256 tokensPerMaker = 20_000_000;
        uint256 usdcPerMaker = 10_000_000;
        uint256 totalTokens = tokensPerMaker * numMakers;
        uint256 totalUsdc = usdcPerMaker * numMakers;

        _dealOutcomeTokensAndApprove(bob, address(exchange), yes, totalTokens);
        _dealOutcomeTokensAndApprove(carla, address(exchange), no, totalTokens);

        takerOrder = _createAndSignOrderWithSalt(bobPK, yes, totalTokens, totalUsdc, Side.SELL, 1);
        makerOrders = new Order[](numMakers);
        fillAmounts = new uint256[](numMakers);

        for (uint256 i = 0; i < numMakers; i++) {
            makerOrders[i] = _createAndSignOrderWithSalt(carlaPK, no, tokensPerMaker, usdcPerMaker, Side.SELL, i + 100);
            fillAmounts[i] = tokensPerMaker;
        }

        takerFillAmount = totalTokens;
    }

    /// @notice Combo: Taker BUY YES, half makers SELL YES (complementary), half makers BUY NO (mint)
    function _prepareComboComplementaryMint(uint256 numMakers) internal returns (
        Order memory takerOrder,
        Order[] memory makerOrders,
        uint256 takerFillAmount,
        uint256[] memory fillAmounts
    ) {
        uint256 half = numMakers / 2;
        uint256 usdcPerMaker = 10_000_000;
        uint256 tokensPerMaker = 20_000_000;

        // Taker needs USDC for both complementary and mint portions
        uint256 totalTakerUsdc = usdcPerMaker * half + (tokensPerMaker * half / 2);
        uint256 totalTakerTokens = tokensPerMaker * numMakers;

        _dealUsdcAndApprove(bob, address(exchange), totalTakerUsdc);
        // Carla needs YES tokens for complementary (SELL) and USDC for mint (BUY)
        _dealOutcomeTokensAndApprove(carla, address(exchange), yes, tokensPerMaker * half);
        _dealUsdcAndApprove(carla, address(exchange), usdcPerMaker * half);

        takerOrder = _createAndSignOrderWithSalt(bobPK, yes, totalTakerUsdc, totalTakerTokens, Side.BUY, 1);
        makerOrders = new Order[](numMakers);
        fillAmounts = new uint256[](numMakers);

        // First half: complementary (SELL YES)
        for (uint256 i = 0; i < half; i++) {
            makerOrders[i] = _createAndSignOrderWithSalt(carlaPK, yes, tokensPerMaker, usdcPerMaker, Side.SELL, i + 100);
            fillAmounts[i] = tokensPerMaker;
        }

        // Second half: mint (BUY NO)
        for (uint256 i = half; i < numMakers; i++) {
            makerOrders[i] = _createAndSignOrderWithSalt(carlaPK, no, usdcPerMaker, tokensPerMaker, Side.BUY, i + 100);
            fillAmounts[i] = usdcPerMaker;
        }

        takerFillAmount = totalTakerUsdc;
    }

    /// @notice Combo: Taker SELL YES, half makers BUY YES (complementary), half makers SELL NO (merge)
    function _prepareComboComplementaryMerge(uint256 numMakers) internal returns (
        Order memory takerOrder,
        Order[] memory makerOrders,
        uint256 takerFillAmount,
        uint256[] memory fillAmounts
    ) {
        uint256 half = numMakers / 2;
        uint256 tokensPerMaker = 20_000_000;
        uint256 usdcPerMaker = 10_000_000;

        uint256 totalTakerTokens = tokensPerMaker * numMakers;
        // Taker receives USDC from complementary and merge
        uint256 totalTakerUsdc = usdcPerMaker * half + usdcPerMaker * half;

        _dealOutcomeTokensAndApprove(bob, address(exchange), yes, totalTakerTokens);
        // Carla needs USDC for complementary (BUY) and NO tokens for merge (SELL)
        _dealUsdcAndApprove(carla, address(exchange), usdcPerMaker * half);
        _dealOutcomeTokensAndApprove(carla, address(exchange), no, tokensPerMaker * half);

        takerOrder = _createAndSignOrderWithSalt(bobPK, yes, totalTakerTokens, totalTakerUsdc, Side.SELL, 1);
        makerOrders = new Order[](numMakers);
        fillAmounts = new uint256[](numMakers);

        // First half: complementary (BUY YES)
        for (uint256 i = 0; i < half; i++) {
            makerOrders[i] = _createAndSignOrderWithSalt(carlaPK, yes, usdcPerMaker, tokensPerMaker, Side.BUY, i + 100);
            fillAmounts[i] = usdcPerMaker;
        }

        // Second half: merge (SELL NO)
        for (uint256 i = half; i < numMakers; i++) {
            makerOrders[i] = _createAndSignOrderWithSalt(carlaPK, no, tokensPerMaker, usdcPerMaker, Side.SELL, i + 100);
            fillAmounts[i] = tokensPerMaker;
        }

        takerFillAmount = totalTakerTokens;
    }

    /*//////////////////////////////////////////////////////////////
                              HELPERS
    //////////////////////////////////////////////////////////////*/

    function _dealUsdcAndApprove(address to, address spender, uint256 amount) internal {
        vm.startPrank(to);
        dealAndApprove(address(usdc), to, spender, amount);
        vm.stopPrank();
    }

    function _dealOutcomeTokensAndApprove(address to, address spender, uint256 tokenId, uint256 amount) internal {
        vm.startPrank(admin);
        approve(address(usdc), address(ctf), type(uint256).max);
        deal(address(usdc), admin, amount);

        uint256[] memory partition = new uint256[](2);
        partition[0] = 1;
        partition[1] = 2;

        ctf.splitPosition(IERC20(address(usdc)), bytes32(0), conditionId, partition, amount);
        IERC1155(address(ctf)).safeTransferFrom(admin, to, tokenId, amount, "");
        vm.stopPrank();

        vm.prank(to);
        IERC1155(address(ctf)).setApprovalForAll(spender, true);
    }

    function _createAndSignOrderWithSalt(
        uint256 pk,
        uint256 tokenId,
        uint256 makerAmount,
        uint256 takerAmount,
        Side side,
        uint256 salt
    ) internal returns (Order memory) {
        address maker = vm.addr(pk);
        Order memory order = _createOrderWithSalt(maker, tokenId, makerAmount, takerAmount, side, salt);
        order.signature = _signMessage(pk, exchange.hashOrder(order));
        return order;
    }

    function _createOrderWithSalt(
        address maker,
        uint256 tokenId,
        uint256 makerAmount,
        uint256 takerAmount,
        Side side,
        uint256 salt
    ) internal pure returns (Order memory) {
        Order memory order = Order({
            salt: salt,
            signer: maker,
            maker: maker,
            taker: address(0),
            tokenId: tokenId,
            makerAmount: makerAmount,
            takerAmount: takerAmount,
            expiration: 0,
            nonce: 0,
            feeRateBps: 0,
            signatureType: SignatureType.EOA,
            side: side,
            signature: new bytes(0)
        });
        return order;
    }
}
