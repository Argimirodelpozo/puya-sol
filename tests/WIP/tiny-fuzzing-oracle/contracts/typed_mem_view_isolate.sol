// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

/// Minimal isolate of the memview-sol representation used by Circle CCTP v1.
/// A bytes29 view packs a 5-byte type, 12-byte memory location, 12-byte length,
/// and three unused low bytes. The implementation intentionally retains the
/// raw-memory idioms from TypedMemView rather than replacing them with ordinary
/// Solidity slicing.
library TypedMemViewIsolate {
    bytes29 internal constant NULL =
        hex"ffffffffffffffffffffffffffffffffffffffffffffffffffffffffff";
    uint256 internal constant LOW_12_MASK = 0xffffffffffffffffffffffff;

    function unsafeBuildUnchecked(uint256 viewType, uint256 location, uint256 length)
        internal pure returns (bytes29 memView)
    {
        assembly {
            memView := shl(96, or(memView, viewType))
            memView := shl(96, or(memView, location))
            memView := shl(24, or(memView, length))
        }
    }

    function build(uint256 viewType, uint256 location, uint256 length)
        internal pure returns (bytes29 memView)
    {
        uint256 endpoint = location + length;
        assembly {
            if gt(endpoint, mload(0x40)) { endpoint := 0 }
        }
        if (endpoint == 0) return NULL;
        return unsafeBuildUnchecked(viewType, location, length);
    }

    function ref(bytes memory data, uint40 newType)
        internal pure returns (bytes29)
    {
        uint256 location;
        assembly { location := add(data, 0x20) }
        return build(newType, location, data.length);
    }

    function typeOf(bytes29 memView) internal pure returns (uint40 viewType) {
        assembly { viewType := shr(216, memView) }
    }

    function loc(bytes29 memView) internal pure returns (uint96 location) {
        uint256 mask = LOW_12_MASK;
        assembly { location := and(shr(120, memView), mask) }
    }

    function len(bytes29 memView) internal pure returns (uint96 length) {
        uint256 mask = LOW_12_MASK;
        assembly { length := and(shr(24, memView), mask) }
    }

    function end(bytes29 memView) internal pure returns (uint256) {
        return uint256(loc(memView)) + uint256(len(memView));
    }

    function slice(bytes29 memView, uint256 start, uint256 length, uint40 newType)
        internal pure returns (bytes29)
    {
        uint256 location = loc(memView);
        if (location + start + length > end(memView)) return NULL;
        return build(newType, location + start, length);
    }

    function leftMask(uint8 bitLength) private pure returns (uint256 mask) {
        assembly {
            mask := sar(
                sub(bitLength, 1),
                0x8000000000000000000000000000000000000000000000000000000000000000
            )
        }
    }

    function index(bytes29 memView, uint256 start, uint8 width)
        internal pure returns (bytes32 result)
    {
        if (width == 0) return bytes32(0);
        require(width <= 32, "index width");
        require(start + width <= len(memView), "index overrun");
        uint8 bitLength = width * 8;
        uint256 location = loc(memView);
        uint256 mask = leftMask(bitLength);
        assembly { result := and(mload(add(location, start)), mask) }
    }

    function indexUint(bytes29 memView, uint256 start, uint8 width)
        internal pure returns (uint256)
    {
        return uint256(index(memView, start, width)) >> ((32 - width) * 8);
    }
}

contract TypedMemViewProbe {
    using TypedMemViewIsolate for bytes;
    using TypedMemViewIsolate for bytes29;

    /// Pure pack/unpack arithmetic, independent of a VM's concrete memory base.
    function packRoundTrip(uint40 viewType, uint96 location, uint96 length)
        external pure returns (bytes29, uint40, uint96, uint96)
    {
        bytes29 memView = TypedMemViewIsolate.unsafeBuildUnchecked(
            viewType, location, length);
        return (memView, memView.typeOf(), memView.loc(), memView.len());
    }

    /// ref(), typeOf(), loc(), and len(), compared through location-independent
    /// invariants because EVM and AVM concrete memory addresses need not match.
    function refMeta(bytes memory data, uint40 viewType)
        external pure returns (uint40, uint96, uint256, bool)
    {
        bytes29 memView = data.ref(viewType);
        uint96 location = memView.loc();
        return (memView.typeOf(), memView.len(), memView.end() - location, location != 0);
    }

    /// Exercise in-bounds and overrun slicing. A valid slice's location delta
    /// must equal start; an invalid slice exposes NULL's type/length instead.
    function sliceMeta(
        bytes memory data,
        uint40 viewType,
        uint8 start,
        uint8 length,
        uint40 newType
    ) external pure returns (bool valid, uint40, uint96, uint96) {
        bytes29 base = data.ref(viewType);
        bytes29 part = base.slice(start, length, newType);
        valid = part != TypedMemViewIsolate.NULL;
        return (
            valid,
            part.typeOf(),
            part.len(),
            valid ? part.loc() - base.loc() : uint96(0)
        );
    }

    /// Exercise the mload-based index and its big-endian integer projection.
    /// Width is capped to the TypedMemView contract; invalid ranges revert on
    /// both legs and are part of the differential verdict.
    function indexBoth(bytes memory data, uint40 viewType, uint8 start, uint8 width)
        external pure returns (bytes32, uint256)
    {
        bytes29 memView = data.ref(viewType);
        width %= 33;
        return (memView.index(start, width), memView.indexUint(start, width));
    }

    /// Compose ref -> slice -> index, the path Message.sol uses to parse fields.
    function sliceIndex(
        bytes memory data,
        uint8 start,
        uint8 length,
        uint8 indexStart,
        uint8 width
    ) external pure returns (bytes32, uint256) {
        bytes29 part = data.ref(1).slice(start, length, 2);
        require(part != TypedMemViewIsolate.NULL, "slice overrun");
        width %= 33;
        return (part.index(indexStart, width), part.indexUint(indexStart, width));
    }
}
