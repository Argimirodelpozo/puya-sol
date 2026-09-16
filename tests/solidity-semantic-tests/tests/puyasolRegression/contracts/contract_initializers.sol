pragma solidity ^0.8.20;

contract FixedInitializers {
    uint256[5] public xs = [uint256(11), 12, 13, 14, 15];
}

contract GlobalDefaultInitializers {
    uint256 public initialized = initialize();
    uint256[2] public entries;
    struct Pair { uint64 x; uint64 y; }
    Pair public pair;
    function initialize() internal returns (uint256) {
        entries[1] = 17;
        pair.x = 29;
        return entries[0] + pair.y;
    }
}

contract OrderedInitializers {
    uint256 public seed = 7;
    uint256[] public xs = [seed];
    uint256 public calls;
    bytes public b = initializeBytes();
    uint256 public afterBytes = calls;
    uint256 public written = initializeForward();
    uint256 public forward;
    function initializeForward() internal returns (uint256) {
        forward = 19;
        return 23;
    }
    function initializeBytes() internal returns (bytes memory) {
        calls++;
        return abi.encode(seed);
    }
}

contract InitializerBase {
    uint256 public seed = 7;
    uint256[] public beforeConstructor = [seed];
    constructor() { seed = 9; }
}

contract InitializerDerived is InitializerBase {
    uint256[] public derived = [seed];
    uint256[5] public fixedValues = [uint256(21), 22, 23, 24, 25];
}

contract BytesInitializers {
    bytes public encoded = abi.encode(uint256(7));
    bytes public literalValue = "abc";
    bytes public emptyValue = "";
    string public composed = string.concat("ab", "cd");
}
