contract RecurClear {
    struct S {
        S[] x;
        uint256 v;
    }
    S s;

    function seed() external {
        s.v = 7;
        s.x.push();
        s.x[0].v = 3;
        s.x[0].x.push();
        s.x[0].x[0].v = 9;
    }

    function wipe() external {
        delete s;
    }

    function read() external view returns (uint256, uint256, uint256) {
        return (s.v, s.x.length, s.x.length > 0 ? s.x[0].v : 0);
    }
}
