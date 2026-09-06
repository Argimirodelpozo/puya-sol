pragma solidity >=0.8.20;

contract HolderAliases {
    struct Entry { uint64 tag; mapping(uint256 => uint256) values; }
    Entry[2] entries;
    mapping(uint256 => Entry) groups;
    uint256 cursor;

    function select(uint256 index) internal view returns (mapping(uint256 => uint256) storage) {
        return entries[index].values;
    }

    function group(uint256 index) internal view returns (Entry storage) { return groups[index]; }

    function aliases() public returns (bool) {
        cursor = 0;
        Entry storage first = entries[cursor++];
        require(cursor == 1);
        cursor = 1;
        first.tag = 7;
        first.values[1] = 11;
        first.values[2] = 12;
        require(cursor == 1 && entries[0].tag == 7 && entries[1].tag == 0);
        require(entries[0].values[1] == 11 && entries[1].values[1] == 0);
        bool choose = true;
        mapping(uint256 => uint256) storage chosen = choose ? entries[0].values : entries[1].values;
        choose = false;
        chosen[3] = 13;
        require(entries[0].values[3] == 13 && entries[1].values[3] == 0);
        chosen = entries[1].values;
        chosen[3] = 23;
        return entries[0].values[2] == 12 && entries[0].values[3] == 13 && entries[1].values[3] == 23;
    }

    function returnedRefs() public returns (bool) {
        mapping(uint256 => uint256) storage returned = select(1);
        returned[5] = 25;
        Entry storage selected = group(7);
        selected.tag = 9;
        selected.values[5] = 35;
        return entries[1].values[5] == 25 && entries[0].values[5] == 0
            && groups[7].tag == 9 && groups[7].values[5] == 35 && groups[8].values[5] == 0;
    }

    function reboundAlias() public returns (bool) {
        Entry storage selected = entries[0];
        cursor = 1;
        selected = entries[cursor++];
        require(cursor == 2);
        cursor = 0;
        selected.tag = 19;
        selected.values[9] = 29;
        return cursor == 0 && entries[0].tag == 7 && entries[1].tag == 19
            && entries[0].values[9] == 0 && entries[1].values[9] == 29;
    }

    function tupleRebind() public returns (bool) {
        mapping(uint256 => uint256) storage left = entries[0].values;
        mapping(uint256 => uint256) storage right = entries[1].values;
        (left, right) = (right, left);
        left[10] = 30;
        right[10] = 40;
        if (cursor == 0) (left, right) = (right, left);
        left[11] = 31;
        {
            mapping(uint256 => uint256) storage left = entries[1].values;
            left[12] = 32;
        }
        left[12] = 42;
        return entries[0].values[10] == 40 && entries[1].values[10] == 30
            && entries[0].values[11] == 31 && entries[1].values[11] == 0
            && entries[0].values[12] == 42 && entries[1].values[12] == 32;
    }
}
