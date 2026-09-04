"""Focused guards for the completed review and recursive-shape audit."""

from framework import as_int


def _ints(values):
    return tuple(as_int(value) for value in values)


def test_nested_dynamic_synthetic_calldata(harness):
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/new_review_nested_calldata.sol")

    header = harness.call(
        app, "inspectHeader(byte[][])", [b"abc", b"xy"]).abi_return
    assert _ints(header) == (68, 2, 64, 128, 260)

    result = harness.call(
        app, "inspectBytes(byte[][])", [b"abc", b"xy"]).abi_return
    assert _ints(result[:2]) == (2, 3)
    assert bytes(result[2]).startswith(b"abc")
    assert as_int(result[3]) == 2
    assert bytes(result[4]).startswith(b"xy")

    result = harness.call(
        app, "inspectNested(uint256[][])", [[11, 12], [99]]).abi_return
    assert _ints(result) == (2, 2, 11, 12, 1, 99)

    result = harness.call(
        app, "inspectTriple(uint256[][][])",
        [[[5, 6], [7]], [[8]]]).abi_return
    assert _ints(result) == (2, 2, 2, 5, 6)

    result = harness.call(
        app, "inspectFixedDynamic(uint256[][2])",
        [[11, 12], [99]]).abi_return
    assert _ints(result) == (36, 2, 64, 160, 11, 99)

    result = harness.call(
        app, "inspectStruct((uint256,byte[],uint256[]))",
        (7, b"abc", [11, 12])).abi_return
    assert _ints(result) == (36, 7, 3, 2, 11)


def test_expression_and_control_guards(harness):
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/new_review_core.sol")

    assert as_int(harness.call(app, "modifierReturn()").abi_return) == 0
    harness.call(app, "setGate(bool)", True)
    assert as_int(harness.call(app, "modifierReturn()").abi_return) == 5
    assert _ints(harness.call(app, "selfPair(uint256)", 10).abi_return) == (11, 12)
    assert _ints(harness.call(app, "convertedSelfPair(uint256)", 20).abi_return) == (21, 22)
    assert _ints(harness.call(app, "selectorEffect()", extra_fee=3_000).abi_return) == (1, 17)
    assert as_int(harness.call(app, "leaveLoop(uint256)", 1).abi_return) == 11
    assert as_int(harness.call(app, "leaveLoop(uint256)", 9).abi_return) == 77

    sliced = harness.call(app, "sliceOnce(byte[])", b"abcd").abi_return
    assert bytes(sliced[0]) == b"bc"
    assert as_int(sliced[1]) == 2
    chained = harness.call(
        app, "chainedByte(byte[],byte[],byte)", b"aa", b"bb", b"Z").abi_return
    assert tuple(bytes(v) for v in chained) == (b"Z", b"Z")

    assert as_int(harness.call(
        app, "decodeSmall(byte[])", (1).to_bytes(32, "big")).abi_return) == 1
    assert harness.call(
        app, "decodeSmall(byte[])", (2).to_bytes(32, "big"),
        expect_revert=True).reverted


def test_storage_identity_and_slot_lowering(harness):
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/new_review_storage.sol",
        extra_args=["--evm-storage-layout"], fund_wei=5_000_000)

    assert _ints(harness.call(app, "transientPair()").abi_return) == (11, 22)
    assert _ints(harness.call(app, "recordPair(uint256)", 7).abi_return) == (101, 202)
    sender = harness.localnet.account.address
    owner = harness.call(app, "assignOwner(address)", sender).abi_return
    assert as_int(owner[0]) == 1
    assert str(owner[1]) == sender

    harness.call(app, "seedArrays()")
    assert _ints(harness.call(app, "packedPostIncrement()").abi_return) == (7, 1)
    assert _ints(harness.call(app, "derivedRead(uint256)", 1).abi_return) == (6, 1)
    assert as_int(harness.call(app, "conditionalRef(uint256)", 0).abi_return) == 33
    assert as_int(harness.call(app, "rawDynamicLength()").abi_return) == 3
    harness.call(app, "resizeDynamic(uint256)", 1)
    assert as_int(harness.call(app, "dynamicLength()").abi_return) == 1
    assert as_int(harness.call(app, "signedKeyParity()").abi_return) == 77


def test_base_constructor_postinit_triggers(harness):
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/new_review_constructor.sol",
        contract_name="NewReviewConstructor",
        extra_args=["--evm-storage-layout"],
        fund_wei=2_000_000)
    assert as_int(harness.call(
        app, "childAnswer()", extra_fee=5_000).abi_return) == 42
    assert as_int(harness.call(app, "rawLibraryValue()").abi_return) == 91


def test_file_level_type_identity(harness):
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/new_review_type_identity.sol")
    result = harness.call(app, "layouts(uint256,uint256,bool)", 4, 9, True)
    assert _ints(result.abi_return) == (4, 9, 1)


def test_freestanding_selector_context(harness):
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/new_review_selector_context.sol",
        extra_args=["--evm-selectors"])
    assert tuple(harness.call(app, "probe()").abi_return) == (True, True)


def test_recursive_multibox_struct_guard(harness):
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/recursive_shape_audit.sol",
        contract_name="RecursiveShapeStorage", fund_wei=15_000_000,
        postinit_budget_pool=8)
    harness.call(app, "write(uint256,uint256,uint256)", 0, 11, 12)
    harness.call(app, "writeFields(uint256,uint256,uint256)", 512, 21, 22)
    assert _ints(harness.call(app, "read(uint256)", 0).abi_return) == (11, 12)
    assert _ints(harness.call(app, "read(uint256)", 512).abi_return) == (22, 22)


def test_recursive_shape_audit_guards(harness):
    evm = harness.compile_and_deploy(
        "puyasolRegression/contracts/recursive_shape_audit.sol",
        contract_name="RecursiveShapeEvm",
        extra_args=["--evm-storage-layout"],
        fund_wei=2_000_000)
    harness.call(evm, "replace(uint256[][][])",
                 [[[5, 6], [7]], [[8]]])
    assert _ints(harness.call(evm, "summary()").abi_return) == (2, 2, 2, 5, 6, 8)
    harness.call(evm, "setHolder(uint256,bool,uint256)", 19, True, 23)
    nested = harness.call(evm, "holder()").abi_return
    assert _ints(nested[0]) == (19, 1)
    assert as_int(nested[1]) == 23
    harness.call(evm, "replaceMixed(uint256[][2][2])",
                 [[[1], [2]], [[3], [4, 5]]])
    assert _ints(harness.call(evm, "mixedSummary()").abi_return) == (1, 2, 3, 5)
    harness.call(evm, "replaceFixedLeaf(uint256[2][2][])",
                 [[[1, 2], [3, 4]], [[5, 6], [7, 8]]])
    assert _ints(harness.call(evm, "fixedLeafSummary()").abi_return) == (
        2, 1, 4, 6, 7)
    harness.call(evm, "replaceMixedTree(uint256[][2][])",
                 [[[9], [10, 11]], [[12, 13], [14]]])
    assert _ints(harness.call(evm, "mixedTreeSummary()").abi_return) == (
        2, 9, 11, 13, 14)
    harness.call(evm, "replacePackedLeaf(bool[3][2][])",
                 [[[True, False, True], [False, True, False]]])
    assert _ints(harness.call(evm, "packedLeafSummary()").abi_return) == (
        1, 1, 0, 1, 0, 1, 0)
    harness.call(evm, "replaceHolders(((uint256,bool),uint256)[2])",
                 [((11, True), 12), ((21, False), 22)])
    assert _ints(harness.call(evm, "holderSummary()").abi_return) == (
        11, 1, 12, 21, 0, 22)
    harness.call(evm, "pushComplex(uint256[],uint256,bool,uint256)",
                 [31, 32], 33, True, 34)
    assert _ints(harness.call(evm, "complexSummary()").abi_return) == (
        1, 2, 33, 1, 34)
    harness.call(evm, "popComplex()")
    harness.call(evm, "pushComplex(uint256[],uint256,bool,uint256)",
                 [41], 42, False, 43)
    assert _ints(harness.call(evm, "complexSummary()").abi_return) == (
        1, 1, 42, 0, 43)
    harness.call(evm, "replaceComplex((uint256[],((uint256,bool),uint256))[])",
                 [([51, 52], ((53, True), 54)),
                  ([61], ((62, False), 63))])
    assert _ints(harness.call(evm, "complexSummary()").abi_return) == (
        2, 1, 62, 0, 63)
    harness.call(evm, "clearAggregates()")
    assert _ints(harness.call(evm, "clearedSummary()").abi_return) == (
        0, 0, 0, 0, 0, 0, 0, 0)

    recursive = harness.compile_and_deploy(
        "puyasolRegression/contracts/recursive_shape_audit.sol",
        contract_name="RecursiveShapeType", fund_wei=1_000_000)
    harness.call(recursive, "setRoot(uint256)", 77)
    assert as_int(harness.call(recursive, "getRoot()").abi_return) == 77

    mappings = harness.compile_and_deploy(
        "puyasolRegression/contracts/recursive_shape_audit.sol",
        contract_name="RecursiveShapeMapping", fund_wei=2_000_000)
    harness.call(mappings, "seed()")
    harness.call(mappings, "write(uint256,uint256,uint256,uint256)",
                 0, 0, 7, 101)
    harness.call(mappings, "write(uint256,uint256,uint256,uint256)",
                 0, 1, 7, 202)
    harness.call(mappings, "setMarker(uint256,uint256,uint256)", 0, 1, 303)
    assert as_int(harness.call(
        mappings, "read(uint256,uint256,uint256)", 0, 0, 7).abi_return) == 101
    assert as_int(harness.call(
        mappings, "read(uint256,uint256,uint256)", 0, 1, 7).abi_return) == 202
    assert as_int(harness.call(
        mappings, "rows(uint256,uint256)", 0, 1).abi_return) == 303

    slot_handles = harness.compile_and_deploy(
        "puyasolRegression/contracts/recursive_shape_audit.sol",
        contract_name="RecursiveShapeSlotHandle", fund_wei=2_000_000)
    harness.call(slot_handles, "write(uint256,uint256,uint256,uint256,bool)",
                 1, 0, 1, 301, True)
    harness.call(slot_handles, "write(uint256,uint256,uint256,uint256,bool)",
                 1, 1, 1, 311, False)
    assert _ints(harness.call(
        slot_handles, "read(uint256,uint256,uint256)", 1, 0, 1).abi_return) == (
            301, 1)
    assert _ints(harness.call(slot_handles, "copied(uint256)", 1).abi_return) == (
        0, 0, 311, 0)

    box_refs = harness.compile_and_deploy(
        "puyasolRegression/contracts/recursive_shape_audit.sol",
        contract_name="RecursiveShapeBoxRef", fund_wei=2_000_000)
    harness.call(box_refs, "seed()")
    assert as_int(harness.call(box_refs, "run()").abi_return) == 707
    assert as_int(harness.call(box_refs, "runElementRef()").abi_return) == 808
    assert _ints(harness.call(box_refs, "runNestedRef()").abi_return) == (910, 1)
    assert _ints(harness.call(box_refs, "runScalarRef()").abi_return) == (
        2, 520, 616)
    assert _ints(harness.call(box_refs, "runMixedRef()").abi_return) == (717, 818)

    asm_arrays = harness.compile_and_deploy(
        "puyasolRegression/contracts/recursive_shape_audit.sol",
        contract_name="RecursiveShapeAsmArrayRoot", fund_wei=3_000_000)
    harness.call(asm_arrays, "resizeRoots(uint256,uint256)", 3, 2)
    harness.call(asm_arrays, "resizeMembers(uint256,uint256)", 4, 3)
    assert _ints(harness.call(asm_arrays, "lengths()").abi_return) == (
        3, 2, 0, 4, 3, 0)
