file(MAKE_DIRECTORY "${OUTPUT_ROOT}/input")
set(input "${OUTPUT_ROOT}/input")
file(WRITE "${input}/Base.sol"
	"pragma solidity ^0.8.20; contract Base { function f() public pure returns(uint64) { return 7; } }")
file(WRITE "${input}/Derived.sol"
	"pragma solidity ^0.8.20; import 'Base.sol'; import {Base as AliasBase} from 'aliases////Base.sol'; contract Derived is Base {}")

# Explicit and imported paths must be normalized by solc to ONE source unit.
execute_process(COMMAND "${PUYA_SOL}"
	--source "${input}/Derived.sol" --source "${input}/./Base.sol"
	--import-path "${input}" --import-path "${input}/."
	--remapping "aliases////Base.sol=Base.sol"
	--no-puya --output-dir "${OUTPUT_ROOT}/aliases"
	RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if(NOT result EQUAL 0)
	message(FATAL_ERROR "source aliases failed: ${output}\n${errors}")
endif()
file(READ "${OUTPUT_ROOT}/aliases/awst.json" awst)
string(JSON root_count LENGTH "${awst}")
if(NOT root_count EQUAL 2)
	message(FATAL_ERROR "expected two distinct declarations, got ${root_count}")
endif()
foreach(i RANGE 0 1)
	string(JSON id GET "${awst}" ${i} id)
	string(JSON name GET "${awst}" ${i} name)
	if(NOT id STREQUAL "${name}.sol:${name}")
		message(FATAL_ERROR "contract identity must come from solc: ${id}")
	endif()
	string(JSON method_count LENGTH "${awst}" ${i} methods)
	math(EXPR last_method "${method_count} - 1")
	foreach(j RANGE 0 ${last_method})
		string(JSON cref GET "${awst}" ${i} methods ${j} cref)
		if(NOT cref STREQUAL id)
			message(FATAL_ERROR "method owner ${cref} does not match ${id}")
		endif()
	endforeach()
endforeach()

# Identical bytes do not prove identity: different source units can resolve
# their imports differently. Real output-name collisions must fail closed.
foreach(name A B)
	file(WRITE "${input}/${name}.sol"
		"pragma solidity ^0.8.20; contract C { function f() public pure returns(uint64) { return 1; } }")
endforeach()
execute_process(COMMAND "${PUYA_SOL}"
	--source "${input}/A.sol" --source "${input}/B.sol" --import-path "${input}"
	--no-puya --output-dir "${OUTPUT_ROOT}/collision"
	RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if(result EQUAL 0 OR NOT "${output}${errors}" MATCHES "artifact name collision: C.*A.sol:C.*B.sol:C")
	message(FATAL_ERROR "distinct contracts must report both identities: ${output}\n${errors}")
endif()
