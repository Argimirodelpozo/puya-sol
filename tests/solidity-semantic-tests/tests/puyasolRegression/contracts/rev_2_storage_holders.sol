pragma solidity >=0.8.29;

library HolderWrites {
    function put(mapping(uint256 => uint256) storage values, uint256 key, uint256 value) internal {
        values[key] = value;
    }
}

contract StorageHolders {
    struct A { uint256 tag; mapping(uint256 => uint256) bc; }
    struct AB { uint256 tag; mapping(uint256 => uint256) c; }
    struct Pair { A a; AB ab; }
    A a;
    AB ab;
    mapping(uint256 => uint256) public abc;
    Pair root;
    mapping(uint256 => Pair) groups;
    A[2] fixedHolders;
    A[][] dynamicHolders;
    mapping(uint256 => uint256)[2] public fixedMaps;
    mapping(uint256 => uint256)[][] public dynamicMaps;
    mapping(string => uint256) public text;
    mapping(string => uint256) public btext;
    uint256 counter;

    function put(mapping(uint256 => uint256) storage values, uint256 value) internal {
        HolderWrites.put(values, 1, value);
    }

    function roots() public returns (bool) {
        a.tag = 7;
        ab.tag = 9;
        a.bc[1] = 11;
        ab.c[1] = 22;
        abc[1] = 33;
        require(a.bc[1] == 11 && ab.c[1] == 22 && abc[1] == 33);
        put(a.bc, 44);
        HolderWrites.put(ab.c, 1, 55);
        return a.bc[1] == 44 && ab.c[1] == 55 && abc[1] == 33 && a.tag == 7 && ab.tag == 9;
    }

    function nested() public returns (bool) {
        root.a.bc[1] = 101;
        root.ab.c[1] = 102;
        A storage first = root.a;
        AB storage second = root.ab;
        require(first.bc[1] == 101 && second.c[1] == 102);
        first.tag = 103;
        second.tag = 104;
        put(first.bc, 105);
        put(second.c, 106);
        return root.a.bc[1] == 105 && root.ab.c[1] == 106 && root.a.tag == 103 && root.ab.tag == 104;
    }

    function mappingValues() public returns (bool) {
        Pair storage first = groups[7];
        Pair storage second = groups[8];
        first.a.bc[1] = 201;
        first.ab.c[1] = 202;
        second.a.bc[1] = 203;
        first.a.tag = 204;
        A storage inner = first.a;
        put(inner.bc, 205);
        return groups[7].a.bc[1] == 205 && groups[7].ab.c[1] == 202
            && groups[8].a.bc[1] == 203 && groups[8].ab.c[1] == 0 && groups[7].a.tag == 204;
    }

    function updatePair(Pair storage pair, uint256 value) internal {
        pair.a.tag = value;
        put(pair.a.bc, value + 1);
        put(pair.ab.c, value + 2);
    }

    function references() public returns (bool) {
        updatePair(root, 601);
        updatePair(groups[11], 611);
        mapping(uint256 => uint256) storage local = root.a.bc;
        local[2] = 621;
        return root.a.tag == 601 && root.a.bc[1] == 602 && root.ab.c[1] == 603
            && groups[11].a.tag == 611 && groups[11].a.bc[1] == 612 && groups[11].ab.c[1] == 613
            && root.a.bc[2] == 621;
    }

    function arrays() public returns (bool) {
        fixedHolders[0].bc[1] = 301;
        fixedHolders[1].bc[1] = 302;
        A storage local = fixedHolders[1];
        put(local.bc, 303);
        dynamicHolders.push();
        dynamicHolders.push();
        dynamicHolders[0].push();
        dynamicHolders[1].push();
        dynamicHolders[0][0].bc[1] = 304;
        dynamicHolders[1][0].bc[1] = 305;
        put(dynamicHolders[1][0].bc, 306);
        return fixedHolders[0].bc[1] == 301 && fixedHolders[1].bc[1] == 303
            && dynamicHolders[0][0].bc[1] == 304 && dynamicHolders[1][0].bc[1] == 306;
    }

    function getters() public returns (bool) {
        fixedMaps[0][3] = 401;
        fixedMaps[1][3] = 402;
        dynamicMaps.push();
        dynamicMaps.push();
        dynamicMaps[0].push();
        dynamicMaps[1].push();
        dynamicMaps[0][0][3] = 403;
        dynamicMaps[1][0][3] = 404;
        text["xb"] = 405;
        btext["x"] = 406;
        return text["xb"] == 405 && btext["x"] == 406;
    }

    function next() internal returns (uint256) { ++counter; return 1; }
    function effectful() public returns (bool) {
        counter = 0;
        fixedHolders[1].bc[7] = 0;
        fixedHolders[next()].bc[7] += 1;
        require(counter == 1 && fixedHolders[1].bc[7] == 1);
        counter = 0;
        put(fixedHolders[next()].bc, 501);
        return counter == 1 && fixedHolders[1].bc[1] == 501;
    }

    function invalid(uint256 index) public view returns (uint256) {
        return dynamicHolders[index][0].bc[1];
    }
}

contract HolderOriginal {
    struct Original { uint128 tag; mapping(uint256 => uint256) values; }
    Original original;
    function read(uint256 key) public view returns (uint256) { return original.values[key]; }
    function write(uint256 key, uint256 value) public { original.values[key] = value; }
}

contract HolderRenamed {
    struct Renamed { uint128 renamedTag; mapping(uint256 => uint256) renamedValues; }
    Renamed renamed;
    function read(uint256 key) public view returns (uint256) { return renamed.renamedValues[key]; }
    function write(uint256 key, uint256 value) public { renamed.renamedValues[key] = value; }
}

contract HolderWide layout at (1 << 200) {
    mapping(uint256 => uint256) public values;
    function write(uint256 key, uint256 value) public { values[key] = value; }
}
