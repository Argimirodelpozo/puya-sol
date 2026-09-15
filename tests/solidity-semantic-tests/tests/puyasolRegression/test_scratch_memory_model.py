"""Memory reference identity under `--memory-model scratch` (experimental).

Every probe follows solc's pointer semantics: memory-to-memory assignment
copies the pointer, internal calls share the object, storage and ABI
crossings copy. The mixed model is run alongside to record its behaviour.
"""

import pytest

from framework import as_int

CONTRACT = "puyasolRegression/contracts/scratch_memory_model.sol"


def _ints(result):
    return tuple(as_int(item) for item in result)


@pytest.mark.parametrize("model", [
    pytest.param("mixed", marks=pytest.mark.xfail(
        reason="mixed model: `f(a, a)` is rejected by puya (mutable value passed twice) and "
               "memory-to-memory assignment copies instead of aliasing")),
    "scratch",
])
def test_memory_reference_identity(harness, model):
    app = harness.compile_and_deploy(
        CONTRACT, contract_name="ScratchMemoryModel",
        extra_args=["--memory-model", model])
    assert _ints(harness.call(app, "aliasThenRebind()").abi_return) == (7, 42, 9)
    assert as_int(harness.call(app, "repeatedArgument()").abi_return) == 11
    assert as_int(harness.call(app, "mutateThroughCall()").abi_return) == 5
    x, y, b = harness.call(app, "structAlias()").abi_return
    assert (as_int(x), as_int(y), b) == (2, 4, True)
    assert _ints(harness.call(app, "returnedReference()").abi_return) == (0, 3)
    assert _ints(harness.call(app, "storageCopies()").abi_return) == (1, 2)
    assert _ints(harness.call(app, "nestedShared()").abi_return) == (5, 2)
    assert list(map(as_int, harness.call(app, "echo(uint256[])", [4, 5]).abi_return)) == [5, 5]
    assert as_int(harness.call(app, "fill(uint256)", 6).abi_return) == 30
    assert _ints(harness.call(app, "fixedNarrow()").abi_return) == (201, 201)
    assert _ints(harness.call(app, "namedReturn()").abi_return) == (10, 3)
    assert _ints(harness.call(app, "nestedAssign()").abi_return) == (1, 6, 0)
    name, v = harness.call(app, "stringMember()").abi_return
    assert (name, as_int(v)) == ("hello", 5)
