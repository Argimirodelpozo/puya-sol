// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

contract OperandEffectFacts {
    function pack(uint64 first, uint64 second) private pure returns (uint64) {
        return first * 10 + second;
    }
    function locals() external pure returns (uint64, uint64) {
        uint64 x = 1;
        uint64 result = pack(x, x++);
        return (result, x);
    }
    function preIncrement() external pure returns (uint64, uint64) {
        uint64 x = 1;
        uint64 result = pack(x, ++x);
        return (result, x);
    }
    function assigned() external pure returns (uint64, uint64) {
        uint64 x = 1;
        uint64 result = pack(x, x = 7);
        return (result, x);
    }
    function nested() external pure returns (uint64, uint64) {
        uint64 x = 1;
        uint64 result = pack(x + 2, pack(x, x++));
        return (result, x);
    }
    function control() external pure returns (uint64, uint64) {
        uint64 x = 1;
        return (pack(x, x + 1), x);
    }
    function mutate(uint64[] memory values) private pure returns (uint64) {
        values[0] = 9;
        return 1;
    }
    function memoryArgs() external pure returns (uint64, uint64) {
        uint64[] memory values = new uint64[](1);
        values[0] = 3;
        uint64 result = pack(values[0], mutate(values));
        return (result, values[0]);
    }
    function memoryBinary(bool mutationOnLeft) external pure returns (uint64, uint64) {
        uint64[] memory values = new uint64[](1);
        values[0] = 3;
        uint64 result = mutationOnLeft ? mutate(values) + values[0] : values[0] + mutate(values);
        return (result, values[0]);
    }
    function mutateBlob(uint64[520] memory values) private pure returns (uint64) {
        values[0] = 9;
        return 1;
    }
    function blobArgs() external pure returns (uint64, uint64) {
        uint64[520] memory values;
        values[0] = 3;
        uint64 result = pack(values[0], mutateBlob(values));
        return (result, values[0]);
    }
}
