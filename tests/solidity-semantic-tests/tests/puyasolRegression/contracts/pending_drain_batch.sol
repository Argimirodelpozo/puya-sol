// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

// CUSTOM regression fixture (NOT vendored / not an original Solidity semantic test).
// Guards the pending-statement drain cluster (fable-review-3 T1):
// (H1) effects of evaluating an if-CONDITION (internal-call write-backs) were
//      emitted AFTER the IfElse — invisible to the branches and LOST entirely
//      when a branch returned;
// (H2) emit never drained the shared buffers — arg-build pre-statements
//      (scoped-ternary temp assignments) leaked into the NEXT statement and
//      the emitted value read an unassigned temp;
// (H3) do-while condition pendings drained into the TOP of the body while the
//      test ran at the BOTTOM, one iteration apart;
// (H5) a trailing bare calldatacopy in asm queued its memory write into a
//      buffer the block never drained — the copy vanished.
contract PendingDrainBatch {
    event Ev(uint256 v);

    uint256 public cnt;
    uint256[] s;

    function bump(uint256[] memory a) internal pure returns (uint256) {
        a[0]++;
        return a[0];
    }

    // H1: the write-back to arr must happen BEFORE the branch runs.
    function condWriteback() external pure returns (uint256) {
        uint256[] memory arr = new uint256[](1);
        if (bump(arr) > 0) {
            return arr[0]; // pre-fix: read 0 (write-back emitted after the if, then dead)
        }
        return 999;
    }

    function g() internal returns (uint256) {
        cnt++;
        return 41;
    }

    // H2: ternary arg → scoped pre-statements assign a __cond temp; emit must
    // drain them before the log statement.
    function emitTernary(bool c) external returns (uint256) {
        cnt = 0;
        emit Ev(c ? 7 : g() + 1);
        return cnt;
    }

    // H3: the condition's bounds-assert/index temps must run WITH the
    // bottom-of-body test, not at the top of the body.
    function doWhileStorage() external returns (uint256 out) {
        delete s;
        s.push(1);
        s.push(2);
        s.push(0);
        uint256 i = 0;
        do {
            out += 10;
            i++;
        } while (s[i] != 0);
        // iterations: i=1 (s[1]=2 → continue), i=2 (s[2]=0 → stop) → 20
    }

    // H5: the trailing calldatacopy's memory write must not vanish. The
    // bytes-calldata param + b.offset read activate the __cd_blob transport
    // (the mode whose calldatacopy handler queues the write).
    function trailingCdc(uint256 x, bytes calldata b) external pure returns (uint256 r) {
        assembly {
            let q := calldataload(sub(b.offset, 32))
            calldatacopy(0x80, 4, 32) // trailing: copies arg0 (= x) to 0x80
        }
        assembly {
            r := mload(0x80)
        }
        x;
    }
}
