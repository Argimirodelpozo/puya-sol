// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

contract ModifierReturns {
    modifier twice() { _; _; }
    modifier repeat(uint64 n) { for (uint64 i; i < n; ++i) _; }
    modifier seed(uint64 ignored) { _; }
    modifier seedOrSkip(uint64 ignored, bool run) { if (run) _; }
    modifier signedSeedOrSkip(int16 ignored, bool run) { if (run) _; }
    modifier stop(bool beforeBody, bool afterBody) {
        if (beforeBody) return;
        _;
        if (afterBody) return;
        _;
    }

    function f(uint64 x) external pure twice returns (uint64 value) { value += x; }
    function explicitReturn(uint64 x) external pure twice returns (uint64 value) {
        value += x;
        return value;
    }
    function nested(uint64 x) external pure twice twice returns (uint64 value) { value += x; }
    function looped(uint64 x, uint64 n) external pure repeat(n) returns (uint64 value) { value += x; }
    function signedTuple(uint64 x) external pure twice returns (uint64 value, int16 negative) {
        value += x;
        negative -= 2;
    }
    function allUnnamed(uint64 x) external pure twice returns (uint64, int16) { return (x, -3); }
    function signedScalar(int16 x) external pure twice returns (int16 value) { value += x; }
    function signedSeeded(bool run) external pure signedSeedOrSkip(value = -5, run) returns (int16 value) {
        value -= 2;
    }

    // The body's inputs need not be zero: modifier arguments can modify them.
    function seeded(uint64 x) external pure seed(value = 5) twice returns (uint64 value) { value += x; }
    function perInvocation(uint64 x) external pure twice seed(value = value + 3) returns (uint64 value) {
        value += x;
    }
    // via-IR snapshots outputs before evaluating arguments, even without `_`.
    function skipped(uint64 x, bool run) external pure seedOrSkip(value = 5, run) returns (uint64 value) {
        value += x;
    }
    function nestedSkip(uint64 x, bool run) external pure
        seed(value = 5) seedOrSkip(value = 9, run) returns (uint64 value) { value += x; }
    function earlyReturn(uint64 x, bool beforeBody, bool afterBody) external pure
        stop(beforeBody, afterBody) returns (uint64 value) { value += x; }

    function update(uint64[] memory state) internal pure twice returns (uint64 value) {
        state[0] += 1;
        value += state[0];
    }
    function memoryWriteBack(uint64 x) external pure returns (uint64, uint64) {
        uint64[] memory state = new uint64[](1);
        state[0] = x;
        uint64 value = update(state);
        return (value, state[0]);
    }

    uint64 private calls;
    function countBody() internal twice returns (uint64 value) {
        ++calls;
        value += calls;
    }
    function storageEffects() external returns (uint64, uint64) {
        calls = 0;
        uint64 value = countBody();
        return (value, calls);
    }
}
