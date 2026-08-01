// add2: prove knowledge of x, y whose (public) sum is the public return value.
// 2 public inputs (x, y) + 1 public return = 3 public values, matching the
// upstream barretenberg `add2` instance (reference/Add2.t.sol: PUBLIC_INPUT_COUNT = 3).
fn main(x: pub Field, y: pub Field) -> pub Field {
    x + y
}
