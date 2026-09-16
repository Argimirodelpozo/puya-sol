#include "builder/types/ConstructorWirePlan.h"
#include "builder/types/TypeMapper.h"
#include "builder/types/TypeCoercion.h"
#include "builder/types/SolIntType.h"
#include "builder/codec/EvmValueCodec.h"
#include <libsolidity/ast/AST.h>

namespace puyasol::builder
{

ConstructorWirePlan::ConstructorWirePlan(TypeMapper& types,
	solidity::frontend::FunctionDefinition const* constructor, bool deferred)
	: m_types(types), m_deferred(deferred)
{
	if (!constructor) return;
	auto const& coder = constructor->sourceUnit().annotation().useABICoderV2;
	m_validate = types.profile().viaIRSequencing || !coder.set() || *coder;
	for (auto const& declaration: constructor->parameters())
	{
		CallParameterPlan parameter;
		parameter.declaration = declaration.get();
		parameter.name = declaration->name().empty()
			? "_param" + std::to_string(parameters.size()) : declaration->name();
		parameter.type = types.map(declaration->type());
		parameter.wireType = parameter.type;
		auto const integer = SolIntType::fromSol(declaration->type());
		// Deferred signed wide values retain the existing native uint512
		// router entry; their Solidity value is canonical 256-bit TC. Create
		// uses a 256-bit word. Unsigned wide values use the declared solc width.
		if (parameter.type == awst::WType::biguintType() && integer && integer->isSigned)
		{
			if (!deferred) parameter.wireType = types.createType<awst::ARC4UIntN>(256);
		}
		else parameter.setAbiWireType(types, declaration->type());
		parameters.push_back(std::move(parameter));
	}
}

std::string ConstructorWirePlan::postInitSignature() const
{
	std::string signature = "__postInit(";
	for (size_t i = 0; i < parameters.size(); ++i)
	{
		if (i) signature += ",";
		signature += TypeCoercion::wtypeToABIName(parameters[i].wireType);
	}
	return signature + ")void";
}

ConstructorWirePlan::Expr ConstructorWirePlan::encode(
	size_t index, Expr value, awst::SourceLocation const& loc) const
{
	auto const& parameter = parameters.at(index);
	if (parameter.type == awst::WType::uint64Type()) return awst::makeItob(std::move(value), loc);
	if (!m_deferred && parameter.type == awst::WType::boolType())
		return awst::makeItob(awst::makeAsUInt64(std::move(value), loc), loc);
	if (m_deferred && parameter.wireType == awst::WType::biguintType())
		return awst::makeLeftPadToN(awst::makeAsBytes(std::move(value), loc), 64, loc);
	if (!m_deferred && (parameter.type == awst::WType::bytesType()
		|| parameter.type == awst::WType::stringType()))
		return awst::makeAsBytes(std::move(value), loc);
	value = parameter.encodeArgument(std::move(value), loc);
	auto const* encodedType = m_types.mapToARC4Type(parameter.wireType);
	if (!awst::structurallyEquivalent(value->wtype, encodedType))
		value = awst::makeARC4Encode(std::move(value), encodedType, loc);
	return awst::makeAsBytes(std::move(value), loc);
}

ConstructorWirePlan::Expr ConstructorWirePlan::decodeParameter(
	size_t index, Expr wire, awst::SourceLocation const& loc, Statements& out) const
{
	auto const& parameter = parameters.at(index);
	auto const* native = parameter.type;
	if (!awst::structurallyEquivalent(wire->wtype, native))
		wire = awst::isNumericWType(wire->wtype)
			? TypeCoercion::coerceScalar(std::move(wire), native, loc)
			: awst::makeARC4Decode(std::move(wire), native, loc);
	if (native == awst::WType::uint64Type())
		return decodeScalar(index, awst::makeItob(std::move(wire), loc), loc, out);
	if (native == awst::WType::biguintType())
	{
		// The deferred signed carrier is uint512. Check before narrowing its
		// bytes to the canonical 256-bit two's-complement Solidity value.
		if (parameter.wireType == native)
			out.push_back(awst::makeExpressionStatement(awst::makeAssert(
				awst::makeNumericCompare(wire, awst::NumericComparison::Lte,
					awst::makeBiguintConstant(solidity::u256(-1).str(), loc), loc),
				loc, "constructor integer exceeds 256 bits"), loc));
		return decodeScalar(index, awst::makeLeftPadToN(awst::makeAsBytes(std::move(wire), loc), 32, loc), loc, out);
	}
	return wire;
}

ConstructorWirePlan::Expr ConstructorWirePlan::decodeScalar(
	size_t index, Expr bytes, awst::SourceLocation const& loc, Statements& out) const
{
	auto const* type = parameters.at(index).declaration->type();
	auto integer = SolIntType::fromSol(type);
	auto word = integer && integer->isSigned
		? codec::signExtendToWord(std::move(bytes), loc)
		: awst::makeLeftPadToN(std::move(bytes), 32, loc);
	return codec::valueFromEvmWord(m_types, type, std::move(word), loc, out,
		m_validate ? codec::PaddingPolicy::Validate : codec::PaddingPolicy::Clean);
}

ConstructorWirePlan::Expr ConstructorWirePlan::decodeCreate(
	size_t index, Expr bytes, awst::SourceLocation const& loc, Statements& out) const
{
	auto const& parameter = parameters.at(index);
	auto const* native = parameter.type;
	// Retain the create reader's legacy whole-word inputs from deploy tooling;
	// generated child calls now also send an 8-byte native narrow carrier.
	if (awst::isNumericWType(native) || native == awst::WType::boolType())
	{
		bytes = awst::makeEvalOnce(std::move(bytes), loc);
		auto const* encoded = dynamic_cast<awst::ARC4UIntN const*>(parameter.wireType);
		auto width = awst::makeIntegerConstant(encoded ? encoded->n() / 8 : 8, loc);
		auto length = awst::makeLen(bytes, loc);
		out.push_back(awst::makeExpressionStatement(awst::makeAssert(
			awst::makeBoolBinOp(
				awst::makeNumericCompare(length, awst::NumericComparison::Eq, width, loc),
				awst::BinaryBooleanOperator::Or,
				awst::makeNumericCompare(length, awst::NumericComparison::Eq,
					awst::makeIntegerConstant(32, loc), loc), loc),
			loc, "invalid constructor scalar width"), loc));
		return decodeScalar(index, std::move(bytes), loc, out);
	}
	if (native == awst::WType::accountType() || native == awst::WType::bytesType()
		|| native == awst::WType::stringType() || awst::fixedBytesLength(native))
		return awst::makeReinterpretCast(std::move(bytes), native, loc);
	return decodeParameter(index,
		awst::makeReinterpretCast(std::move(bytes), parameter.wireType, loc), loc, out);
}

} // namespace puyasol::builder
