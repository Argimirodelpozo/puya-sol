// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

contract Sink {
    struct P { uint64 x; uint64 y; }
    function take(P memory m) external pure returns (uint256) { return m.x + m.y; }
}

contract ExtArg {
    Sink.P a;
    Sink.P b;
    Sink sink;
    constructor() {
        a = Sink.P(1, 2);
        b = Sink.P(30, 40);
        sink = new Sink();
    }
    function viaExtArg(bool pick) external returns (uint256) {
        Sink.P storage s = pick ? a : b;
        return sink.take(s);
    }
}
