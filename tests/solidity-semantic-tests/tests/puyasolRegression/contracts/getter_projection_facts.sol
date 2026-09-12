// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

type GetterSigned is int72;

contract GetterProjectionFacts {
    struct Row {
        int8 negative;
        uint128 positive;
        bool flag;
        bytes5 tag;
        bytes data;
        uint256[] hidden;
        mapping(uint256 => uint256) omitted;
    }
    struct Single { GetterSigned negative; uint64[] hidden; }
    mapping(uint64 => Row) public keyed;
    mapping(uint64 => Row[2]) public ranked;
    Row[] public rows;
    Single public single;
    function populate() external {
        // Direct field writes keep this getter test independent of the named
        // backend's restriction on interior mapping-containing reference args.
        keyed[3].negative = -7;
        keyed[3].positive = 2**100 + 3;
        keyed[3].flag = true;
        keyed[3].tag = hex"0102030405";
        keyed[3].data = hex"123456";
        keyed[3].hidden.push(77);
        keyed[3].omitted[0] = 99;
        ranked[4][1].negative = -7;
        ranked[4][1].positive = 2**100 + 3;
        ranked[4][1].flag = true;
        ranked[4][1].tag = hex"0102030405";
        ranked[4][1].data = hex"123456";
        rows.push();
        rows[0].negative = -7;
        rows[0].positive = 2**100 + 3;
        rows[0].flag = true;
        rows[0].tag = hex"0102030405";
        rows[0].data = hex"123456";
        single.negative = GetterSigned.wrap(-9);
        single.hidden.push(88);
    }
}
