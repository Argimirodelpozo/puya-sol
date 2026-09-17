// SPDX-License-Identifier: MIT
pragma solidity ^0.8.28;
import {AVM} from "libs/AVM.sol";

contract AsaOperandOrder {
    using AVM for uint64;
    uint64 private trace;
    function total() internal returns (uint64) { trace = trace * 10 + 1; return 100; }
    function decimals() internal returns (uint8) { trace = trace * 10 + 2; return 0; }
    function name() internal returns (string memory) { trace = trace * 10 + 3; return "Order"; }
    function symbol() internal returns (string memory) { trace = trace * 10 + 4; return "ORD"; }
    function run(bool named) external returns (uint64) {
        trace = 0;
        uint64 asset;
        if (named) asset = total().asaCreate({symbol: symbol(), name: name(), decimals: decimals()});
        else asset = total().asaCreate(decimals(), name(), symbol());
        AVM.asaDestroy(asset);
        return trace;
    }
}
