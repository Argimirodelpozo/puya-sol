// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// Guard: a signed intN (N<=64) local read inside inline assembly must present
// the sign-extended 256-bit word (a Yul identifier IS the full EVM word:
// int64 -1 = 0xFF..FF). The uint64-backed 64-bit-TC value used to be read
// zero-padded, so `ret := val` into a bytes2 UDVT took 0x0000 from the top
// instead of 0xFFFF for every negative input.
type MyBytes2 is bytes2;

contract C {
    // the fuzzer's shape: assembly-assign an int64 into a bytes2 UDVT
    function h(int64 val) external pure returns (MyBytes2) {
        MyBytes2 ret;
        assembly {
            ret := val
        }
        return ret;
    }

    // narrower width, plain bytes2 target
    function top(int32 val) external pure returns (bytes2 b) {
        assembly {
            b := val
        }
    }

    // full-word observation of the sign extension
    function asWord(int64 val) external pure returns (uint256 r) {
        assembly {
            r := val
        }
    }
}
