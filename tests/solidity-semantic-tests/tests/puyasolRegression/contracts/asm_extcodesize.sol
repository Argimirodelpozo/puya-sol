// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

// `extcodesize` in inline assembly is how OpenZeppelin's Address.isContract is
// written, so it is vendored into a large share of real contracts. It used to
// be a hard error even though `address(x).code.length` — the same question —
// was supported; both now share a non-materialising app-metadata lowering.

contract Probe {
    uint256 public x;

    fallback() external {}
}

contract AsmExtcodesize {
    Probe public probe;

    constructor() {
        probe = new Probe();
    }

    function isContract(address account) internal view returns (bool) {
        uint256 size;
        assembly {
            size := extcodesize(account)
        }
        return size > 0;
    }

    /// A deployed contract must report code, an EOA-shaped address must not.
    function contractHasCode() external view returns (bool) {
        return isContract(address(probe));
    }

    function eoaHasNoCode(address account) external view returns (bool) {
        return isContract(account);
    }

    /// The assembly spelling and the high-level one must agree.
    function agreesWithDotCode() external view returns (bool) {
        uint256 size;
        address a = address(probe);
        assembly {
            size := extcodesize(a)
        }
        return size == a.code.length;
    }

    /// Two reads in one expression must not share a temp.
    function twoReads() external view returns (uint256) {
        address a = address(probe);
        address b = address(this);
        uint256 s1;
        uint256 s2;
        assembly {
            s1 := extcodesize(a)
            s2 := extcodesize(b)
        }
        return s1 + s2;
    }

    /// Match SafeERC20's nesting: extcodesize shares an expression with the
    /// returndata opcodes immediately after a low-level call.
    function nestedAfterCall() external returns (bool success) {
        address a = address(probe);
        assembly {
            success := call(gas(), a, 0, 0, 0, 0, 32)
            success := and(success, and(iszero(returndatasize()), gt(extcodesize(a), 0)))
        }
    }
}
