// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

function increment(uint64 x) pure returns (uint64) { return x + 1; }
function permitted(uint64 x) pure returns (bool) { return x < 100; }

type CallableWord is uint64;
using {addWords as +} for CallableWord global;

function addWords(CallableWord x, CallableWord y) pure returns (CallableWord) {
    require(permitted(CallableWord.unwrap(x)));
    return CallableWord.wrap(CallableWord.unwrap(x) + CallableWord.unwrap(y));
}

library CallableLeaf {
    modifier checked(uint64 x) { require(permitted(x)); _; }
    function leaf(uint64 x, function(uint64) pure returns (uint64) callback)
        internal pure checked(x) returns (uint64)
    {
        return callback(x);
    }
}

library CallableRelay {
    function relay(uint64 x) internal pure returns (uint64) {
        return CallableLeaf.leaf(x, increment);
    }

    function operators(uint64 x) internal pure returns (uint64) {
        return CallableWord.unwrap(CallableWord.wrap(x) + CallableWord.wrap(1));
    }
}

function throughFree(uint64 x) pure returns (uint64) { return CallableRelay.relay(x); }

contract CallableHost {
    function run(uint64 x) external pure returns (uint64) { return throughFree(x); }
    function runOperators(uint64 x) external pure returns (uint64) {
        return CallableRelay.operators(x);
    }
}

contract UnrelatedHost {
    function run(uint64 x) external pure returns (uint64) { return x; }
}
