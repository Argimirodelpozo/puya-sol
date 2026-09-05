// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

contract ConversionChild {}

contract ExplicitConversionPlan {
    enum Choice { First, Second, Third }
    function unsignedWidths(uint256 value) external pure returns (uint256, uint256, uint256, uint256, uint256, uint256) {
        return (uint8(value), uint24(value), uint64(value), uint72(value), uint128(value), uint248(value));
    }
    function signedWidths(int256 value) external pure returns (int256, int256, int256, int256, int256, int256) {
        return (int8(value), int24(value), int64(value), int72(value), int128(value), int248(value));
    }
    function widenSmall(int8 value) external pure returns (int16, int128) {
        return (int16(value), int128(value));
    }
    function reinterpret(int128 value) external pure returns (uint128, int128) {
        uint128 unsignedValue = uint128(value);
        return (unsignedValue, int128(unsignedValue));
    }
    function bytesMagnitude(bytes8 small, bytes16 wide) external pure returns (uint64, uint128) {
        return (uint64(small), uint128(wide));
    }
    function enumCheck(uint256 value) external pure returns (Choice) { return Choice(value); }
    function deploymentOnce() external returns (bool) {
        return address(new ConversionChild()) != address(0);
    }
}
