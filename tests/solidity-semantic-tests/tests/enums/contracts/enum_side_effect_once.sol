// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    enum Color { R, G, B }
    event CE(Color c);
    uint256 public cnt;
    function f() internal returns (Color) { cnt++; return Color.G; }
    // emit with side-effecting enum arg: f() once (range-assert + field)
    function emitOnce() external returns (uint256) {
        cnt = 0;
        emit CE(f());
        return cnt;   // expect 1
    }
    // return of side-effecting enum value: f() once (range-assert + return)
    function retOnce() external returns (Color, uint256) {
        cnt = 0;
        Color c = f();
        return (c, cnt);   // expect (G=1, 1)
    }
    // direct return shape (the SolExpressionStatement path)
    function retDirect() external returns (Color) {
        cnt = 0;
        return f();   // returns G; cnt must be exactly 1 after
    }
}
