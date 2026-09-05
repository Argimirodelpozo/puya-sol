pragma solidity ^0.8.24;

contract YulScopedFacts {
    function siblings(uint64 input) external pure returns (uint64 first, uint64 second) {
        assembly {
            {
                function value(x) -> result { let offset := 11 result := add(x, offset) }
                first := value(input)
            }
            {
                function value(x) -> result { let offset := 22 result := add(x, offset) }
                second := value(input)
            }
        }
    }

    function locals(uint64 input) external pure returns (uint64 first, uint64 second) {
        assembly {
            { let value := input let delta := 7 first := add(value, delta) }
            { let value := add(input, 1) let delta := 9 second := add(value, delta) }
        }
    }

    function recursive(uint64 input) external pure returns (uint64 first, uint64 second) {
        assembly {
            {
                function sum(n) -> result {
                    if n { result := add(n, sum(sub(n, 1))) }
                }
                first := sum(input)
            }
            {
                function sum(n) -> result {
                    if n { result := add(mul(n, 2), sum(sub(n, 1))) }
                }
                second := sum(input)
            }
        }
    }
}
