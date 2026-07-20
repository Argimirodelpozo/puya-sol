// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// CUSTOM regression fixture (NOT vendored / not an original Solidity semantic test).
// Guards super/Base.f() impl emission: the impl copy was built as an ABI method
// (config reset only AFTERWARDS), baking the base's not-payable group assert,
// ABI entry checks, and wire-return encoding into the direct-callsub body — a
// payable caller invoking Base.f() falsely reverted whenever the outer call
// was grouped with a payment. The impl now builds as a plain internal
// subroutine (native args/returns, no entry semantics).
contract A {
    uint256 public total;

    function f(uint256 x) public virtual returns (uint256) {
        total += x;
        return total;
    }
}

contract B is A {
    function f(uint256 x) public override returns (uint256) {
        total += 2 * x;
        return total;
    }

    // payable + explicit Base.f(): must NOT inherit A.f's not-payable assert
    function g(uint256 x) public payable returns (uint256) {
        return A.f(x);
    }

    // super chain form
    function h(uint256 x) public payable returns (uint256) {
        return super.f(x);
    }
}
