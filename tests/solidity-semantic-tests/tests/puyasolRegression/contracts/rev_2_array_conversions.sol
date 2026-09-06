// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

contract ArrayConversions {
    uint256 internal calls;
    int16[] internal dynamicValues;
    int16[3] internal fixedValues;
    int128[] internal wideValues;
    uint16[] internal unsignedValues;
    int16[] internal initialized = [int8(-7), int8(9)];
    int16[] internal initializedDynamic = dynamicSource();
    struct Holder { int16[] values; }
    Holder internal holder;

    function next() internal returns (int8) { ++calls; return -3; }
    function fixedSource() internal returns (int8[2] memory) { return [next(), next()]; }
    function dynamicSource() internal returns (int8[] memory values) {
        values = new int8[](2);
        values[0] = next();
        values[1] = next();
    }

    function literalCopy() external returns (uint256, int16, int16) {
        calls = 0;
        dynamicValues = [next(), next()];
        return (calls, dynamicValues[0], dynamicValues[1]);
    }
    function fixedToDynamic() external returns (uint256, int16, int16) {
        calls = 0;
        dynamicValues = fixedSource();
        return (calls, dynamicValues[0], dynamicValues[1]);
    }
    function dynamicToDynamic() external returns (uint256, int16, int16) {
        calls = 0;
        dynamicValues = dynamicSource();
        return (calls, dynamicValues[0], dynamicValues[1]);
    }
    function tupleCopy() external returns (uint256, int16, int128) {
        calls = 0;
        (dynamicValues, wideValues) = (dynamicSource(), dynamicSource());
        return (calls, dynamicValues[0], wideValues[1]);
    }
    function memberCopy() external returns (uint256, int16, int16) {
        calls = 0;
        holder.values = dynamicSource();
        return (calls, holder.values[0], holder.values[1]);
    }
    function emptyCopy() external returns (uint256) {
        dynamicValues = new int8[](0);
        return dynamicValues.length;
    }
    function consume(int8[2] memory values) internal pure returns (int8[2] memory) {
        return values;
    }
    function argumentReturn() external returns (uint256, int16, int16) {
        calls = 0;
        dynamicValues = consume(fixedSource());
        return (calls, dynamicValues[0], dynamicValues[1]);
    }
    function fixedToFixed() external returns (uint256, int16, int16, int16) {
        calls = 0;
        fixedValues = fixedSource();
        return (calls, fixedValues[0], fixedValues[1], fixedValues[2]);
    }
    function literalToFixed() external returns (uint256, int16, int16, int16) {
        calls = 0;
        fixedValues = [next(), next()];
        return (calls, fixedValues[0], fixedValues[1], fixedValues[2]);
    }
    function wide(int72 a, int72 b) external returns (int128, int128) {
        wideValues = [a, b];
        return (wideValues[0], wideValues[1]);
    }
    function unsignedCopy(uint8 a) external returns (uint16) {
        unsignedValues = [a];
        return unsignedValues[0];
    }
    function initial() external view returns (int16, int16) {
        return (initialized[0], initialized[1]);
    }
    function initialDynamic() external view returns (int16, int16) {
        return (initializedDynamic[0], initializedDynamic[1]);
    }
}
