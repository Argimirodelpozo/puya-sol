// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    uint256 public seq;
    // Side effect in an if-condition via postfix: i++ must commit BEFORE the
    // branch body reads i (EVM: condition evaluates fully first).
    function condOrder() external returns (uint256, uint256) {
        seq = 0;
        uint256 i = 0;
        if (i++ < 1) { seq = i * 10; }   // i==1 inside body -> seq=10
        return (seq, i);                  // expect (10, 1)
    }
    // ternary-in-condition with assignment
    function condAssignIf() external returns (uint256) {
        uint256 x = 0;
        if ((x = 5) > 3) { return x; }   // expect 5
        return 99;
    }
}
