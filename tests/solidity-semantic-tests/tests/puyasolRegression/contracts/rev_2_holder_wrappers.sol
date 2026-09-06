pragma solidity >=0.8.20;

library HolderWrapperLib {
    struct Data { uint64 tag; uint256[] items; mapping(uint256 => uint256) values; }
    struct Wrapped { Data inner; }
    struct Deep { Wrapped inner; }

    function update(Data storage data, uint64 value) internal {
        data.tag = value;
        data.items.push(value);
        data.values[1] = value + 100;
    }
    function updateWrapped(Wrapped storage data, uint64 value) internal { update(data.inner, value); }
    function selected(Deep storage data) internal view returns (Data storage result) { result = data.inner.inner; }
}

contract HolderWrappers {
    using HolderWrapperLib for *;
    HolderWrapperLib.Wrapped first;
    HolderWrapperLib.Deep second;
    mapping(uint256 => HolderWrapperLib.Deep) groups;
    HolderWrapperLib.Wrapped[2] rows;

    function selected() internal view returns (HolderWrapperLib.Data storage) { return first.inner; }

    function run() public returns (bool) {
        first.updateWrapped(7);
        second.inner.inner.update(9);
        groups[3].inner.updateWrapped(11);
        require(first.inner.tag == 7 && first.inner.items[0] == 7 && first.inner.values[1] == 107);
        require(second.inner.inner.tag == 9 && second.inner.inner.items[0] == 9 && second.inner.inner.values[1] == 109);
        require(groups[3].inner.inner.tag == 11 && groups[3].inner.inner.values[1] == 111);
        require(groups[4].inner.inner.tag == 0 && groups[4].inner.inner.items.length == 0);
        first.inner.tag = 17;
        first.inner.items.push(18);
        HolderWrapperLib.Data storage alias_ = first.inner;
        alias_.values[2] = 19;
        require(alias_.tag == 17 && alias_.items[1] == 18 && first.inner.values[2] == 19);
        selected().update(21);
        second.selected().update(23);
        require(first.inner.tag == 21 && first.inner.items[2] == 21 && first.inner.values[1] == 121);
        require(second.inner.inner.tag == 23 && second.inner.inner.items[1] == 23 && second.inner.inner.values[1] == 123);
        delete first;
        require(first.inner.tag == 0 && first.inner.items.length == 0 && first.inner.values[1] == 121);
        first.updateWrapped(25);
        rows[0].inner.tag = 27;
        rows[1].inner.tag = 29;
        rows[1].inner.items.push(30);
        HolderWrapperLib.Data storage row = rows[1].inner;
        row.tag = 31;
        row.values[1] = 32;
        require(rows[0].inner.tag == 27 && rows[0].inner.values[1] == 0);
        require(rows[1].inner.tag == 31 && rows[1].inner.values[1] == 32 && row.items[0] == 30);
        return first.inner.tag == 25 && first.inner.items[0] == 25 && first.inner.values[1] == 125
            && second.inner.inner.tag == 23 && groups[3].inner.inner.tag == 11;
    }
}
