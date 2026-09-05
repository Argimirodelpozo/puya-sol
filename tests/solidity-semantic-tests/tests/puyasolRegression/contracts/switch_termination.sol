// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

contract SwitchTermination {
    function exhaustive(uint256 x) external pure returns (uint256) {
        assembly {
            switch x
            case 0 { mstore(0, 11) return(0, 32) }
            default { mstore(0, 22) return(0, 32) }
        }
    }

    function nonExhaustive(uint256 x) external pure returns (uint256) {
        assembly {
            switch x
            case 0 { mstore(0, 33) return(0, 32) }
        }
        return 44;
    }

    function loop(uint256 x) external pure returns (uint256 result) {
        assembly {
            for { let i := 0 } lt(i, 3) { i := add(i, 1) } {
                switch x
                case 0 { continue }
                case 1 { break }
                default { result := add(result, 1) }
            }
        }
        result += 10;
    }
}
