// SPDX-License-Identifier: MIT
pragma solidity ^0.8.34;

contract ExpressionConversions {
    int256 transient scratch;

    function conditional(bool c, int8 a, int256 b) external pure returns (int256) {
        return c ? a : b;
    }

    function pair(int8 a) internal pure returns (int8, int8) { return (a, -2); }

    function tupleValues(int8 a, bool call) external pure returns (int256, int256) {
        int256 x; int256 y;
        if (call) (x, y) = pair(a);
        else (x, y) = (a, int8(-2));
        return (x, y);
    }

    function tupleConditional(bool c, int8 a, int8 b) external pure returns (int256, int256) {
        return c ? (a, b) : (b, a);
    }

    function nestedTuple(int8 value) external pure returns (int256, int256) {
        int256 a; int256 b;
        (((a, ), b)) = ((value, 5), value);
        return (a, b);
    }

    function arrayValue(int8 a) external pure returns (int256) {
        int256[2] memory values = [int256(0), a];
        return values[1];
    }

    function transientValue(int8 a) external returns (int256, int256) {
        int256 result = (scratch = a);
        return (result, scratch);
    }

    function transientTuple(int8 a) external returns (int256, int256) {
        int256 other;
        (scratch, other) = (a, int8(-2));
        return (scratch, other);
    }

    function blobValue(int8 a) external pure returns (int256, int256) {
        int256[2] memory values;
        assembly { mstore(values, 0) }
        int256 result = (values[1] = a);
        return (result, values[1]);
    }
}
