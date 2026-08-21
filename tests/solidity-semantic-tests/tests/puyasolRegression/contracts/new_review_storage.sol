// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

struct ReviewBaseHolder { mapping(uint256 => uint256) values; }
struct ReviewDerivedHolder { mapping(uint256 => uint256) values; }

contract NewReviewStorageBase {
    uint256 transient private status;
    ReviewBaseHolder[] private records;

    function setBaseTransient(uint256 value) internal { status = value; }
    function baseTransient() internal view returns (uint256) { return status; }

    function setBaseRecord(uint256 key, uint256 value) public {
        if (records.length == 0) records.push();
        records[0].values[key] = value;
    }

    function baseRecord(uint256 key) public view returns (uint256) {
        return records[0].values[key];
    }
}

contract NewReviewStorage is NewReviewStorageBase {
    uint256 transient private status;
    ReviewDerivedHolder[] private records;

    struct Owned { address owner; uint96 tag; }
    struct Cell { uint256 value; }

    Owned private owned;
    Cell[] private left;
    Cell[] private right;
    uint8[] private packed;
    uint256[] private dynamicValues;
    mapping(int8 => uint256) private signedMap;
    uint256 public derivations;
    uint256 public ownerFetches;

    function transientPair() external returns (uint256, uint256) {
        setBaseTransient(11);
        status = 22;
        return (baseTransient(), status);
    }

    function setDerivedRecord(uint256 key, uint256 value) public {
        if (records.length == 0) records.push();
        records[0].values[key] = value;
    }

    function derivedRecord(uint256 key) public view returns (uint256) {
        return records[0].values[key];
    }

    function recordPair(uint256 key) external returns (uint256, uint256) {
        setBaseRecord(key, 101);
        setDerivedRecord(key, 202);
        return (baseRecord(key), derivedRecord(key));
    }

    function fetchOwner(address value) internal returns (address) {
        ownerFetches += 1;
        return value;
    }

    // B13: address RHS executes once and the slot-backed field is updated.
    function assignOwner(address value) external returns (uint256, address) {
        ownerFetches = 0;
        Owned storage ref = owned;
        ref.owner = fetchOwner(value);
        return (ownerFetches, owned.owner);
    }

    function seedArrays() external {
        if (packed.length == 0) {
            packed.push(7);
            packed.push(9);
        }
        if (left.length == 0) {
            left.push(Cell(33));
            right.push(Cell(44));
        }
        if (dynamicValues.length == 0) {
            dynamicValues.push(5);
            dynamicValues.push(6);
            dynamicValues.push(7);
        }
    }

    // C9: the side-effecting packed index is shared by word and byte offset.
    function packedPostIncrement() external view returns (uint256, uint256) {
        uint256 index = 0;
        uint256 value = packed[index++];
        return (value, index);
    }

    function derivedArray() internal returns (uint256[] storage result) {
        derivations += 1;
        assembly { result.slot := dynamicValues.slot }
    }

    // C14: the storage-ref base is evaluated once across bounds/base hashing.
    function derivedRead(uint256 index) external returns (uint256, uint256) {
        derivations = 0;
        uint256 value = derivedArray()[index];
        return (value, derivations);
    }

    // B14: the false branch's pos-1 must not materialize when pos is zero.
    function conditionalRef(uint256 pos) external view returns (uint256) {
        Cell storage ref = pos == 0 ? left[0] : right[pos - 1];
        return ref.value;
    }

    // C6: assembly root access observes and mutates a box-backed array length.
    function rawDynamicLength() external view returns (uint256 result) {
        assembly { result := sload(dynamicValues.slot) }
    }

    function resizeDynamic(uint256 length) external {
        assembly { sstore(dynamicValues.slot, length) }
    }

    function dynamicLength() external view returns (uint256) {
        return dynamicValues.length;
    }

    // C20: a signed key hashes identically in Solidity and inline assembly.
    function signedKeyParity() external returns (uint256 result) {
        signedMap[-1] = 77;
        assembly {
            mstore(0, not(0))
            mstore(32, signedMap.slot)
            result := sload(keccak256(0, 64))
        }
    }
}
