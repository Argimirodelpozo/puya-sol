pragma solidity ^0.8.24;

library YulLibrary {
    function twice(uint256 x) internal pure returns (uint256 r) {
        assembly {
            function double(v) -> out { out := mul(v, 2) }
            r := double(x)
        }
    }
    function write(uint256 x) internal {
        assembly {
            function store(v) { sstore(0, v) }
            store(x)
        }
    }
}

function increment(uint256 x) pure returns (uint256 r) {
    assembly {
        function next(v) -> out { out := add(v, 1) }
        r := next(x)
    }
}

contract FirstYulHost {
    uint256 public first;
    function f(uint256 x) external pure returns (uint256) {
        return increment(YulLibrary.twice(x));
    }
    function write(uint256 x) external { YulLibrary.write(x); }
}

contract SecondYulHost {
    uint256 public second;
    function f(uint256 x) external pure returns (uint256) {
        return YulLibrary.twice(increment(x));
    }
    function write(uint256 x) external { YulLibrary.write(x); }
}
