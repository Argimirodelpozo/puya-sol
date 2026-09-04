// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

contract InlineCtorChain {
    uint256 public trace;

    modifier twice() {
        _;
        trace = trace * 10 + 5;
        _;
    }

    constructor(uint256, uint256 digit) twice {
        trace = trace * 10 + digit;
    }
}

contract DeferredCtorChain {
    uint256 public trace;
    uint256[] private values;

    modifier twice() {
        _;
        trace = trace * 10 + 5;
        _;
    }

    constructor(uint256, uint256 digit) twice {
        trace = trace * 10 + digit;
        values.push(digit);
    }

    function count() external view returns (uint256) {
        return values.length;
    }
}

contract MemoryCtorChain {
    struct Cell {
        uint256 value;
    }

    uint256 public trace;

    modifier touch(Cell memory cell) {
        cell.value += 1;
        _;
        trace = trace * 10 + cell.value;
    }

    constructor(Cell memory cell) touch(cell) {
        trace = cell.value;
    }
}
