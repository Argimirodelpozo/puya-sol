#include "builder/ReturnWirePlan.h"
#include "builder/ProgramAnalysis.h"
#include "builder/sol-ast/StorageRefPointer.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/sol-types/SolIntType.h"
#include "builder/sol-types/Arc4Defaults.h"

#include <libsolidity/ast/AST.h>

namespace puyasol::builder
{

awst::WType const* abiReturnNativeType(
	TypeMapper& types, solidity::frontend::Type const* solType)
{
	if (auto integer = SolIntType::fromSolOrEnum(solType); integer && integer->isSigned)
		return awst::WType::biguintType();
	return types.map(solType);
}

ReturnWireElem planReturnElement(
	TypeMapper& types,
	solidity::frontend::Type const* solType,
	awst::WType const* nativeType)
{
	ReturnWireElem item;
	item.nativeType = item.wireType = nativeType;
	auto integer = SolIntType::fromSolOrEnum(solType);
	if (nativeType == awst::WType::biguintType())
	{
		item.isSigned = integer && integer->isSigned;
		item.bits = integer ? integer->bits : 256u;
		item.wireType = types.createType<awst::ARC4UIntN>(
			static_cast<int>(item.isSigned ? 256u : item.bits));
		item.encoded = true;
	}
	else if (integer && !integer->isSigned && integer->bits < 64)
	{
		item.masked = true;
		item.bits = integer->bits;
	}
	else if (nativeType && nativeType->kind() == awst::WTypeKind::ReferenceArray)
	{
		item.wireType = types.mapToARC4Type(nativeType);
		item.encoded = item.wireType != nativeType;
	}
	return item;
}

FunctionReturnPlan const& TypeMapper::functionReturnPlan(
	solidity::frontend::FunctionDefinition const& function)
{
	using solidity::frontend::VariableDeclaration;
	if (auto it = m_returnPlans.find(function.id()); it != m_returnPlans.end())
		return it->second;

	FunctionReturnPlan plan;
	auto const& returns = function.returnParameters();
	std::vector<awst::WType const*> nativeTypes, wireTypes;
	std::vector<std::string> names;
	bool hasNames = false;
	for (auto const& parameter: returns)
	{
		auto const* native = function.isPartOfExternalInterface()
			? abiReturnNativeType(*this, parameter->type()) : map(parameter->type());
		bool const storage = parameter->referenceLocation() == VariableDeclaration::Location::Storage;
		if (storage && (profile().evmStorageLayout || storageRefReturnUsesSlot(&function, analysis())))
			native = awst::WType::biguintType();
		else if (returns.size() == 1 && storageRefPointerReturn(&function, analysis()))
			native = storageRefReturnIsBytesKeyed(&function, analysis())
				? awst::WType::bytesType() : awst::WType::uint64Type();
		plan.elements.push_back(planReturnElement(*this, parameter->type(), native));
		nativeTypes.push_back(native);
		wireTypes.push_back(plan.elements.back().wireType);
		names.push_back(parameter->name());
		hasNames |= !parameter->name().empty();
	}
	if (returns.empty())
		plan.nativeType = plan.wireType = awst::WType::voidType();
	else if (returns.size() == 1)
	{
		plan.nativeType = nativeTypes.front();
		plan.wireType = wireTypes.front();
	}
	else
	{
		plan.nativeType = hasNames
			? createType<awst::WTuple>(std::move(nativeTypes), std::move(names), function.name() + "Return")
			: createType<awst::WTuple>(std::move(nativeTypes));
		plan.wireType = createType<awst::WTuple>(std::move(wireTypes));
	}
	plan.internalType = plan.nativeType;
	// The existing blob-return protocol transports a named memory result as
	// its uint64 base offset; the caller reconstructs the source-level value.
	if (returns.size() == 1 && !returns[0]->name().empty()
		&& returns[0]->referenceLocation() == VariableDeclaration::Location::Memory
		&& memoryUsesBlob(plan.nativeType))
		plan.internalType = awst::WType::uint64Type();
	return m_returnPlans.emplace(function.id(), std::move(plan)).first->second;
}

} // namespace puyasol::builder
