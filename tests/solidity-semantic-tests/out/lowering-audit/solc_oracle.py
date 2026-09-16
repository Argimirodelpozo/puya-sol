"""Pinned-solc/PyEVM oracle for the lowering-audit runtime regressions."""

import hashlib
import json
from pathlib import Path

import solcx
from web3 import Web3, EthereumTesterProvider
from eth_tester.exceptions import TransactionFailed

ROOT = Path(__file__).resolve().parents[4]
CONTRACTS = ROOT / 'tests/solidity-semantic-tests/tests/puyasolRegression/contracts'
for via_ir in (False, True):
    for source in ('lowering_calls', 'lowering_precompiles', 'lowering_pointer_override',
                   'lowering_returndata', 'lowering_parentheses', 'lowering_wire', 'lowering_static'):
        compiled = solcx.compile_standard({
            'language': 'Solidity',
            'sources': {source + '.sol': {'content': (CONTRACTS / (source + '.sol')).read_text()}},
            'settings': {'viaIR': via_ir, 'optimizer': {'enabled': True, 'runs': 200}, 'evmVersion': 'cancun',
                         'outputSelection': {'*': {'*': ['abi', 'evm.bytecode.object']}}}},
            solc_binary=str(ROOT / 'solidity/build/solc/solc'))
        web = Web3(EthereumTesterProvider())
        account = web.eth.accounts[0]

        def deploy(name):
            artifact = compiled['contracts'][source + '.sol'][name]
            factory = web.eth.contract(abi=artifact['abi'], bytecode=artifact['evm']['bytecode']['object'])
            receipt = web.eth.wait_for_transaction_receipt(factory.constructor().transact({'from': account}))
            return web.eth.contract(address=receipt.contractAddress, abi=artifact['abi'])

        if source == 'lowering_calls':
            app, sink = deploy('Probe'), deploy('Sink')
            results = {name: getattr(app.functions, name)(*args).call({'from': account, 'value': value})
                       for name, args, value in (
                           ('internalPurePointer', (), 0), ('pointerOptions', (sink.address,), 7),
                           ('fakeEncoder', (sink.address,), 0), ('fakeSelfEncoder', (), 0),
                           ('foreignSelector', (sink.address,), 0), ('foreignSelectorSum', (sink.address,), 0),
                           ('directSelf', (), 0), ('rawSelf', (), 0), ('rawSelectorSelf', (), 0),
                           ('parenthesizedSelf', (), 0), ('emptySelf', (), 0))}
        elif source == 'lowering_precompiles':
            app = deploy('PrecompileProbe')
            results = {name: getattr(app.functions, name)(b'abc').call()
                       for name in ('direct', 'wrapped', 'constantTarget')}
            results['encodedInput'] = app.functions.encodedInput().call()
        elif source == 'lowering_pointer_override':
            app, other = deploy('B'), deploy('A')
            results = {'foreign': app.functions.test(other.address, [5]).call(),
                       'self': app.functions.test(app.address, [5]).call()}
        elif source == 'lowering_returndata':
            results = {'test': deploy('FallbackProbe').functions.test().call()}
        elif source == 'lowering_parentheses':
            app = deploy('Parentheses')
            results = {name: getattr(app.functions, name)().call()
                       for name in ('aliases', 'returnedReferences', 'indexedPaths', 'byteOps', 'allocation', 'singleton')}
        elif source == 'lowering_static':
            app = deploy('StaticProbe')
            results = {name: getattr(app.functions, name)().call()
                       for name in ('internalViewNoStatic', 'staticThenInternal', 'rawNonStatic',
                                    'directStatic', 'rawStatic')}
            for name, args in (('inheritedStatic', ()), ('pointerStatic', (True,)), ('nestedStatic', ())):
                try:
                    getattr(app.functions, name)(*args).call()
                except TransactionFailed:
                    results[name] = 'reverted'
                else:
                    raise AssertionError(name + ' did not revert')
        else:
            app, sink = deploy('WireProbe'), deploy('WireSink')
            results = {kind + str(flag): getattr(app.functions, kind)(sink.address, flag).call()
                       for kind in ('typed', 'raw') for flag in (False, True)}
            assert len(set(results.values())) == 1
            results['librarySelf'] = app.functions.librarySelf().call()

        expected = {
            'lowering_calls': {
                'internalPurePointer': 1, 'pointerOptions': [7, 1100],
                'fakeEncoder': 22, 'fakeSelfEncoder': 22,
                'foreignSelector': 11, 'foreignSelectorSum': 16,
                **{name: [True, (42).to_bytes(32, 'big')] for name in
                   ('directSelf', 'rawSelf', 'rawSelectorSelf', 'parenthesizedSelf')},
                'emptySelf': [True, 5],
            },
            'lowering_precompiles': {
                **{name: [True, hashlib.sha256(b'abc').digest()] for name in
                   ('direct', 'wrapped', 'constantTarget')},
                'encodedInput': [True, hashlib.sha256(bytes.fromhex('26121ff0')).digest()],
            },
            'lowering_pointer_override': {'foreign': 5, 'self': 6},
            'lowering_returndata': {'test': [True, 32, 3, 3]},
            'lowering_parentheses': {
                'aliases': [11, 33], 'returnedReferences': 6, 'indexedPaths': [7, 9, 0],
                'byteOps': [3, 0], 'allocation': [3, 7], 'singleton': 7,
            },
            'lowering_static': {
                'internalViewNoStatic': 1, 'staticThenInternal': [0, 1],
                'rawNonStatic': [True, (1).to_bytes(32, 'big')],
                'directStatic': [False, b''], 'rawStatic': [False, b''],
                'inheritedStatic': 'reverted', 'pointerStatic': 'reverted', 'nestedStatic': 'reverted',
            },
            'lowering_wire': {
                **{kind + str(flag): bytes.fromhex(
                    '49dcde80d7f724a4702bd94f259af938bd27f3851183bfd0de6aa107a36aef3a')
                   for kind in ('typed', 'raw') for flag in (False, True)},
                'librarySelf': 42,
            },
        }[source]
        assert results == expected, (source, via_ir, results, expected)
        print(json.dumps({'source': source, 'via_ir': via_ir, 'results': results},
                         default=lambda obj: obj.hex() if isinstance(obj, bytes) else str(obj)), flush=True)
