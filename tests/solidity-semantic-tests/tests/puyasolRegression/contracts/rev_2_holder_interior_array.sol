pragma solidity >=0.8.20;

contract HolderInterior {
    struct Entry { uint256 tag; mapping(uint256 => uint256) values; }
    Entry[2] entries;

    function update(Entry storage entry) internal {
        entry.tag = 7;
        entry.values[5] = 17;
    }

    function run() public returns (bool) {
        Entry storage alias_ = entries[1];
        update(alias_);
        return entries[1].tag == 7 && entries[1].values[5] == 17
            && entries[0].tag == 0 && entries[0].values[5] == 0;
    }
}
