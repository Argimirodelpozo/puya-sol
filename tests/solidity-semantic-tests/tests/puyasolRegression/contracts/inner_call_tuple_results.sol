// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// Multiple inner calls built inside ONE expression (a return tuple) must each
// capture their OWN result: the AVM itxn context is a single register, so all
// submits flush (pre-pending) before the tuple evaluates — a live LastLog read
// in each slot returned the LAST call's value for every slot (v=w=ln all read
// ln()'s 3). Fixed by capture-after-submit (InnerCallHandlers::captureLastLog).
contract Cee {
    function a() external pure returns (uint256) { return 11; }
    function b() external pure returns (uint256) { return 22; }
    function c() external pure returns (uint256) { return 33; }
}
contract Caller {
    Cee s;
    constructor() { s = new Cee(); }
    function tup() external returns (uint256, uint256, uint256) {
        return (s.a(), s.b(), s.c());   // three inner calls in ONE tuple
    }
    function nested() external returns (uint256) {
        return add2(s.a(), s.c());       // two inner calls as ARGS of one call
    }
    function add2(uint256 x, uint256 y) internal pure returns (uint256) { return x + y; }
}
