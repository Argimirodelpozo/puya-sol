// SPDX-License-Identifier: MIT
pragma solidity ^0.8.4;

import "../contracts/utils/LibBitmap.sol";

contract LibBitmapWrapper {
    using LibBitmap for LibBitmap.Bitmap;

    LibBitmap.Bitmap private bitmap;

    function set(uint256 index) external {
        bitmap.set(index);
    }

    function unset(uint256 index) external {
        bitmap.unset(index);
    }

    function get(uint256 index) external view returns (bool) {
        return bitmap.get(index);
    }

    function toggle(uint256 index) external returns (bool) {
        return bitmap.toggle(index);
    }
}
