pragma solidity ^0.8.24;

type OperatorWord is uint64;
using {operatorInvert as ~, operatorNegate as -, operatorPair as +} for OperatorWord global;

function operatorInvert(OperatorWord value) pure returns (OperatorWord) {
    return OperatorWord.wrap(OperatorWord.unwrap(value) + 7);
}

function operatorNegate(OperatorWord value) pure returns (OperatorWord) {
    return OperatorWord.wrap(OperatorWord.unwrap(value) + 11);
}

library OperatorHost {
    modifier pass() { _; }
    function pair(uint64 left, uint64 right) internal pure pass returns (uint64) {
        return left * 10 + right;
    }
}

function operatorPair(OperatorWord left, OperatorWord right) pure returns (OperatorWord) {
    return OperatorWord.wrap(OperatorHost.pair(OperatorWord.unwrap(left), OperatorWord.unwrap(right)));
}

contract OperatorCalls {
    uint64 private count;

    function unary(uint64 value) external pure returns (uint64, uint64) {
        return (OperatorWord.unwrap(~OperatorWord.wrap(value)),
            OperatorWord.unwrap(-OperatorWord.wrap(value)));
    }

    function binary(uint64 left, uint64 right) external pure returns (uint64) {
        return OperatorWord.unwrap(OperatorWord.wrap(left) + OperatorWord.wrap(right));
    }

    function next() internal returns (OperatorWord) {
        count++;
        return OperatorWord.wrap(count);
    }

    function sequenced() external returns (uint64, uint64) {
        count = 0;
        uint64 result = OperatorWord.unwrap(next() + next());
        return (result, count);
    }
}
