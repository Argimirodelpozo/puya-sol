#include "builder/CallBoundaryPlan.h"
#include "builder/sol-types/RefParamPassing.h"
#include "builder/sol-types/SolIntType.h"
#include "builder/sol-types/TypeCoercion.h"
#include "awst/Termination.hpp"

namespace puyasol::builder
{

CallBoundaryPlan const& TypeMapper::callBoundaryPlan(
	solidity::frontend::FunctionDefinition const& function,
	solidity::frontend::ContractDefinition const* mostDerived)
{
	using namespace solidity::frontend;
	auto const* owner = function.annotation().contract;
	bool const freestanding = function.isFree() || (owner && owner->isLibrary());
	if (freestanding) mostDerived = nullptr;
	auto key = std::make_pair(mostDerived ? mostDerived->id() : int64_t{0}, function.id());
	if (auto it = m_callPlans.find(key); it != m_callPlans.end()) return it->second;
	CallBoundaryPlan plan;
	bool const internalMethod = !freestanding && function.visibility() == Visibility::Internal;
	bool const threadReferences = function.isImplemented()
		&& (internalMethod || (freestanding && function.visibility() != Visibility::Private));
	auto const* mutations = threadReferences ? &analysis().parameterMutations(mostDerived, function) : nullptr;
	bool const assembly = analysis().callablesWithInlineAssembly.contains(function.id());
	for (size_t pi = 0; pi < function.parameters().size(); ++pi)
	{
		auto const& declaration = *function.parameters()[pi];
		CallParameterPlan parameter;
		parameter.declaration = &declaration;
		parameter.name = declaration.name().empty() ? "_param" + std::to_string(pi) : declaration.name();
		bool const asmSlot = analysis().asmSlotReferenceDeclarations.contains(declaration.id());
		if (asmSlot) plan.asmSlotParams.insert(pi);
		parameter.passing = classifyRefParamPassing(*this, declaration, asmSlot);
		parameter.type = refParamWType(parameter.passing, *this, declaration);
		parameter.wireType = parameter.type;
		switch (parameter.passing)
		{
		case RefParamPassing::SlotHandle: plan.slotParams.insert(pi); break;
		case RefParamPassing::BoxKeyPrefix: plan.keyParams.insert(pi); break;
		case RefParamPassing::BlobOffset: plan.blobParams.insert(pi); break;
		case RefParamPassing::Value: break;
		}
		if (parameter.passing == RefParamPassing::BoxKeyPrefix
			&& analysis().structRefOffsetParams.contains(declaration.id()))
			plan.offsetParams.push_back(pi);
		if (threadReferences && freestanding && parameter.passing == RefParamPassing::Value
			&& declaration.referenceLocation() == VariableDeclaration::Location::Storage
			&& function.stateMutability() != StateMutability::Pure
			&& function.stateMutability() != StateMutability::View)
			plan.storageWriteBackParams.push_back(pi);
		if (mutations && mutations->mutates(pi)
			&& declaration.referenceLocation() == VariableDeclaration::Location::Memory
			&& isMemoryRefWriteBackType(declaration.type())
			&& (internalMethod || parameter.passing != RefParamPassing::BlobOffset))
			plan.memoryWriteBackParams.push_back(pi);

		// ABI entries and function-pointer adapters share this recipe, including
		// the declared underlying width of a user-defined value type.
		if (function.isPartOfExternalInterface())
		{
			if (parameter.type == awst::WType::biguintType())
			{
				auto integer = SolIntType::fromSol(declaration.type());
				unsigned bits = integer ? integer->bits : 256;
				parameter.wireType = createType<awst::ARC4UIntN>(static_cast<int>(bits));
				if (integer && integer->isSigned && bits > 64 && bits < 256)
					parameter.signedDecodeBits = bits;
			}
			else if (!assembly && parameter.type)
			{
				auto kind = parameter.type->kind();
				if (kind == awst::WTypeKind::ReferenceArray || kind == awst::WTypeKind::ARC4StaticArray
					|| kind == awst::WTypeKind::ARC4DynamicArray || kind == awst::WTypeKind::WTuple
					|| (kind == awst::WTypeKind::Bytes && dynamic_cast<FunctionType const*>(declaration.type())))
					parameter.wireType = mapToARC4Type(parameter.type);
			}
		}
		plan.parameters.push_back(std::move(parameter));
	}
	plan.writeBackParams = plan.storageWriteBackParams;
	plan.writeBackParams.insert(plan.writeBackParams.end(), plan.memoryWriteBackParams.begin(), plan.memoryWriteBackParams.end());
	return m_callPlans.emplace(key, std::move(plan)).first->second;
}

awst::WType const* CallBoundaryPlan::augmentReturn(TypeMapper& mapper, awst::WType const* original) const
{
	if (writeBackParams.empty()) return original;
	std::vector<awst::WType const*> types;
	if (auto const* tuple = dynamic_cast<awst::WTuple const*>(original)) types = tuple->types();
	else if (original != awst::WType::voidType()) types.push_back(original);
	for (auto pi: writeBackParams) types.push_back(parameters[pi].type);
	return types.size() == 1 ? types.front() : mapper.createType<awst::WTuple>(std::move(types));
}

void CallBoundaryPlan::augmentReturns(awst::Block& body, awst::WType const* augmented) const
{
	if (writeBackParams.empty()) return;
	awst::forEachReturnStatement(body.body, [&](awst::ReturnStatement& statement) {
		auto const& loc = statement.sourceLocation;
		if (!dynamic_cast<awst::WTuple const*>(augmented))
		{
			auto const& parameter = parameters[writeBackParams.front()];
			statement.value = awst::makeVarExpression(parameter.name, parameter.type, loc);
			return;
		}
		auto tuple = awst::makeTupleExpression(augmented, loc);
		if (auto const* literal = dynamic_cast<awst::TupleExpression const*>(statement.value.get()))
			tuple->items = literal->items;
		else if (statement.value)
		{
			if (auto const* original = dynamic_cast<awst::WTuple const*>(statement.value->wtype))
			{
				auto value = awst::makeEvalOnce(std::move(statement.value), loc);
				for (size_t i = 0; i < original->types().size(); ++i)
					tuple->items.push_back(awst::makeTupleItem(value, static_cast<int>(i), original->types()[i], loc));
			}
			else tuple->items.push_back(std::move(statement.value));
		}
		for (auto pi: writeBackParams)
			tuple->items.push_back(awst::makeVarExpression(parameters[pi].name, parameters[pi].type, loc));
		statement.value = std::move(tuple);
	});
}

std::shared_ptr<awst::Expression> CallParameterPlan::encodeArgument(
	std::shared_ptr<awst::Expression> value, awst::SourceLocation const& loc) const
{
	if (!value || wireType == type || awst::structurallyEquivalent(value->wtype, wireType)) return value;
	// Signed wide carriers are canonical 256-bit TC; the input wire carries
	// only the declared N bits. The callee sign-extends after decoding.
	if (signedDecodeBits)
		value = TypeCoercion::maskUnsignedToWidth(std::move(value), signedDecodeBits, loc);
	return awst::makeARC4Encode(std::move(value), wireType, loc);
}

std::shared_ptr<awst::Expression> decodeCallResult(
	std::shared_ptr<awst::Expression> value, awst::WType const* native, awst::SourceLocation const& loc)
{
	if (!value || awst::structurallyEquivalent(value->wtype, native)) return value;
	if (auto const* wire = dynamic_cast<awst::WTuple const*>(value->wtype))
		if (auto const* tuple = dynamic_cast<awst::WTuple const*>(native))
		{
			value = awst::makeEvalOnce(std::move(value), loc);
			auto result = awst::makeTupleExpression(native, loc);
			for (size_t i = 0; i < tuple->types().size(); ++i)
				result->items.push_back(decodeCallResult(
					awst::makeTupleItem(value, static_cast<int>(i), wire->types().at(i), loc), tuple->types()[i], loc));
			return result;
		}
	if (dynamic_cast<awst::ARC4UIntN const*>(value->wtype))
		return TypeCoercion::implicitNumericCast(awst::makeARC4Decode(std::move(value), awst::WType::biguintType(), loc), native, loc);
	if (native && native->kind() == awst::WTypeKind::ReferenceArray)
		return awst::makeConvertArray(std::move(value), native, loc);
	return TypeCoercion::coerceForAssignment(std::move(value), native, loc);
}

} // namespace puyasol::builder
