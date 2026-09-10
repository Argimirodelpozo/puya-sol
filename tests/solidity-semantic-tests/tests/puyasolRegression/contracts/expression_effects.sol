// SPDX-License-Identifier: MIT
pragma solidity ^0.8.34;

contract ExpressionEffects {
    uint64 private trace;
    uint64 transient transientValue;
    struct Cell { int256 value; uint64 other; }
    mapping(uint256 => Cell) private cells;
    uint64[2] private slots;
    bytes private storedBytes;

    function nextIndex() internal returns (uint256) { return trace++; }
    function tupleTargets(bool memoryTarget) external returns (uint256, uint256, uint256) {
        trace = 0;
        uint64[2] memory values;
        assembly { mstore(values, 0) }
        if (memoryTarget) {
            (values[nextIndex()], values[nextIndex()]) = (uint64(5), uint64(9));
            return (values[0], values[1], trace);
        }
        (slots[nextIndex()], slots[nextIndex()]) = (uint64(5), uint64(9));
        return (slots[0], slots[1], trace);
    }
    function bytesTargets() external returns (bytes1, bytes memory, uint256) {
        storedBytes = hex"0102";
        uint256 index;
        bytes1 assigned = (storedBytes[index++] |= bytes1(0x04));
        delete storedBytes[1];
        return (assigned, storedBytes, index);
    }

    function left(uint64[2] memory values) internal returns (uint64) {
        trace = trace * 10 + 1;
        values[0] = 9;
        return 1;
    }
    function right(uint64[2] memory values) internal returns (uint64) {
        trace = trace * 10 + 2;
        values[1] = 20;
        return 4;
    }
    function blobOrder(bool compound) external returns (uint256, uint256, uint256) {
        trace = 0;
        uint64[2] memory values = [uint64(3), uint64(7)];
        assembly { mstore(values, 3) }
        if (compound) values[left(values)] += right(values);
        else values[left(values)] = right(values);
        return (values[0], values[1], trace);
    }
    function bump(uint64[] memory values) internal pure returns (uint64) {
        values[0]++;
        return values[0];
    }
    function branches(bool condition, bool disjunction) external pure returns (uint256, uint256) {
        uint64[] memory values = new uint64[](1);
        int256 result = condition ? int8(-7) : int256(uint256(bump(values)));
        bool taken = disjunction ? (condition || bump(values) > 0) : (condition && bump(values) > 0);
        return (uint256(result + 10), uint256(values[0]) * 10 + (taken ? 1 : 0));
    }
    function targets(uint256 key) external returns (int256, int256, uint256, uint256) {
        cells[key].value = -7;
        int256 old = cells[key].value++;
        int256 next = ++cells[key].value;
        transientValue = 3;
        uint64 t = transientValue++;
        delete cells[key].value;
        delete transientValue;
        return (old, next, t, uint256(cells[key].value) + transientValue);
    }
    function unary(int8 value, bytes3 fixedBytes) external pure returns (int256, bytes3, bool) {
        return (-value, ~fixedBytes, !(value == 0));
    }
}
