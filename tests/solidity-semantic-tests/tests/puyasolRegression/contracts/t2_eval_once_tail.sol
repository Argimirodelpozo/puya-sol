// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// CUSTOM regression fixture (NOT vendored / not an original Solidity semantic test).
// Guards the T2 EvalOnce tail (fable-review-3): call-valued operands that the
// builders reference more than once must evaluate exactly once —
// encodePacked fixed-array element loop, address-compare stored side,
// write-path array index, precompile staticcall input.
contract T2EvalOnce {
    uint256 public cnt;
    uint256[3] arr;

    function bumpU(uint256 v) internal returns (uint256) {
        cnt += 1;
        return v;
    }

    function bumpArr() internal returns (uint64[3] memory a) {
        cnt += 1;
        a[0] = 1; a[1] = 2; a[2] = 3;
    }

    function bumpAddr() internal returns (address) {
        cnt += 1;
        return msg.sender;
    }

    function bumpB128() internal returns (bytes memory b) {
        cnt += 1;
        b = new bytes(128);
    }

    function packedArr() external returns (uint256 c, bytes32 h) {
        cnt = 0;
        h = keccak256(abi.encodePacked(bumpArr()));
        c = cnt;
    }

    function addrCmp() external returns (uint256 c, bool eq) {
        cnt = 0;
        eq = (bumpAddr() == msg.sender);
        c = cnt;
    }

    function writeIdx() external returns (uint256 c, uint256 v) {
        delete arr;
        cnt = 0;
        arr[bumpU(1)] += 5;
        c = cnt;
        v = arr[1];
    }

    function ecInput() external returns (uint256 c, bool ok) {
        cnt = 0;
        // ecAdd of two zero points (point at infinity) — valid input, zero output.
        (ok, ) = address(6).staticcall(bumpB128());
        c = cnt;
    }
}
