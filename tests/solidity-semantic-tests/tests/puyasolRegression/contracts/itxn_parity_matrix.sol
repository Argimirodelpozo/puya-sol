// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// External-call semantics matrix vs solc (itxn/ audit). Cells where the AVM
// adaptation DIVERGES are asserted at their adapted values and documented in
// EVM_DIVERGENCE.md; the rest must match the EVM oracle.
contract ItxnCallee {
    function whoami() public payable returns (address s, uint256 v) {
        return (msg.sender, msg.value);
    }

    function five() public pure returns (uint256) {
        return 5;
    }
}

contract ItxnMatrix {
    ItxnCallee public c;
    uint256 public sideCount;

    constructor() {
        c = new ItxnCallee();
    }

    function bump() public returns (uint256) {
        sideCount++;
        return 100000;
    }

    function g() public payable returns (address s, uint256 v) {
        return (msg.sender, msg.value);
    }

    // EVM: this.g() is a real external CALL — g sees msg.sender ==
    // address(this) and msg.value == 0. AVM: self inner calls are FORBIDDEN
    // (no reentrancy), so this.g() lowers to a SUBROUTINE — g sees the
    // ORIGINAL msg.sender/msg.value. Documented divergence.
    function thisCallSender() public payable returns (bool senderIsSelf, uint256 innerValue) {
        (address s, uint256 v) = this.g();
        senderIsSelf = (s == address(this));
        innerValue = v;
    }

    // Typed cross-contract call with value: the callee must see the CALLER
    // CONTRACT as sender and the forwarded value. Equivalent on both.
    function crossValue() public payable returns (bool senderIsMe, uint256 v) {
        (address s, uint256 vv) = c.whoami{value: msg.value}();
        senderIsMe = (s == address(this));
        v = vv;
    }

    // solc EVALUATES call-option expressions: {gas: bump()} must bump once.
    function gasOptionSideEffect() public returns (uint256) {
        c.five{gas: bump()}();
        return sideCount;
    }

    // Typed call to a codeless address: EVM extcodesize-checks and reverts;
    // AVM inner call to a nonexistent app aborts. Same observable (revert).
    function typedCodeless() public returns (uint256) {
        return ItxnCallee(address(0x9999)).five();
    }
}
