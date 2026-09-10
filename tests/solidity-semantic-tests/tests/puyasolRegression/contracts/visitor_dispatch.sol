// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

contract VisitorDispatch {
    uint64 private trace;

    function mark(uint64 digit) internal returns (uint64) {
        trace = trace * 10 + digit;
        return digit;
    }

    function expressions(bool choose) external returns (uint64[3] memory, uint64) {
        trace = 0;
        uint64[3] memory values;
        values[mark(1) % 3] = choose ? mark(2) + mark(3) * mark(4) : mark(5);
        (values[2], values[0]) = (mark(6), mark(7));
        ++values[0];
        --values[1];
        bool selected = (choose && mark(8) > 0) || (!choose && mark(9) > 0);
        assert(selected);
        return (values, trace);
    }

    function slices(bytes calldata data, uint256 start, uint256 end) external pure returns (bytes memory) {
        return data[start:end];
    }
}
