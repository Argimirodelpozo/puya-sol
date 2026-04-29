// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

contract CountersTest {
    uint256 private _counter;

    function current() external view returns (uint256) {
        return _counter;
    }

    function increment() external {
        _counter += 1;
    }

    function decrement() external {
        require(_counter > 0, "Counter: decrement overflow");
        _counter -= 1;
    }

    function reset() external {
        _counter = 0;
    }
}
