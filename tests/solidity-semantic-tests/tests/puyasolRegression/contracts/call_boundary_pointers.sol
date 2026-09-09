// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

contract CallBoundaryPointerBase {
    function(uint64, uint64) internal pure returns (uint64) stored = decimal;
    function decimal(uint64 a, uint64 b) internal pure returns (uint64) { return a * 10 + b; }
    function sum(uint64 a, uint64 b) internal pure returns (uint64) { return a + b; }
}

contract CallBoundaryPointers is CallBoundaryPointerBase {
    struct Box { function(uint64, uint64) internal pure returns (uint64) pointer; }
    struct Value { uint64 n; }
    uint64 private trace;

    function reassigned(bool second) external pure returns (uint64) {
        function(uint64, uint64) internal pure returns (uint64) p = decimal;
        if (second) p = sum;
        return p(3, 2);
    }
    function swapped() external pure returns (uint64) {
        function(uint64, uint64) internal pure returns (uint64) p = decimal;
        function(uint64, uint64) internal pure returns (uint64) q = sum;
        (p, q) = (q, p);
        return p(3, 2) * 100 + q(3, 2);
    }
    function loop() external pure returns (uint64 result) {
        function(uint64, uint64) internal pure returns (uint64) p = decimal;
        for (uint64 i = 0; i < 3; ++i) {
            result = result * 100 + p(3, 2);
            p = sum;
        }
    }
    function cleared(bool clear) external pure returns (uint64) {
        function(uint64, uint64) internal pure returns (uint64) p = decimal;
        if (clear) delete p;
        return p(3, 2);
    }
    function assemblyReassigned() external pure returns (uint64) {
        function(uint64, uint64) internal pure returns (uint64) p = decimal;
        function(uint64, uint64) internal pure returns (uint64) q = sum;
        assembly { p := q }
        return p(3, 2);
    }
    function publicDecimal(uint64 a, uint64 b) public pure returns (uint64) { return a * 10 + b; }
    function publicSum(uint64 a, uint64 b) public pure returns (uint64) { return a + b; }
    function externalReassigned() external view returns (uint64) {
        function(uint64, uint64) external pure returns (uint64) p = this.publicDecimal;
        p = this.publicSum;
        return p(3, 2);
    }
    function forms(bool second) external returns (uint64, uint64, uint64, uint64) {
        stored = second ? sum : decimal;
        Box memory box = Box(stored);
        function(uint64, uint64) internal pure returns (uint64)[2] memory array = [decimal, sum];
        return (stored(3, 2), CallBoundaryPointerBase.stored(3, 2),
            box.pointer(3, 2), array[second ? 1 : 0](3, 2));
    }
    function sequenced(bool second) external pure returns (uint64, uint64) {
        function(uint64, uint64) internal pure returns (uint64) p = second ? sum : decimal;
        uint64 x = 1;
        return (p(x, x++), x);
    }
    function choose() internal returns (function(uint64, uint64) internal pure returns (uint64)) {
        trace = trace * 10 + 1;
        return decimal;
    }
    function mark(uint64 n) internal returns (uint64) { trace = trace * 10 + n; return n; }
    function calleeOrder() external returns (uint64, uint64) {
        trace = 0;
        uint64 result = choose()(mark(2), mark(3));
        return (result, trace);
    }
    function assignedInArgument() external pure returns (uint64) {
        function(uint64, uint64) internal pure returns (uint64) p = decimal;
        return p((p = sum)(1, 1), 3);
    }
    function replaceStored() internal returns (uint64) { stored = sum; return 2; }
    function stateCalleeOrder() external returns (uint64) {
        stored = decimal;
        return stored(replaceStored(), 3);
    }
    function bump(Value memory value) internal pure { value.n += 7; }
    function stableReference() external pure returns (uint64) {
        Value memory value = Value(3);
        function(Value memory) internal pure p = bump;
        p(value);
        return value.n;
    }
}
