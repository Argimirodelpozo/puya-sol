// Audit of resolveBlobOffset's arms, after the dynamic-array element offset
// was found to skip the length word (silent corruption).
//
// Every probe has a NON-BLOB control with identical logic: the control tells
// us the expected values without trusting the blob path. A probe is marked
// blob-backed by an `assembly { mstore(local, n) }` on the LOCAL (a named
// return is not blob-registered).
contract Audit {
    struct S { uint256 a; uint256 b; }

    // ── A: struct members inside a dynamic array element (MemberAccess over
    //       IndexAccess — the composed arm) ─────────────────────────────────
    function structInDynBlob() public pure returns (uint256[] memory) {
        S[] memory arr = new S[](2);
        arr[0].a = 11; arr[0].b = 22;
        arr[1].a = 33; arr[1].b = 44;
        assembly { mstore(arr, 2) }
        uint256[] memory out = new uint256[](4);
        out[0] = arr[0].a; out[1] = arr[0].b;
        out[2] = arr[1].a; out[3] = arr[1].b;
        return out;
    }
    function structInDynPlain() public pure returns (uint256[] memory) {
        S[] memory arr = new S[](2);
        arr[0].a = 11; arr[0].b = 22;
        arr[1].a = 33; arr[1].b = 44;
        uint256[] memory out = new uint256[](4);
        out[0] = arr[0].a; out[1] = arr[0].b;
        out[2] = arr[1].a; out[3] = arr[1].b;
        return out;
    }

    // ── B: FIXED-size array (no length word — must NOT gain the +32) ───────
    function fixedArrBlob() public pure returns (uint256[] memory) {
        uint256[3] memory fx;
        fx[0] = 11; fx[1] = 22; fx[2] = 33;
        uint256[] memory out = new uint256[](3);
        out[0] = fx[0]; out[1] = fx[1]; out[2] = fx[2];
        assembly { mstore(out, 3) }
        return out;
    }

    // ── C: struct with a fixed array member, blob-backed ───────────────────
    function structFieldsBlob() public pure returns (uint256[] memory) {
        S memory s;
        s.a = 77; s.b = 88;
        uint256[] memory out = new uint256[](2);
        out[0] = s.a; out[1] = s.b;
        assembly { mstore(out, 2) }
        return out;
    }

    // ── D: writes at a RUNTIME index into a blob dynamic array, then read
    //       back at a runtime index (both directions through the offset) ────
    function runtimeIdxBlob(uint256 n) public pure returns (uint256[] memory) {
        uint256[] memory buf = new uint256[](n);
        for (uint256 i = 0; i < n; i++) { buf[i] = (i + 1) * 100; }
        assembly { mstore(buf, n) }
        uint256[] memory out = new uint256[](n);
        for (uint256 j = 0; j < n; j++) { out[j] = buf[j]; }
        return out;
    }
    function runtimeIdxPlain(uint256 n) public pure returns (uint256[] memory) {
        uint256[] memory buf = new uint256[](n);
        for (uint256 i = 0; i < n; i++) { buf[i] = (i + 1) * 100; }
        uint256[] memory out = new uint256[](n);
        for (uint256 j = 0; j < n; j++) { out[j] = buf[j]; }
        return out;
    }

    // ── E: the LAST element specifically (the one the old bug never wrote) ─
    function lastElemBlob() public pure returns (uint256) {
        uint256[] memory buf = new uint256[](4);
        for (uint256 i = 0; i < 4; i++) { buf[i] = i + 1; }
        assembly { mstore(buf, 4) }
        return buf[3];
    }
}
