// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

contract LookupBase {
    uint256 public trace;
    function reset() external { trace = 0; }

    modifier mark() virtual { trace = trace * 10 + 1; _; trace = trace * 10 + 2; }
    function inherited() public mark returns (uint256) { return trace; }
    function explicitBase() public LookupBase.mark returns (uint256) { return trace; }
}

contract LookupLeft is LookupBase {
    modifier mark() virtual override { trace = trace * 10 + 3; _; trace = trace * 10 + 4; }
    function explicitLeft() public LookupLeft.mark returns (uint256) { return trace; }
}

contract LookupRight is LookupBase {
    modifier mark() virtual override { trace = trace * 10 + 5; _; trace = trace * 10 + 6; }
}

contract ModifierLookup is LookupLeft, LookupRight {
    modifier mark() override(LookupLeft, LookupRight) {
        trace = trace * 10 + 7;
        _;
        trace = trace * 10 + 8;
    }

    function stacked() external LookupBase.mark mark returns (uint256) { return trace; }
}
