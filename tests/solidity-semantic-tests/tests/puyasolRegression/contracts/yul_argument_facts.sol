pragma solidity ^0.8.24;

contract YulArgumentFacts {
    function aligned(uint256 value) external pure returns (uint256 result) {
        assembly {
            function read_aligned(p) -> r { r := mload(p) }
            function forward_aligned(p) -> r { r := read_aligned(add(p, 32)) }
            let p := add(512, mul(and(value, 31), 32))
            mstore(add(p, 32), value)
            result := forward_aligned(p)
        }
    }

    function constantOffset(uint256 value) external pure returns (uint256 result) {
        assembly {
            function read_constant(p) -> r { r := mload(p) }
            function forward_constant(p) -> r { r := read_constant(add(p, 32)) }
            mstore(544, value)
            result := forward_constant(512)
        }
    }

    function sameResidue() external pure returns (uint256 first, uint256 second) {
        assembly {
            function read_same_residue(p) -> r { r := mload(p) }
            mstore(512, 11)
            mstore(544, 22)
            first := read_same_residue(512)
            second := read_same_residue(544)
        }
    }

    function mixed() external pure returns (uint256 first, uint256 second) {
        assembly {
            function read_mixed(p) -> r { r := mload(p) }
            mstore(512, 11)
            mstore(4095, 22)
            first := read_mixed(512)
            second := read_mixed(4095)
        }
    }

    function reassigned(uint256 value) external pure returns (uint256 result, uint256 ptr) {
        assembly {
            function read_reassigned(p, v) -> r, q {
                p := add(p, 31)
                mstore(p, v)
                r := mload(p)
                q := p
            }
            result, ptr := read_reassigned(4064, value)
        }
    }

    function snapshot(uint256 value) external pure returns (uint256 result, uint256 delta) {
        assembly {
            function read_snapshot(p) -> r { r := mload(p) }
            let p := 4095
            let saved := p
            p := 512
            let d := sub(saved, p)
            delta := d
            mstore(d, value)
            result := read_snapshot(d)
        }
    }

    function recursive(uint256 n, uint256 value) external pure returns (uint256 result) {
        assembly {
            function read_recursive(p, count) -> r {
                if count { r := read_recursive(add(p, 1), sub(count, 1)) leave }
                r := mload(p)
            }
            mstore(add(4094, n), value)
            result := read_recursive(4094, n)
        }
    }

    function memoryPointer(uint256 p, uint256 value) external pure returns (uint256 result) {
        assembly {
            function read_memory_pointer(q) -> r { r := mload(q) }
            mstore(0x40, p)
            let ptr := mload(0x40)
            mstore(ptr, value)
            result := read_memory_pointer(ptr)
        }
    }
}
