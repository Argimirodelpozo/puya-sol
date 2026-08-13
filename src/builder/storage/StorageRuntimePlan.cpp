#include "builder/storage/StorageRuntimePlan.h"

#include "builder/contract/StateVarWalker.h"
#include "builder/storage/EvmLayoutMode.h"

#include <libsolidity/ast/ASTVisitor.h>
#include <libsolidity/ast/Types.h>

#include <functional>

namespace puyasol::builder
{

namespace
{

struct InlineAsmDetector: solidity::frontend::ASTConstVisitor
{
	bool found = false;
	bool visit(solidity::frontend::InlineAssembly const&) override
	{
		found = true;
		return false;
	}
};

bool typeUsesHashedSlots(solidity::frontend::Type const* _type)
{
	if (!_type)
		return true;
	if (dynamic_cast<solidity::frontend::MappingType const*>(_type))
		return true;
	if (auto const* array = dynamic_cast<solidity::frontend::ArrayType const*>(_type))
		return array->isDynamicallySized() || typeUsesHashedSlots(array->baseType());
	if (auto const* structure = dynamic_cast<solidity::frontend::StructType const*>(_type))
	{
		for (auto const& member: structure->structDefinition().members())
			if (member && typeUsesHashedSlots(member->type()))
				return true;
	}
	return false;
}

} // namespace

StorageRuntimePlan StorageRuntimePlan::analyze(
	solidity::frontend::ContractDefinition const& _contract,
	TypeMapper& _typeMapper)
{
	StorageRuntimePlan result;
	result.layout.computeLayout(_contract, _typeMapper);

	InlineAsmDetector asmDetector;
	forEachDefinedFunction(_contract, [&](auto const* _function) {
		if (!asmDetector.found && _function->isImplemented())
			_function->body().accept(asmDetector);
	});
	result.containsInlineAssembly = asmDetector.found;
	result.requiresSparseSlots = asmDetector.found;

	// Packed addresses use a keccak-derived shadow slot for their high bytes.
	for (auto const& variable: result.layout.variables())
	{
		if (variable.slot >= solidity::u256(kEvmDenseSlotLimit))
			result.requiresSparseSlots = true;
		if (variable.wtype == awst::WType::accountType() && variable.byteSize == 20)
			if (auto const* slot = result.layout.getSlotInfo(variable.slot);
				slot && slot->variableIndices.size() > 1)
				result.requiresSparseSlots = true;
	}

	for (auto const* base: _contract.annotation().linearizedBaseContracts)
	{
		if (!base)
			continue;
		for (auto const* variable: base->stateVariables())
		{
			if (!variable || variable->isConstant() || variable->immutable())
				continue;
			if (variable->referenceLocation()
				== solidity::frontend::VariableDeclaration::Location::Transient)
				result.requiresSparseSlots = true;
			if (typeUsesHashedSlots(variable->annotation().type))
				result.requiresSparseSlots = true;
		}
	}

	return result;
}

} // namespace puyasol::builder
