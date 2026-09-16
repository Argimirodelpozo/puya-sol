import json
from pathlib import Path
import solcx
from web3 import Web3, EthereumTesterProvider

repo = Path(__file__).resolve().parents[4]
fixtures = repo / 'tests/solidity-semantic-tests/tests/puyasolRegression/contracts'
for via_ir in (False, True):
    for fixture in ('contract_initializers', 'contract_storage_words', 'contract_ingress'):
        compiled = solcx.compile_standard({'language': 'Solidity',
            'sources': {fixture + '.sol': {'content': (fixtures / (fixture + '.sol')).read_text()}},
            'settings': {'evmVersion': 'cancun', 'viaIR': via_ir,
                'optimizer': {'enabled': True, 'runs': 200},
                'outputSelection': {'*': {'*': ['abi', 'evm.bytecode.object']}}}},
            solc_binary=str(repo / 'solidity/build/solc/solc'))
        web = Web3(EthereumTesterProvider())
        for name, artifact in compiled['contracts'][fixture + '.sol'].items():
            receipt = web.eth.wait_for_transaction_receipt(web.eth.send_transaction({
                'from': web.eth.accounts[0], 'data': artifact['evm']['bytecode']['object'], 'gas': 5000000}))
            assert receipt.status == 1, name
            contract = web.eth.contract(address=receipt.contractAddress, abi=artifact['abi'])
            cases = {
                'FixedInitializers': [('xs', [i], i + 11) for i in range(5)],
                'GlobalDefaultInitializers': [('initialized', [], 0), ('entries', [0], 0),
                                              ('entries', [1], 17), ('pair', [], [29, 0])],
                'OrderedInitializers': [('seed', [], 7), ('xs', [0], 7), ('calls', [], 1),
                                        ('afterBytes', [], 1), ('b', [], (7).to_bytes(32, 'big')),
                                        ('written', [], 23), ('forward', [], 19)],
                'InitializerDerived': [('derived', [0], 9 if via_ir else 7), ('beforeConstructor', [0], 7)],
                'BytesInitializers': [('encoded', [], (7).to_bytes(32, 'big')), ('literalValue', [], b'abc'),
                                      ('emptyValue', [], b''), ('composed', [], 'abcd')],
                'BytesStorageHeaders': [(method, [word], 'REVERT') for method in ('read', 'replace')
                                        for word in (1, 3, 63, 64, 126, 254)],
                'RawStorageRead': [('named', [], 0), ('constantSlot', [], 0)]
                    + [('read', [slot], 0) for slot in (0, 777, 1 << 128, (1 << 256) - 1)],
                'ContractIngress': [('fromDirty', [263], 14), ('recurse', [3], 4), ('skipped', [7], 0),
                    ('selfWord', [263], 7), ('internalPointer', [], (1 << 90) + 7),
                    ('externalPointer', [], (1 << 90) + 7)]
                    + [('selfEnum', [value, pointer], 77 if value == 1 else 'REVERT')
                       for pointer in (False, True) for value in (1, 5)],
            }.get(name, [])
            for method, args, expected in cases:
                try:
                    actual = getattr(contract.functions, method)(*args).call()
                except Exception:
                    actual = 'REVERT'
                print(json.dumps({'via_ir': via_ir, 'contract': name, 'method': method,
                    'args': args, 'expected': expected, 'actual': actual, 'match': actual == expected},
                    default=lambda value: value.hex()), flush=True)
                assert actual == expected, (name, method, args, expected, actual)
