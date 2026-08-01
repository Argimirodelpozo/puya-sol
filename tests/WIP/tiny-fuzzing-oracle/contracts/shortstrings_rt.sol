// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

// OZ ShortStrings round-trip, reduced: pack a short string into a bytes32 with
// the length in the LOW byte, then unpack it again.
type ShortString is bytes32;

library SSLib {
    error StringTooLong(string str);

    function toShortString(string memory str) internal pure returns (ShortString) {
        bytes memory bstr = bytes(str);
        if (bstr.length > 31) revert StringTooLong(str);
        return ShortString.wrap(bytes32(uint256(bytes32(bstr)) | bstr.length));
    }

    function byteLength(ShortString sstr) internal pure returns (uint256) {
        return uint256(ShortString.unwrap(sstr)) & 0xFF;
    }

    function toString(ShortString sstr) internal pure returns (string memory) {
        uint256 len = byteLength(sstr);
        string memory str = new string(32);
        assembly {
            mstore(str, len)
            mstore(add(str, 0x20), sstr)
        }
        return str;
    }
}

contract ShortStringsRT {
    // the USDe shape: the packed value lives in an IMMUTABLE, and the read goes
    // through a fallback wrapper, exactly like OZ ShortStrings/EIP712.
    ShortString private immutable _name;
    string private _nameFallback;
    bytes32 private constant _FALLBACK_SENTINEL =
        0x00000000000000000000000000000000000000000000000000000000000000FF;

    constructor() { _name = SSLib.toShortString("USDe"); }

    function nameViaImmutable() external view returns (string memory) {
        return _toStringWithFallback(_name, _nameFallback);
    }
    function _toStringWithFallback(ShortString value, string storage store)
        internal view returns (string memory)
    {
        if (ShortString.unwrap(value) != _FALLBACK_SENTINEL) return SSLib.toString(value);
        return store;
    }

    function roundTrip(string calldata s) external pure returns (string memory) {
        require(bytes(s).length <= 31);
        return SSLib.toString(SSLib.toShortString(s));
    }
    function packedLen(string calldata s) external pure returns (uint256) {
        require(bytes(s).length <= 31);
        return SSLib.byteLength(SSLib.toShortString(s));
    }
    function packedWord(string calldata s) external pure returns (bytes32) {
        require(bytes(s).length <= 31);
        return ShortString.unwrap(SSLib.toShortString(s));
    }
}
