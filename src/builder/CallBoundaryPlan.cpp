#include "builder/CallBoundaryPlan.h"
#include "builder/sol-types/RefParamPassing.h"
#include "builder/sol-types/SolIntType.h"
#include "builder/sol-types/TypeCoercion.h"
#include "builder/abi/EvmAbiDecode.h"
#include "Logger.h"
#include "awst/Termination.hpp"
#include "awst/TupleValue.h"

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
		bool const returnedSlots = function.returnParameters().size() > 1
			&& storageRefReturnUsesSlot(&function, analysis());
		parameter.passing = returnedSlots
			&& declaration.referenceLocation() == VariableDeclaration::Location::Storage
			? RefParamPassing::SlotHandle : classifyRefParamPassing(*this, declaration, asmSlot);
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
			parameter.setAbiWireType(*this, declaration.type(), assembly);
		plan.parameters.push_back(std::move(parameter));
	}
	plan.writeBackParams = plan.storageWriteBackParams;
	plan.writeBackParams.insert(plan.writeBackParams.end(), plan.memoryWriteBackParams.begin(), plan.memoryWriteBackParams.end());
	return m_callPlans.emplace(key, std::move(plan)).first->second;
}

void CallParameterPlan::setAbiWireType(
	TypeMapper& types, solidity::frontend::Type const* solType, bool assembly)
{
	wireType = type;
	signedDecodeBits = 0;
	if (type == awst::WType::biguintType())
	{
		auto integer = SolIntType::fromSol(solType);
		unsigned bits = integer ? integer->bits : 256;
		wireType = types.createType<awst::ARC4UIntN>(static_cast<int>(bits));
		if (integer && integer->isSigned && bits > 64 && bits < 256)
			signedDecodeBits = bits;
	}
	else if (!assembly && type)
	{
		auto kind = type->kind();
		if (kind == awst::WTypeKind::ReferenceArray || kind == awst::WTypeKind::ARC4StaticArray
			|| kind == awst::WTypeKind::ARC4DynamicArray || kind == awst::WTypeKind::WTuple
			|| (kind == awst::WTypeKind::Bytes
				&& dynamic_cast<solidity::frontend::FunctionType const*>(solType)))
			wireType = types.mapToARC4Type(type);
	}
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
		if (statement.value)
		{
			if (dynamic_cast<awst::WTuple const*>(statement.value->wtype))
				tuple->items = awst::tupleItems(std::move(statement.value), loc);
			else
				tuple->items.push_back(std::move(statement.value));
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
	if (dynamic_cast<awst::WTuple const*>(value->wtype))
		if (auto const* tuple = dynamic_cast<awst::WTuple const*>(native))
		{
			auto items = awst::tupleItems(std::move(value), loc);
			auto result = awst::makeTupleExpression(native, loc);
			for (size_t i = 0; i < tuple->types().size(); ++i)
				result->items.push_back(decodeCallResult(
					std::move(items.at(i)), tuple->types()[i], loc));
			return result;
		}
	if (dynamic_cast<awst::ARC4UIntN const*>(value->wtype))
		return TypeCoercion::implicitNumericCast(awst::makeARC4Decode(std::move(value), awst::WType::biguintType(), loc), native, loc);
	if (native && native->kind() == awst::WTypeKind::ReferenceArray)
		return awst::makeConvertArray(std::move(value), native, loc);
	return TypeCoercion::coerceForAssignment(std::move(value), native, loc);
}

std::shared_ptr<awst::Expression> decodeExternalCallResult(
	TypeMapper& types, std::shared_ptr<awst::Expression> bytes,
	std::vector<solidity::frontend::Type const*> const& returns,
	awst::WType const* native, awst::SourceLocation const& loc,
	std::vector<std::shared_ptr<awst::Statement>>& out)
{
	if (types.profile().contractAbi == ContractAbi::Evm)
	{
		if (!abi::canDecodeEvmAbi(returns))
		{
			Logger::instance().error("external return type is not representable in canonical Solidity ABI", loc);
			return awst::makeVoidConstant(loc);
		}
		return abi::decodeEvmAbi(types, std::move(bytes), returns, native, loc, out);
	}
	if (returns.empty()) return awst::makeVoidConstant(loc);
	std::vector<awst::WType const*> wireElements, decodedElements;
	for (auto const* type: returns)
	{
		auto element = planReturnElement(types, type, abiReturnNativeType(types, type));
		wireElements.push_back(types.mapToARC4Type(element.wireType));
		// Signed narrow returns travel as uint256; decode to biguint before
		// the shared native adaptation narrows their two's-complement carrier.
		decodedElements.push_back(element.nativeType);
	}
	auto const* wireType = returns.size() == 1 ? wireElements.front()
		: types.createType<awst::ARC4Tuple>(std::move(wireElements));
	auto const* decodedType = returns.size() == 1 ? decodedElements.front()
		: types.createType<awst::WTuple>(std::move(decodedElements));
	// Each submitted call has its own identity, even if its payload expression
	// looks identical to another call's LastLog read.
	bytes = awst::makeSingleEvaluation(std::move(bytes), awst::WType::bytesType(),
		awst::nextSingleEvalId(), loc);
	std::shared_ptr<awst::Expression> value = awst::makeReinterpretCast(std::move(bytes), wireType, loc);
	if (!awst::structurallyEquivalent(wireType, decodedType))
		value = awst::makeARC4Decode(std::move(value), decodedType, loc);
	return decodeCallResult(std::move(value), native, loc);
}

} // namespace puyasol::builder
