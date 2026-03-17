// SPDX-License-Identifier: MIT
pragma solidity ^0.8.4;

import "../contracts/utils/SafeCastLib.sol";

contract SafeCastWrapper {
    using SafeCastLib for uint256;
    using SafeCastLib for int256;

    function toUint8(uint256 x) external pure returns (uint8) {
        return x.toUint8();
    }

    function toUint16(uint256 x) external pure returns (uint16) {
        return x.toUint16();
    }

    function toUint32(uint256 x) external pure returns (uint32) {
        return x.toUint32();
    }

    function toUint64(uint256 x) external pure returns (uint64) {
        return x.toUint64();
    }

    function toUint128(uint256 x) external pure returns (uint128) {
        return x.toUint128();
    }

    function toInt8(int256 x) external pure returns (int8) {
        return x.toInt8();
    }

    function toInt256(uint256 x) external pure returns (int256) {
        return x.toInt256();
    }

    function toUint256(int256 x) external pure returns (uint256) {
        return x.toUint256();
    }
}
