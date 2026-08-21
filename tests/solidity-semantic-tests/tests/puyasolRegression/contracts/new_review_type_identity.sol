// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

import {ReviewCollision as ReviewA} from "./new_review_type_a.sol";
import {ReviewCollision as ReviewB} from "./new_review_type_b.sol";

// C16: same-spelled file-level types retain declaration-specific layouts.
contract NewReviewTypeIdentity {
    function layouts(uint256 a, uint256 b, bool flag)
        external pure returns (uint256, uint256, bool)
    {
        ReviewA memory one = ReviewA(a);
        ReviewB memory two = ReviewB(b, flag);
        return (one.first, two.first, two.second);
    }
}
