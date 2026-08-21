// Regression coverage for the remaining builder-review storage seams.
contract BuilderReviewRemaining {
    mapping(int24 => int24) public signedMap;

    function setSigned(int24 key, int24 value) external {
        signedMap[key] = value;
    }

    function writeSparseSlots() external {
        assembly {
            sstore(1, 11)
            sstore(257, 22)
        }
    }

    function readSparseSlots() external view returns (uint256, uint256) {
        uint256 a;
        uint256 b;
        assembly {
            a := sload(1)
            b := sload(257)
        }
        return (a, b);
    }

    function writeWrappedSlot() external {
        assembly {
            // add(2^256-1, 2) == 1 modulo 2^256.
            sstore(add(not(0), 2), 33)
        }
    }
}
