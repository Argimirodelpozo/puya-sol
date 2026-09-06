pragma solidity >=0.8.20;

contract HolderInterior {
    struct Entry { uint256 tag; mapping(uint256 => uint256) values; }
    struct Pair { Entry left; Entry right; }
    Pair root;

    function selected() internal view returns (Entry storage entry) { entry = root.right; }

    function run() public returns (bool) {
        Entry storage entry = selected();
        entry.tag = 7;
        entry.values[5] = 17;
        return root.right.tag == 7 && root.right.values[5] == 17
            && root.left.tag == 0 && root.left.values[5] == 0;
    }
}
