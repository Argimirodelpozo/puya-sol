// SPDX-License-Identifier: MIT
pragma solidity ^0.8.4;

import "../contracts/utils/LibPRNG.sol";

contract LibPRNGWrapper {
    using LibPRNG for LibPRNG.PRNG;

    function next(uint256 seed) external pure returns (uint256) {
        LibPRNG.PRNG memory prng;
        prng.seed(seed);
        return prng.next();
    }

    function uniform(uint256 seed, uint256 upper) external pure returns (uint256) {
        LibPRNG.PRNG memory prng;
        prng.seed(seed);
        return prng.uniform(upper);
    }
}
