import hashlib
import json
import subprocess
from pathlib import Path

import solcx
from web3 import Web3, EthereumTesterProvider

root = Path(__file__).resolve().parents[4]
source = root / 'tests/solidity-semantic-tests/tests/puyasolRegression/contracts/core_constructor_ingress.sol'
solc = root / 'solidity/build/solc/solc'
cases = {
    'ScalarIngress': [(7, 1, 1), (256, 0, 0), (0, 2, 0), (0, 0, 2), ((1 << 64) + 7, 0, 0)],
    'SignedIngress': [(-128, -(1 << 95)), (-1, -1), (127, (1 << 95) - 1),
                      (128, 0), (255, 0), (0, 1 << 95), (0, (1 << 96) - 1)],
    'UnsignedIngress': [((1 << 96) - 1,), (1 << 96,)],
}
print(json.dumps({'solc': subprocess.check_output([str(solc), '--version'], text=True).strip(),
                  'source_sha256': hashlib.sha256(source.read_bytes()).hexdigest()}))
for coder in (1, 2):
    for via_ir in (False, True):
        text = source.read_text().replace('pragma solidity ^0.8.20;', f'pragma solidity ^0.8.20; pragma abicoder v{coder};')
        output = solcx.compile_standard({'language': 'Solidity', 'sources': {'C.sol': {'content': text}},
            'settings': {'evmVersion': 'cancun', 'viaIR': via_ir, 'optimizer': {'enabled': True, 'runs': 200},
                         'outputSelection': {'*': {'*': ['evm.bytecode.object']}}}}, solc_binary=str(solc))
        web = Web3(EthereumTesterProvider())
        for contract, values in cases.items():
            bytecode = output['contracts']['C.sol'][contract]['evm']['bytecode']['object']
            for index, args in enumerate(values):
                data = bytecode + ''.join((v % (1 << 256)).to_bytes(32, 'big').hex() for v in args)
                receipt = web.eth.wait_for_transaction_receipt(web.eth.send_transaction({
                    'from': web.eth.accounts[0], 'data': data, 'gas': 1000000}))
                expected = index < (3 if contract == 'SignedIngress' else 1)
                if coder == 2 or via_ir:
                    assert receipt.status == int(expected), (coder, via_ir, contract, args, receipt.status)
                state = [web.eth.get_storage_at(receipt.contractAddress, slot).hex()
                         for slot in range(2 if contract == 'SignedIngress' else 1)] if receipt.status else []
                print(json.dumps({'coder': coder, 'via_ir': via_ir, 'contract': contract,
                                  'args': args, 'status': receipt.status, 'storage': state}))
