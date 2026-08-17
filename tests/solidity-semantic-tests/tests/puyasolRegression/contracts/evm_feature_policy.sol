// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

// CUSTOM puya-sol regression fixture — not an upstream Solidity semantic test.
contract EvmFeaturePolicyRegression {
    function configuredEnvironmentMatches() external view returns (bool) {
        return block.chainid == 11155111
            && block.gaslimit == 30000000
            && block.coinbase == address(0x1111111111111111111111111111111111111111);
    }
}
