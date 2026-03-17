// SPDX-License-Identifier: GPL-3.0
pragma solidity =0.6.12;

import '../UniswapV2Pair.sol';

/// @title Test wrapper that extends initialize to also set factory.
/// The real Pair sets factory = msg.sender in constructor, which doesn't work
/// on AVM where contracts can't deploy other contracts. This wrapper adds
/// a setFactory method for test setup.
contract PairTestHelper is UniswapV2Pair {
    function setFactory(address _factory) external {
        factory = _factory;
    }
}
