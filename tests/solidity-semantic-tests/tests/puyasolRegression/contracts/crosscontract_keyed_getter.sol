// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// CUSTOM regression fixture (NOT vendored / not an original Solidity semantic test).
// Guards cross-contract KEYED public getter calls: the callee published
// m(<declared-width-key>)T while the caller emitted the return-only selector
// m()byte[] (and encoded biguint keys at the 32-byte backing width where the
// callee decoded the declared width) — every keyed getter call reverted with
// a router selector mismatch. Param-less getters were already fixed (night-2).
contract Store {
    mapping(uint256 => uint256) public m;
    mapping(uint128 => uint256) public mNarrow;
    uint256[3] public arr;
    uint256 public plain = 77;

    function seed() external {
        m[5] = 500;
        mNarrow[9] = 900;
        arr[1] = 22;
    }
}

contract Reader {
    function readMap(address a, uint256 k) external returns (uint256) {
        return Store(a).m(k);
    }

    function readNarrow(address a, uint128 k) external returns (uint256) {
        return Store(a).mNarrow(k);
    }

    function readArr(address a, uint256 i) external returns (uint256) {
        return Store(a).arr(i);
    }

    function readPlain(address a) external returns (uint256) {
        return Store(a).plain();
    }
}
