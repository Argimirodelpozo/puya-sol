// SPDX-License-Identifier: GPL-3.0-or-later

pragma solidity >=0.8.0;

import '../libraries/Babylonian.sol';

contract BabylonianEchidnaTest {
    function checkSqrt(uint256 input) public pure {
        uint256 sqrtVal = Babylonian.sqrt(input);

        assert(sqrtVal < 2**128); // 2**128 == sqrt(2^256)
        // since we compute floor(sqrt(input))
        unchecked {
            assert(sqrtVal**2 <= input);
            assert((sqrtVal + 1)**2 > input || sqrtVal == type(uint128).max);
        }
    }

    function checkMaxForIndex(uint8 index) external pure {
        checkSqrt(index == 255 ? type(uint256).max : uint256(2)**(index + 1));
    }
}
