// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

type BoundarySigned is int16;

function freeInspect(bytes calldata data, uint16 extra) pure returns (uint64 result) {
    assembly { result := add(byte(0, calldataload(data.offset)), extra) }
}

library BoundaryLibrary {
    function inspect(bytes calldata data, uint16 extra) internal pure returns (uint64 result) {
        assembly { result := add(byte(0, calldataload(data.offset)), extra) }
    }
}

contract CallBoundaryValues {
    struct Record { uint8 small; BoundarySigned negative; uint128 wide; bytes data; }
    Record public record;
    mapping(uint128 => Record) public records;

    constructor() {
        record = Record(251, BoundarySigned.wrap(-123), (uint128(1) << 100) + 7, hex"010203");
        records[(uint128(1) << 80) + 9] = record;
    }
    function hostedInspect(bytes calldata data, uint16 extra) internal pure returns (uint64 result) {
        assembly { result := add(byte(0, calldataload(data.offset)), extra) }
    }
    function contexts(bytes calldata data, uint16 extra) external pure returns (uint64, uint64, uint64) {
        return (hostedInspect(data, extra), BoundaryLibrary.inspect(data, extra), freeInspect(data, extra));
    }
    function scalar() public pure returns (int16) { return -123; }
    function otherScalar() public pure returns (int16) { return -22; }
    function directScalar() external view returns (int16, int16) { return (scalar(), this.scalar()); }
    function pointerScalar(bool other) external view returns (int16) {
        function() external pure returns (int16) p = other ? this.otherScalar : this.scalar;
        return p();
    }
    function tupleValue() public pure returns (int16, uint8, uint128) {
        return (-123, 251, (uint128(1) << 100) + 7);
    }
    function directTuple() external view returns (int16, uint8, uint128) { return this.tupleValue(); }
    function pointerTuple() external view returns (int16, uint8, uint128) {
        function() external pure returns (int16, uint8, uint128) p;
        p = this.tupleValue;
        return p();
    }
    function selfRecord() external view returns (uint8, BoundarySigned, uint128, bytes memory) {
        return this.record();
    }
    function selfKeyedRecord(uint128 key) external view returns (uint8, BoundarySigned, uint128, bytes memory) {
        return this.records(key);
    }
    function bytesValue() public pure returns (bytes memory) { return hex"00ff0102"; }
    function remoteTuple(CallBoundaryValues other) external view returns (int16, uint8, uint128) {
        function() external pure returns (int16, uint8, uint128) p = other.tupleValue;
        return p();
    }
    function directRemoteTuple(CallBoundaryValues other) external view returns (int16, uint8, uint128) {
        return other.tupleValue();
    }
    function remoteBytes(CallBoundaryValues other) external view returns (bytes memory) {
        function() external pure returns (bytes memory) p = other.bytesValue;
        return p();
    }
    function directRemoteBytes(CallBoundaryValues other) external view returns (bytes memory) {
        return other.bytesValue();
    }
    function remotePair(CallBoundaryValues other) external view returns (int16, int16) {
        function() external pure returns (int16) p = other.scalar;
        function() external pure returns (int16) q = other.otherScalar;
        return (p(), q());
    }
}
