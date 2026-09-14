// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

// Public libraries can be emitted both as a deployable root and as a linked
// subroutine. Their source declaration IDs alone do not identify an emission.
library FirstYulLibrary {
    function f(uint256 x) public pure returns (uint256 result) {
        assembly {
            function twice(n) -> r { r := mul(n, 2) }
            result := twice(x)
        }
    }
}

library SecondYulLibrary {
    function f(uint256 x) public pure returns (uint256 result) {
        assembly {
            function plus(n) -> r { r := add(n, 1) }
            result := plus(x)
        }
    }
}
