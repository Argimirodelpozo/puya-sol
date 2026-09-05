// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

contract TypedConstants {
    bytes1 constant SMALL = 0x12;
    bytes2 constant WIDE = SMALL;
    bytes4 constant WIDER = WIDE;
    bytes4 constant TEXT = "0x12";
    bytes8 constant TEXT_CHAIN = TEXT;
    bytes8 constant HEX = hex"0030781200";
    bytes4 constant NUMBER = 0x12_34_56_78;
    uint256 constant N = 2;
    uint256 constant N_CHAIN = N;
    int256 constant NEG = -7;
    bool constant YES = true;
    bool constant YES_CHAIN = YES;
    address constant ACCOUNT = 0x1212121212121212121212121212121212121212;
    address constant ACCOUNT_CHAIN = ACCOUNT;

    function high() external pure returns (bytes32, bytes32, bytes32, bytes32) {
        return (WIDER, TEXT_CHAIN, HEX, NUMBER);
    }

    function words() external pure returns (bytes32 a, bytes32 b, bytes32 c, bytes32 d) {
        assembly { a := WIDER b := TEXT_CHAIN c := HEX d := NUMBER }
    }

    function scalars() external pure returns (uint256 a, int256 b, bool c, uint256 d) {
        assembly { a := N_CHAIN b := NEG c := YES_CHAIN d := ACCOUNT_CHAIN }
    }
}
