// puya-sol regression: real Yul call boundaries, shared buffers and scoped leave.
pragma solidity ^0.8.24;

contract YulSubroutines {
    uint256 public stored;

    function mutual(uint256 n) external pure returns (uint256 a, uint256 b) {
        assembly {
            function even(x) -> r {
                if iszero(x) { r := 1 leave }
                r := odd(sub(x, 1))
            }
            function odd(x) -> r {
                if iszero(x) { leave }
                r := even(sub(x, 1))
            }
            a := even(n)
            b := odd(n)
        }
    }

    function nestedLeave(uint256 n) external pure returns (uint256 a, uint256 b) {
        assembly {
            function run(limit) -> sum, count {
                for { let i := 0 } lt(i, 4) { i := add(i, 1) } {
                    for { let j := 0 } lt(j, 3) { j := add(j, 1) } {
                        if eq(count, limit) { leave }
                        sum := add(sum, add(mul(i, 10), j))
                        count := add(count, 1)
                    }
                }
            }
            a, b := run(n)
        }
    }

    function switchLeave(uint256 n) external pure returns (uint256 a, uint256 b) {
        assembly {
            function pick(x) -> first, second {
                switch x
                case 0 { first := 7 leave }
                case 1 { second := 9 leave }
                default { first := 11 second := 13 }
                second := add(second, 100)
            }
            a, b := pick(n)
        }
    }

    function memoryShared(uint256 offset, uint256 value)
        external pure returns (uint256 a, uint256 b, uint256 c)
    {
        assembly {
            function write(p, v) { mstore(p, v) }
            function read(p) -> r { r := mload(p) }
            function allocate(v) -> p {
                p := mload(0x40)
                mstore(0x40, add(p, 32))
                write(p, v)
            }
            let p := allocate(value)
            a := read(p)
            write(offset, add(value, 1))
            b := mload(offset)
            c := sub(mload(0x40), p)
        }
    }

    function order() external pure returns (uint256 a, uint256 b) {
        assembly {
            function bump(v) -> r {
                r := add(mload(0), v)
                mstore(0, r)
            }
            function pair(x, y) -> r { r := add(mul(x, 100), y) }
            mstore(0, 0)
            a := pair(bump(1), bump(10))
            b := mload(0)
        }
    }

    function memoryArgument(uint256[] memory values)
        external pure returns (uint256 before, uint256 afterValue, uint256 size)
    {
        assembly {
            function read(p) -> r { r := mload(add(p, 32)) }
            function write(p, v) { mstore(add(p, 32), v) }
            before := read(values)
            write(values, add(before, 1))
            size := mload(values)
        }
        afterValue = values[0];
    }

    function memoryFixed(uint256[2] memory values)
        external pure returns (uint256 before, uint256 afterValue, uint256 other)
    {
        assembly {
            function change(p) -> old { old := mload(p) mstore(p, add(old, 10)) }
            before := change(values)
        }
        afterValue = values[0];
        other = values[1];
    }

    function memoryBytes(bytes memory data) external pure returns (uint256 word, bytes memory result) {
        assembly {
            function change(p) -> old {
                old := mload(add(p, 32))
                mstore8(add(p, 32), 0xa5)
            }
            word := change(data)
        }
        result = data;
    }

    function calldataShared(bytes calldata data, uint256 offset)
        external pure returns (uint256 word, uint256 size, uint256 copied)
    {
        assembly {
            function load(p) -> r { r := calldataload(p) }
            function forward(p) -> r, n { r := load(p) n := calldatasize() }
            function copy(p) -> r {
                calldatacopy(0, p, 32)
                r := mload(0)
            }
            word, size := forward(add(data.offset, offset))
            copied := copy(add(data.offset, offset))
        }
    }

    function captureValues() external pure returns (uint256 a, uint256 b, uint256 c, uint256 d) {
        assembly {
            function bump(v) -> r { r := add(mload(0), v) mstore(0, r) }
            function pair(x, y) -> first, second { first := x second := y }
            function pack(x, y) -> r { r := add(mul(x, 100), y) }
            function sink(x, y) { mstore(32, pack(x, y)) }
            mstore(0, 0)
            let x, y := pair(bump(1), mload(0))
            a := pack(x, y)
            x, y := pair(bump(1), mload(0))
            b := pack(x, y)
            c := pack(bump(1), mload(0))
            sink(bump(1), mload(0))
            d := mload(32)
        }
    }

    function staticCalldata(uint256 value) external pure returns (uint256 out) {
        assembly {
            function load() -> r { r := calldataload(4) }
            out := load()
        }
    }

    function writeStorage(uint256 value, bool fail) external returns (uint256 out) {
        assembly {
            function write(v, bad) {
                sstore(0, v)
                if bad { revert(0, 0) }
            }
            function read() -> r { r := sload(0) }
            write(value, fail)
            out := read()
        }
    }

    function evmReturn(uint256 value) external pure returns (uint256) {
        assembly {
            function finish(v) { mstore(0, v) return(0, 32) }
            function forward(v) { finish(v) }
            forward(value)
        }
    }
}
