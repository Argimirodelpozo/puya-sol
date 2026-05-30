"""Tests for the events category."""
import pytest

from algosdk import encoding
from framework import (
    Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted,
    as_int, as_bytes,
)


def test_emit_three_identical_events(harness):
    """events/contracts/emit_three_identical_events.sol"""
    app = harness.compile_and_deploy("events/contracts/emit_three_identical_events.sol")
    # terminate() ->
    r = harness.call(app, "terminate()")
    # (void return — call succeeding is the assertion)

def test_emit_two_identical_events(harness):
    """events/contracts/emit_two_identical_events.sol"""
    app = harness.compile_and_deploy("events/contracts/emit_two_identical_events.sol")
    # terminate() ->
    r = harness.call(app, "terminate()")
    # (void return — call succeeding is the assertion)

@pytest.mark.xfail(reason="Yul `log3` has no AVM equivalent; now a hard compile error per EVM_DIVERGENCE.md (was a silent stub-to-0)", strict=False)
def test_event(harness):
    """events/contracts/event.sol"""
    app = harness.compile_and_deploy("events/contracts/event.sol")
    # deposit(bytes32,bool), 18 wei: 0x1234, true ->
    r = harness.call(app, "deposit(bytes32,bool)", (4660).to_bytes(32, "big"), True, payment_wei=18)
    # (void return — call succeeding is the assertion)
    # deposit(bytes32,bool), 18 wei: 0x1234, false ->
    r = harness.call(app, "deposit(bytes32,bool)", (4660).to_bytes(32, "big"), False, payment_wei=18)
    # (void return — call succeeding is the assertion)

def test_event_access_through_base_name_emit(harness):
    """events/contracts/event_access_through_base_name_emit.sol"""
    app = harness.compile_and_deploy("events/contracts/event_access_through_base_name_emit.sol")
    # f() -> 1
    r = harness.call(app, "f()")
    assert as_int(r.abi_return) == 1

def test_event_anonymous(harness):
    """events/contracts/event_anonymous.sol"""
    app = harness.compile_and_deploy("events/contracts/event_anonymous.sol")
    # deposit() ->
    r = harness.call(app, "deposit()")
    # (void return — call succeeding is the assertion)

def test_event_anonymous_with_signature_collision(harness):
    """events/contracts/event_anonymous_with_signature_collision.sol"""
    app = harness.compile_and_deploy("events/contracts/event_anonymous_with_signature_collision.sol")
    # deposit(bytes32), 18 wei: 0x1234 ->
    r = harness.call(app, "deposit(bytes32)", (4660).to_bytes(32, "big"), payment_wei=18)
    # (void return — call succeeding is the assertion)

def test_event_anonymous_with_signature_collision2(harness):
    """events/contracts/event_anonymous_with_signature_collision2.sol"""
    app = harness.compile_and_deploy("events/contracts/event_anonymous_with_signature_collision2.sol")
    # deposit(bytes32), 18 wei: 0x1234 ->
    r = harness.call(app, "deposit(bytes32)", (4660).to_bytes(32, "big"), payment_wei=18)
    # (void return — call succeeding is the assertion)

def test_event_anonymous_with_topics(harness):
    """events/contracts/event_anonymous_with_topics.sol"""
    app = harness.compile_and_deploy("events/contracts/event_anonymous_with_topics.sol")
    # deposit(bytes32), 18 wei: 0x1234 ->
    r = harness.call(app, "deposit(bytes32)", (4660).to_bytes(32, "big"), payment_wei=18)
    # (void return — call succeeding is the assertion)

def test_event_constructor(harness):
    """events/contracts/event_constructor.sol"""
    app = harness.compile_and_deploy("events/contracts/event_constructor.sol")
    # constructor-only test — deployment succeeding is the assertion

def test_event_dynamic_array_memory(harness):
    """events/contracts/event_dynamic_array_memory.sol"""
    app = harness.compile_and_deploy("events/contracts/event_dynamic_array_memory.sol")
    # createEvent(uint256): 42 ->
    r = harness.call(app, "createEvent(uint256)", 42)
    # (void return — call succeeding is the assertion)

def test_event_dynamic_array_memory_v2(harness):
    """events/contracts/event_dynamic_array_memory_v2.sol"""
    app = harness.compile_and_deploy("events/contracts/event_dynamic_array_memory_v2.sol")
    # createEvent(uint256): 42 ->
    r = harness.call(app, "createEvent(uint256)", 42)
    # (void return — call succeeding is the assertion)

def test_event_dynamic_array_storage(harness):
    """events/contracts/event_dynamic_array_storage.sol"""
    app = harness.compile_and_deploy("events/contracts/event_dynamic_array_storage.sol")
    # createEvent(uint256): 42 ->
    r = harness.call(app, "createEvent(uint256)", 42)
    # (void return — call succeeding is the assertion)

def test_event_dynamic_array_storage_v2(harness):
    """events/contracts/event_dynamic_array_storage_v2.sol"""
    app = harness.compile_and_deploy("events/contracts/event_dynamic_array_storage_v2.sol")
    # createEvent(uint256): 42 ->
    r = harness.call(app, "createEvent(uint256)", 42)
    # (void return — call succeeding is the assertion)

def test_event_dynamic_nested_array_memory_v2(harness):
    """events/contracts/event_dynamic_nested_array_memory_v2.sol"""
    app = harness.compile_and_deploy("events/contracts/event_dynamic_nested_array_memory_v2.sol")
    # createEvent(uint256): 42 ->
    r = harness.call(app, "createEvent(uint256)", 42)
    # (void return — call succeeding is the assertion)

def test_event_dynamic_nested_array_storage_v2(harness):
    """events/contracts/event_dynamic_nested_array_storage_v2.sol"""
    app = harness.compile_and_deploy("events/contracts/event_dynamic_nested_array_storage_v2.sol")
    # createEvent(uint256): 42 ->
    r = harness.call(app, "createEvent(uint256)", 42)
    # (void return — call succeeding is the assertion)

def test_event_emit(harness):
    """events/contracts/event_emit.sol"""
    app = harness.compile_and_deploy("events/contracts/event_emit.sol")
    # deposit(bytes32), 18 wei: 0x1234 ->
    r = harness.call(app, "deposit(bytes32)", (4660).to_bytes(32, "big"), payment_wei=18)
    # (void return — call succeeding is the assertion)

def test_event_emit_file_level(harness):
    """events/contracts/event_emit_file_level.sol"""
    app = harness.compile_and_deploy("events/contracts/event_emit_file_level.sol")
    # deposit(bytes32), 18 wei: 0x1234 ->
    r = harness.call(app, "deposit(bytes32)", (4660).to_bytes(32, "big"), payment_wei=18)
    # (void return — call succeeding is the assertion)

def test_event_emit_from_a_foreign_contract(harness):
    """events/contracts/event_emit_from_a_foreign_contract.sol"""
    app = harness.compile_and_deploy("events/contracts/event_emit_from_a_foreign_contract.sol")
    # test() ->
    r = harness.call(app, "test()")
    # (void return — call succeeding is the assertion)

def test_event_emit_from_a_foreign_contract_same_name(harness):
    """events/contracts/event_emit_from_a_foreign_contract_same_name.sol"""
    app = harness.compile_and_deploy("events/contracts/event_emit_from_a_foreign_contract_same_name.sol")
    # test() ->
    r = harness.call(app, "test()")
    # (void return — call succeeding is the assertion)

def test_event_emit_from_module_via_member_access(harness):
    """events/contracts/event_emit_from_module_via_member_access.sol"""
    app = harness.compile_and_deploy("events/contracts/event_emit_from_module_via_member_access.sol")
    # returnAddress() ->
    r = harness.call(app, "returnAddress()")
    # (void return — call succeeding is the assertion)

def test_event_emit_from_other_contract(harness):
    """events/contracts/event_emit_from_other_contract.sol"""
    app = harness.compile_and_deploy("events/contracts/event_emit_from_other_contract.sol")
    # deposit(bytes32), 18 wei: 0x1234 ->
    r = harness.call(app, "deposit(bytes32)", (4660).to_bytes(32, "big"), payment_wei=18)
    # (void return — call succeeding is the assertion)

def test_event_emit_interface_event_via_library(harness):
    """events/contracts/event_emit_interface_event_via_library.sol"""
    app = harness.compile_and_deploy("events/contracts/event_emit_interface_event_via_library.sol")
    # g() ->
    r = harness.call(app, "g()")
    # (void return — call succeeding is the assertion)

def test_event_emit_via_interface(harness):
    """events/contracts/event_emit_via_interface.sol"""
    app = harness.compile_and_deploy("events/contracts/event_emit_via_interface.sol")
    # emitEvent(uint256): 100 ->
    r = harness.call(app, "emitEvent(uint256)", 100)
    # (void return — call succeeding is the assertion)

def test_event_indexed_function(harness):
    """events/contracts/event_indexed_function.sol"""
    app = harness.compile_and_deploy("events/contracts/event_indexed_function.sol")
    # f() ->
    r = harness.call(app, "f()")
    # (void return — call succeeding is the assertion)

def test_event_indexed_function2(harness):
    """events/contracts/event_indexed_function2.sol"""
    app = harness.compile_and_deploy("events/contracts/event_indexed_function2.sol")
    # f1() ->
    r = harness.call(app, "f1()")
    # (void return — call succeeding is the assertion)
    # f2(uint256): 1 ->
    r = harness.call(app, "f2(uint256)", 1)
    # (void return — call succeeding is the assertion)

def test_event_indexed_mixed(harness):
    """events/contracts/event_indexed_mixed.sol"""
    app = harness.compile_and_deploy("events/contracts/event_indexed_mixed.sol")
    # deposit() ->
    r = harness.call(app, "deposit()")
    # (void return — call succeeding is the assertion)

def test_event_indexed_string(harness):
    """events/contracts/event_indexed_string.sol"""
    app = harness.compile_and_deploy("events/contracts/event_indexed_string.sol")
    # deposit() ->
    r = harness.call(app, "deposit()")
    # (void return — call succeeding is the assertion)

def test_event_lots_of_data(harness):
    """events/contracts/event_lots_of_data.sol"""
    app = harness.compile_and_deploy("events/contracts/event_lots_of_data.sol")
    # deposit(bytes32), 18 wei: 0x1234 ->
    r = harness.call(app, "deposit(bytes32)", (4660).to_bytes(32, "big"), payment_wei=18)
    # (void return — call succeeding is the assertion)

def test_event_no_arguments(harness):
    """events/contracts/event_no_arguments.sol"""
    app = harness.compile_and_deploy("events/contracts/event_no_arguments.sol")
    # deposit() ->
    r = harness.call(app, "deposit()")
    # (void return — call succeeding is the assertion)

def test_event_really_lots_of_data(harness):
    """events/contracts/event_really_lots_of_data.sol"""
    app = harness.compile_and_deploy("events/contracts/event_really_lots_of_data.sol")
    # deposit() ->
    r = harness.call(app, "deposit()")
    # (void return — call succeeding is the assertion)

def test_event_really_lots_of_data_from_storage(harness):
    """events/contracts/event_really_lots_of_data_from_storage.sol"""
    app = harness.compile_and_deploy("events/contracts/event_really_lots_of_data_from_storage.sol")
    # deposit() ->
    r = harness.call(app, "deposit()")
    # (void return — call succeeding is the assertion)

def test_event_really_really_lots_of_data_from_storage(harness):
    """events/contracts/event_really_really_lots_of_data_from_storage.sol"""
    app = harness.compile_and_deploy("events/contracts/event_really_really_lots_of_data_from_storage.sol")
    # deposit() ->
    r = harness.call(app, "deposit()")
    # (void return — call succeeding is the assertion)

def test_event_selector(harness):
    """events/contracts/event_selector.sol"""
    app = harness.compile_and_deploy("events/contracts/event_selector.sol")
    # test1() -> 0x92bbf6e823a631f3c8e09b1c8df90f378fb56f7fbc9701827e1ff8aad7f6a028, 0x2ff0672f372fbe844b353429d4510ea5e43683af134c54f75f789ff57bc0c0, 0x92bbf6e823a631f3c8e09b1c8df90f378fb56f7fbc9701827e1ff8aad7f6a028, 0x92bbf6e823a631f3c8e09b1c8df90f378fb56f7fbc9701827e1ff8aad7f6a028
    r = harness.call(app, "test1()")
    assert tuple(as_int(x) for x in r.abi_return) == (66369780382333686747531266010219861741175586311421417686517335350473919995944, 84701013014700633381896489586161358225060424425641365676549285893971361984, 66369780382333686747531266010219861741175586311421417686517335350473919995944, 66369780382333686747531266010219861741175586311421417686517335350473919995944)
    # test2() -> 0x92bbf6e823a631f3c8e09b1c8df90f378fb56f7fbc9701827e1ff8aad7f6a028, 0x2ff0672f372fbe844b353429d4510ea5e43683af134c54f75f789ff57bc0c0, 0x92bbf6e823a631f3c8e09b1c8df90f378fb56f7fbc9701827e1ff8aad7f6a028, 0x92bbf6e823a631f3c8e09b1c8df90f378fb56f7fbc9701827e1ff8aad7f6a028, 0x92bbf6e823a631f3c8e09b1c8df90f378fb56f7fbc9701827e1ff8aad7f6a028
    r = harness.call(app, "test2()")
    # TODO: verify structural decoding matches expected: 66369780382333686747531266010219861741175586311421417686517335350473919995944, 84701013014700633381896489586161358225060424425641365676549285893971361984, 66369780382333686747531266010219861741175586311421417686517335350473919995944, 66369780382333686747531266010219861741175586311421417686517335350473919995944, 66369780382333686747531266010219861741175586311421417686517335350473919995944
    assert not r.reverted
    # test3() -> 0x28811f5935c16a099486acb976b3a6b4942950a1425a74e9eb3e9b7f7135e12a
    r = harness.call(app, "test3()")
    assert as_int(r.abi_return) == 18320653573920188446226298860793104238698786666923688161409786579693587521834

def test_event_selector_file_level(harness):
    """events/contracts/event_selector_file_level.sol"""
    app = harness.compile_and_deploy("events/contracts/event_selector_file_level.sol")
    # main() -> 0x92bbf6e823a631f3c8e09b1c8df90f378fb56f7fbc9701827e1ff8aad7f6a028, 0x92bbf6e823a631f3c8e09b1c8df90f378fb56f7fbc9701827e1ff8aad7f6a028
    r = harness.call(app, "main()")
    assert tuple(as_int(x) for x in r.abi_return) == (66369780382333686747531266010219861741175586311421417686517335350473919995944, 66369780382333686747531266010219861741175586311421417686517335350473919995944)

def test_event_shadowing_file_level(harness):
    """events/contracts/event_shadowing_file_level.sol"""
    app = harness.compile_and_deploy("events/contracts/event_shadowing_file_level.sol")
    # main() -> 0x3e9992c940c54ea252d3a34557cc3d3014281525c43d694f89d5f3dfd820b07d, 0x3e9992c940c54ea252d3a34557cc3d3014281525c43d694f89d5f3dfd820b07d, 0x92bbf6e823a631f3c8e09b1c8df90f378fb56f7fbc9701827e1ff8aad7f6a028
    r = harness.call(app, "main()")
    assert tuple(as_int(x) for x in r.abi_return) == (28314737293810674527488474353404243661761185637991170190359344066147653824637, 28314737293810674527488474353404243661761185637991170190359344066147653824637, 66369780382333686747531266010219861741175586311421417686517335350473919995944)
    # k_main() -> 0x92bbf6e823a631f3c8e09b1c8df90f378fb56f7fbc9701827e1ff8aad7f6a028, 0x3e9992c940c54ea252d3a34557cc3d3014281525c43d694f89d5f3dfd820b07d, 0x92bbf6e823a631f3c8e09b1c8df90f378fb56f7fbc9701827e1ff8aad7f6a028
    r = harness.call(app, "k_main()")
    assert tuple(as_int(x) for x in r.abi_return) == (66369780382333686747531266010219861741175586311421417686517335350473919995944, 28314737293810674527488474353404243661761185637991170190359344066147653824637, 66369780382333686747531266010219861741175586311421417686517335350473919995944)

def test_event_signature_in_library(harness):
    """events/contracts/event_signature_in_library.sol"""
    app = harness.compile_and_deploy("events/contracts/event_signature_in_library.sol")
    # constructor-only test — deployment succeeding is the assertion

def test_event_static_calldata_uint_array_and_dynamic_array(harness):
    """events/contracts/event_static_calldata_uint_array_and_dynamic_array.sol"""
    app = harness.compile_and_deploy("events/contracts/event_static_calldata_uint_array_and_dynamic_array.sol")
    r = harness.call(app, "f(uint256[],uint256[1])", [255], [65535])
    assert not r.reverted

def test_event_string(harness):
    """events/contracts/event_string.sol"""
    app = harness.compile_and_deploy("events/contracts/event_string.sol")
    # deposit() ->
    r = harness.call(app, "deposit()")
    # (void return — call succeeding is the assertion)

def test_event_struct_memory_v2(harness):
    """events/contracts/event_struct_memory_v2.sol"""
    app = harness.compile_and_deploy("events/contracts/event_struct_memory_v2.sol")
    # createEvent(uint256): 42 ->
    r = harness.call(app, "createEvent(uint256)", 42)
    # (void return — call succeeding is the assertion)

def test_event_struct_storage_v2(harness):
    """events/contracts/event_struct_storage_v2.sol"""
    app = harness.compile_and_deploy("events/contracts/event_struct_storage_v2.sol")
    # createEvent(uint256): 42 ->
    r = harness.call(app, "createEvent(uint256)", 42)
    # (void return — call succeeding is the assertion)

def test_events_with_same_name(harness):
    """events/contracts/events_with_same_name.sol"""
    app = harness.compile_and_deploy("events/contracts/events_with_same_name.sol")
    # deposit() -> 1
    r = harness.call(app, "deposit()")
    assert as_int(r.abi_return) == 1
    # deposit(address): 0x5082a85c489be6aa0f2e6693bf09cc1bbd35e988 -> 2
    r = harness.call(app, "deposit(address)", encoding.encode_address((459633024808244335532108103627335112557220718984).to_bytes(32, "big")))
    assert as_int(r.abi_return) == 2
    # deposit(address,uint256): 0x5082a85c489be6aa0f2e6693bf09cc1bbd35e988, 100 -> 3
    r = harness.call(app, "deposit(address,uint256)", encoding.encode_address((459633024808244335532108103627335112557220718984).to_bytes(32, "big")), 100)
    assert as_int(r.abi_return) == 3
    # deposit(address,bool): 0x5082a85c489be6aa0f2e6693bf09cc1bbd35e988, false -> 4
    r = harness.call(app, "deposit(address,bool)", encoding.encode_address((459633024808244335532108103627335112557220718984).to_bytes(32, "big")), False)
    assert as_int(r.abi_return) == 4

def test_events_with_same_name_file_level(harness):
    """events/contracts/events_with_same_name_file_level.sol"""
    app = harness.compile_and_deploy("events/contracts/events_with_same_name_file_level.sol")
    # deposit() -> 1
    r = harness.call(app, "deposit()")
    assert as_int(r.abi_return) == 1
    # deposit(address): 0x5082a85c489be6aa0f2e6693bf09cc1bbd35e988 -> 2
    r = harness.call(app, "deposit(address)", encoding.encode_address((459633024808244335532108103627335112557220718984).to_bytes(32, "big")))
    assert as_int(r.abi_return) == 2
    # deposit(address,uint256): 0x5082a85c489be6aa0f2e6693bf09cc1bbd35e988, 100 -> 3
    r = harness.call(app, "deposit(address,uint256)", encoding.encode_address((459633024808244335532108103627335112557220718984).to_bytes(32, "big")), 100)
    assert as_int(r.abi_return) == 3
    # deposit(address,bool): 0x5082a85c489be6aa0f2e6693bf09cc1bbd35e988, false -> 4
    r = harness.call(app, "deposit(address,bool)", encoding.encode_address((459633024808244335532108103627335112557220718984).to_bytes(32, "big")), False)
    assert as_int(r.abi_return) == 4

def test_events_with_same_name_inherited_emit(harness):
    """events/contracts/events_with_same_name_inherited_emit.sol"""
    app = harness.compile_and_deploy("events/contracts/events_with_same_name_inherited_emit.sol")
    # deposit() -> 1
    r = harness.call(app, "deposit()")
    assert as_int(r.abi_return) == 1
    # deposit(address): 0x5082a85c489be6aa0f2e6693bf09cc1bbd35e988 -> 1
    r = harness.call(app, "deposit(address)", encoding.encode_address((459633024808244335532108103627335112557220718984).to_bytes(32, "big")))
    assert as_int(r.abi_return) == 1
    # deposit(address,uint256): 0x5082a85c489be6aa0f2e6693bf09cc1bbd35e988, 100 -> 1
    r = harness.call(app, "deposit(address,uint256)", encoding.encode_address((459633024808244335532108103627335112557220718984).to_bytes(32, "big")), 100)
    assert as_int(r.abi_return) == 1

def test_simple(harness):
    """events/contracts/simple.sol"""
    app = harness.compile_and_deploy("events/contracts/simple.sol")
    # f() ->
    r = harness.call(app, "f()")
    # (void return — call succeeding is the assertion)
