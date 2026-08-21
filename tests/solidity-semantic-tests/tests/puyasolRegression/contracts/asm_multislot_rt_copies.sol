// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// CUSTOM puya-sol regression fixtures — runtime-offset memory ranges that
// CROSS the 4096-byte scratch-slot boundary, plus the runtime-out-offset
// returndata copy. All three previously routed through single-slot (or
// silently skipped) lowerings.
contract MultiSlotRtCopies {
    // A Yul low-level call is an expression, not merely a statement or the
    // direct RHS of a let/assignment. Solady uses this exact compact shape for
    // ecrecover: the success word (1) doubles as the mload offset while the
    // precompile writes its result at offset 1.
    function nestedIdentity(bytes32 x) external view returns (bytes32 y) {
        assembly {
            mstore(0x00, x)
            y := mload(staticcall(gas(), 4, 0x00, 0x20, 0x01, 0x20))
        }
    }

    // Identity precompile at runtime offsets with src and dst in DIFFERENT
    // scratch slots. Old lowering extract3/replace3'd slot 0 only.
    function identityCross(uint256 srcOff, uint256 dstOff)
        external
        view
        returns (bytes32 a, bytes32 b)
    {
        assembly {
            mstore(srcOff, 0x1111111111111111111111111111111111111111111111111111111111111111)
            mstore(add(srcOff, 0x20), 0x2222222222222222222222222222222222222222222222222222222222222222)
            pop(staticcall(gas(), 4, srcOff, 0x40, dstOff, 0x40))
            a := mload(dstOff)
            b := mload(add(dstOff, 0x20))
        }
    }

    // SHA-256 precompile with INPUT above the first slot boundary.
    function shaCross(uint256 off) external view returns (bytes32 h) {
        assembly {
            mstore(off, 0x4141414141414141414141414141414141414141414141414141414141414141)
            pop(staticcall(gas(), 2, off, 0x20, 0x00, 0x20))
            h := mload(0x00)
        }
    }

    // Dynamic-length mcopy across the slot boundary (shared range helpers).
    function mcopyCross(uint256 srcOff, uint256 dstOff, uint256 len)
        external
        pure
        returns (bytes32 w)
    {
        assembly {
            mstore(srcOff, 0x3333333333333333333333333333333333333333333333333333333333333333)
            mcopy(dstOff, srcOff, len)
            w := mload(dstOff)
        }
    }
}

contract RtOutCallee {
    function ping(uint256 x) external pure returns (uint256) {
        return x + 1000;
    }
}

contract RtOutCaller {
    // Same shape as asm_pop_call_output but the OUT offset arrives at RUNTIME
    // (a parameter can never constant-fold): the returndata copy was silently
    // skipped for non-constant offsets, so `r` read stale request bytes.
    function h(uint256 x, uint256 po) external returns (uint256 r) {
        RtOutCallee c = new RtOutCallee();
        address t = address(c);
        uint32 sel = uint32(RtOutCallee.ping.selector);
        assembly {
            mstore(0x00, sel)
            mstore(0x20, x)
            pop(call(gas(), t, 0, 0x1c, 0x24, po, 0x20))
            r := mload(po)
        }
    }
}
