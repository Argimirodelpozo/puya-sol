// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

contract SuperLookupBase {
    uint256 public trace;
    function digit() internal pure virtual returns (uint256) { return 1; }
    modifier mark() virtual { trace = 1; _; }
    function inherited() public mark returns (uint256) { return trace; }
}

contract SuperLookupLeft is SuperLookupBase {
    function digit() internal pure virtual override returns (uint256) { return 3; }
    modifier mark() virtual override { trace = super.digit(); _; }
    function explicitLeft() public SuperLookupLeft.mark returns (uint256) { return trace; }
}

contract SuperLookupRight is SuperLookupBase {
    function digit() internal pure virtual override returns (uint256) { return 5; }
    modifier mark() virtual override { trace = super.digit(); _; }
    function explicitRight() public SuperLookupRight.mark returns (uint256) { return trace; }
}

contract ModifierSuperLookup is SuperLookupLeft, SuperLookupRight {
    function digit() internal pure override(SuperLookupLeft, SuperLookupRight)
        returns (uint256) { return 7; }
    modifier mark() override(SuperLookupLeft, SuperLookupRight) { trace = super.digit(); _; }
    function explicitBase() public SuperLookupBase.mark returns (uint256) { return trace; }
}
