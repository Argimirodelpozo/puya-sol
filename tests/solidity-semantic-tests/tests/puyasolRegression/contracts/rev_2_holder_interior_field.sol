pragma solidity >=0.8.20;

contract HolderInterior {
    struct Entry { uint256 tag; mapping(uint256 => uint256) values; }
    struct Pair { Entry left; Entry right; }
    Pair root;

    function update(Entry storage entry) internal {
        entry.tag = 7;
        entry.values[5] = 17;
    }

    function run() public returns (bool) {
        update(root.right);
        return root.right.tag == 7 && root.right.values[5] == 17
            && root.left.tag == 0 && root.left.values[5] == 0;
    }
}
