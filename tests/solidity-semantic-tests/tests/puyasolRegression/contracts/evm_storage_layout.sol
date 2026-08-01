// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

contract EvmFull {
    uint256 internal a = 41;               // slot 0, ctor init
    uint64 internal b;                     // slot 1 packed
    uint32 internal c;                     //   with b
    bool internal d;                       //   and c
    int64 internal e;                      //   and d
    address internal owner;                // slot 2 (full-slot account)
    mapping(address => uint256) internal bal;      // slot 3
    mapping(uint256 => mapping(uint256 => int256)) internal grid; // slot 4
    uint256[] internal nums;               // slot 5
    uint32[] internal packedNums;          // slot 6
    uint256[4] internal fixedArr;          // slots 7..10
    struct Pos { uint128 x; uint128 y; uint256 z; }
    Pos internal pos;                      // slots 11..12
    mapping(uint256 => Pos) internal poss; // slot 13
    mapping(string => uint256) internal named; // slot 14
    string internal tag;                       // slot 15 (EVM short/long form)
    mapping(uint256 => string) internal notes; // slot 16

    constructor() {
        owner = msg.sender;
        b = 7;
    }

    function getA() external view returns (uint256) { return a; }
    function bump() external returns (uint256) { a++; ++a; a += 2; return a; }
    function getB() external view returns (uint64) { return b; }
    function setCD(uint32 v, bool f) external { c = v; d = f; }
    function getC() external view returns (uint32) { return c; }
    function getD() external view returns (bool) { return d; }
    function setE(int64 v) external { e = v; }
    function getE() external view returns (int64) { return e; }
    function getOwner() external view returns (address) { return owner; }
    function clearA() external { delete a; }

    function setBal(address w, uint256 v) external { bal[w] = v; }
    function addBal(address w, uint256 v) external { bal[w] += v; }
    function getBal(address w) external view returns (uint256) { return bal[w]; }
    function setGrid(uint256 x, uint256 y, int256 v) external { grid[x][y] = v; }
    function getGrid(uint256 x, uint256 y) external view returns (int256) { return grid[x][y]; }

    function pushNum(uint256 v) external { nums.push(v); }
    function popNum() external { nums.pop(); }
    function numAt(uint256 i) external view returns (uint256) { return nums[i]; }
    function setNum(uint256 i, uint256 v) external { nums[i] = v; }
    function numsLen() external view returns (uint256) { return nums.length; }

    function pushPacked(uint32 v) external { packedNums.push(v); }
    function packedAt(uint256 i) external view returns (uint32) { return packedNums[i]; }
    function setPacked(uint256 i, uint32 v) external { packedNums[i] = v; }

    function setFixed(uint256 i, uint256 v) external { fixedArr[i] = v; }
    function fixedAt(uint256 i) external view returns (uint256) { return fixedArr[i]; }

    function setPos(uint128 x, uint128 y, uint256 z) external { pos.x = x; pos.y = y; pos.z = z; }
    function getPosX() external view returns (uint128) { return pos.x; }
    function getPosZ() external view returns (uint256) { return pos.z; }
    function setPossY(uint256 k, uint128 y) external { poss[k].y = y; }
    function getPossY(uint256 k) external view returns (uint128) { return poss[k].y; }

    function setNamed(string calldata k, uint256 v) external { named[k] = v; }
    function getNamed(string calldata k) external view returns (uint256) { return named[k]; }

    function setTag(string calldata v) external { tag = v; }
    function getTag() external view returns (string memory) { return tag; }
    function tagLen() external view returns (uint256) { return bytes(tag).length; }
    function clearTag() external { delete tag; }
    function setNote(uint256 k, string calldata v) external { notes[k] = v; }
    function getNote(uint256 k) external view returns (string memory) { return notes[k]; }
    // raw word of tag's slot — verifies the EVM short-string form directly
    function tagWord() external view returns (uint256 r) {
        assembly { r := sload(tag.slot) }
    }

    // ── assembly slot arithmetic, the point of the mode ──
    function rawSlot(uint256 s) external view returns (uint256 r) {
        assembly { r := sload(s) }
    }
    function rawStore(uint256 s, uint256 v) external {
        assembly { sstore(s, v) }
    }
    // read `a` via its .slot constant
    function aViaAsm() external view returns (uint256 r) {
        assembly { r := sload(a.slot) }
    }
    // OZ StorageSlot-style: computed slot written in asm, read in Solidity
    function storeBalAsm(address w, uint256 v) external {
        uint256 p;
        assembly {
            mstore(0x00, w)
            mstore(0x20, bal.slot)
            p := keccak256(0x00, 0x40)
            sstore(p, v)
        }
    }
    // Checkpoints-style: element access via add(keccak256(...), i)
    function numAtAsm(uint256 i) external view returns (uint256 r) {
        assembly {
            mstore(0x00, nums.slot)
            let data := keccak256(0x00, 0x20)
            r := sload(add(data, i))
        }
    }
}
