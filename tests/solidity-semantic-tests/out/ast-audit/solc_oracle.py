import json
from pathlib import Path
import solcx
from web3 import Web3, EthereumTesterProvider

root = Path(__file__).resolve().parents[4]
sources = root / 'tests/solidity-semantic-tests/tests/puyasolRegression/contracts'
names = ('ast_expression_facts', 'ast_reference_bounds', 'ast_tuple_storage')
extra_sources = {
    'inherited_function_calldata_memory': 'inheritance',
    'array_mapping_abstract_constructor_param': 'types',
}
for via_ir in (False, True):
    compiled = solcx.compile_standard({
        'language': 'Solidity',
        'sources': {name + '.sol': {'content': (sources / (name + '.sol')).read_text()} for name in names}
                   | {name + '.sol': {'content': (sources.parents[1] / category / 'contracts' / (name + '.sol')).read_text()}
                      for name, category in extra_sources.items()},
        'settings': {'viaIR': via_ir, 'optimizer': {'enabled': True}, 'evmVersion': 'cancun',
                     'outputSelection': {'*': {'*': ['abi', 'evm.bytecode.object']}}}},
        solc_binary=str(root / 'solidity/build/solc/solc'))
    web = Web3(EthereumTesterProvider())
    account = web.eth.accounts[0]
    def deploy(source, name):
        artifact = compiled['contracts'][source + '.sol'][name]
        factory = web.eth.contract(abi=artifact['abi'], bytecode=artifact['evm']['bytecode']['object'])
        receipt = web.eth.wait_for_transaction_receipt(factory.constructor().transact({'from': account}))
        return web.eth.contract(address=receipt.contractAddress, abi=artifact['abi'])
    app = deploy(names[0], 'AstExpressionFacts')
    target = deploy(names[0], 'AstOptionsTarget')
    assert app.functions.singleton().call() == 7
    for flag in (False, True):
        assert app.functions.addresses(flag).call()
        assert app.functions.options(target.address, flag).call({'from': account, 'value': 7}) == [12, 7]
    for address in ('0x' + '00' * 20, target.address):
        assert app.functions.foreign(address).call()
    app = deploy(names[1], 'AstReferenceBounds')
    def reverts(call):
        try:
            call.call()
        except Exception:
            return
        raise AssertionError('Expected bounds panic')
    for method, expected in (('element', 123), ('dynamicElement', 124)):
        fn = getattr(app.functions, method)
        assert fn(0).call() == expected
        for i in (1, 2, 2**64, 2**256 - 1):
            reverts(fn(i))
    assert app.functions.nested(0, 0).call() == 123
    for i, j in ((0, 1), (1, 0), (2**64, 0), (0, 2**64)):
        reverts(app.functions.nested(i, j))
    assert app.functions.growing().call() == 123
    assert app.functions.calldataPointer([[11, 22]], 0).call() == 100
    for i in (1, 2, 2**64, 2**256 - 1):
        reverts(app.functions.calldataPointer([[11, 22]], i))
    assert app.functions.calldataLocal([[11, 22]], 0).call() == 100
    reverts(app.functions.calldataLocal([[11, 22]], 1))
    assert app.functions.calldataFixed([[11, 22], [33, 44]], 1).call() == 68
    for i in (2, 2**64, 2**256 - 1):
        reverts(app.functions.calldataFixed([[11, 22], [33, 44]], i))
    app = deploy(names[2], 'AstTupleStorage')
    assert app.functions.dynamicMembers().call() == [11, 11, b'\x01\x02\x03', b'\x01\x02\x03']
    assert app.functions.packedMembers().call({'from': account})
    assert app.functions.parenthesizedDestinations().call() == [7, 9, 2]
    app = deploy('inherited_function_calldata_memory', 'B')
    assert app.functions.g().call() == 23
    app = deploy('array_mapping_abstract_constructor_param', 'C')
    assert app.functions.m(1, 0, 1).call() == 2
    assert app.functions.m(1, 0, 5).call() == 0
    print(json.dumps({'via_ir': via_ir, 'regression_oracle': 'passed'}), flush=True)
