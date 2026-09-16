#include "builder/types/ConversionPlan.h"
#include "builder/types/TypeCoercion.h"
#include "builder/types/SolIntType.h"
#include "awst/TupleValue.h"

#include <libsolidity/ast/Types.h>

namespace puyasol::builder
{

std::shared_ptr<awst::Expression> ConversionPlan::emit(
	std::shared_ptr<awst::Expression> _value,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>* _pre) const
{
	if (!_value) return nullptr;
	if (m_context == Context::ExplicitInteger)
	{
		auto const target = SolIntType::fromSol(m_target).value();
		auto const source = SolIntType::fromSolOrEnum(m_source);
		// Fixed bytes and addresses carry their numeric magnitude in bytes;
		// use biguint first so a wide source never reaches AVM's 8-byte btoi.
		if (_value->wtype == awst::WType::accountType()
			|| (_value->wtype && _value->wtype->kind() == awst::WTypeKind::Bytes))
			_value = awst::makeAsBiguint(std::move(_value), _loc);
		_value = TypeCoercion::coerceScalar(std::move(_value), m_targetRepresentation, _loc);
		bool const wide = m_targetRepresentation == awst::WType::biguintType();
		if (source && source->isSigned && source->bits < target.bits)
			return wide ? TypeCoercion::signExtendToUint256(std::move(_value), source->bits, _loc)
				: TypeCoercion::signExtendToUint64(std::move(_value), source->bits, _loc);
		if (target.isSigned)
			return wide ? TypeCoercion::signExtendToUint256(std::move(_value), target.bits, _loc)
				: TypeCoercion::signExtendToUint64(std::move(_value), target.bits, _loc);
		// Solc's source width proves that an unsigned widening needs no mask.
		if (!source || source->isSigned || source->bits > target.bits)
		{
			if (wide && target.bits < 256)
				return TypeCoercion::maskUnsignedToWidth(std::move(_value), target.bits, _loc);
			if (!wide && target.bits < 64)
				return awst::makeUInt64BinOp(std::move(_value), awst::UInt64BinaryOperator::BitAnd,
					awst::makeIntegerConstant((uint64_t{1} << target.bits) - 1, _loc), _loc);
		}
		return _value;
	}
	char const* site = "implicit conversion";
	switch (m_context)
	{
	case Context::Assignment: site = "assignment"; break;
	case Context::Initialization: site = "variable-declaration init"; break;
	case Context::Argument: site = "internal-call arg"; break;
	case Context::Return: site = "return"; break;
	case Context::AbiArgument: site = "ABI-call arg"; break;
	case Context::AbiReinterpret: site = "untyped ABI payload adaptation"; break;
	case Context::ExplicitInteger: break; // handled above; not an implicit conversion
	}
	auto const* sourceType = m_source;
	auto const* targetType = m_target;
	if (m_context == Context::AbiReinterpret)
	{
		// Only the untyped encodeWithSelector/Signature self-call adapter
		// crosses nominal UDVT boundaries by representation, not assignment.
		auto underlying = [](solidity::frontend::Type const* type) {
			if (auto const* udvt = dynamic_cast<solidity::frontend::UserDefinedValueType const*>(type))
				return &udvt->underlyingType();
			return type;
		};
		sourceType = underlying(sourceType);
		targetType = underlying(targetType);
	}
	TypeCoercion::assertImplicitlyConvertible(sourceType, targetType, _loc, site);
	// A tuple snapshot can hide the literal node, but solc still gives its
	// exact byte count. Preserve the value's evaluation and pad using that fact.
	if (auto const* literal = dynamic_cast<solidity::frontend::StringLiteralType const*>(sourceType))
		if (auto const* bytes = dynamic_cast<solidity::frontend::FixedBytesType const*>(targetType);
			bytes && literal->value().size() <= bytes->numBytes())
			_value = awst::makeReinterpretCast(awst::makeRightPad(
				awst::makeAsBytes(std::move(_value), _loc),
				static_cast<int>(bytes->numBytes() - literal->value().size()), _loc),
				m_targetRepresentation, _loc);
	// Solc gives each tuple component its own conversion. Representation-only
	// casts lose signed widening, including when the tuple is returned by a call.
	if (auto const* target = dynamic_cast<solidity::frontend::TupleType const*>(m_target))
	{
		if (target->components().empty()) return _value;
		auto const* source = dynamic_cast<solidity::frontend::TupleType const*>(m_source);
		auto const* sourceW = dynamic_cast<awst::WTuple const*>(_value->wtype);
		auto const* targetW = dynamic_cast<awst::WTuple const*>(m_targetRepresentation);
		assert(source && sourceW && targetW);
		assert(source->components().size() == target->components().size());
		assert(sourceW->types().size() == targetW->types().size());
		auto items = awst::tupleItems(std::move(_value), _loc, _pre);
		auto result = awst::makeTupleExpression(targetW, _loc);
		for (size_t i = 0; i < target->components().size(); ++i)
		{
			auto item = std::move(items[i]);
			result->items.push_back(ConversionPlan{source->components()[i],
				target->components()[i], targetW->types()[i], m_context}.emit(std::move(item), _loc, _pre));
		}
		return result;
	}
	_value = TypeCoercion::coerceForAssignment(
		std::move(_value), m_targetRepresentation, _loc, _pre);
	if (m_context == Context::AbiArgument)
		if (auto const* integer = dynamic_cast<solidity::frontend::IntegerType const*>(targetType))
			_value = TypeCoercion::coerceToCommonInt(
				std::move(_value), integer, m_targetRepresentation, _loc);
	if (m_context == Context::AbiArgument || m_context == Context::AbiReinterpret)
		if (auto const* enumeration = dynamic_cast<solidity::frontend::EnumType const*>(targetType))
		{
			// solc's enum ABI cleanup validates even an unused callee argument.
			// CheckedMaybe keeps the assert in expression-only conversion sites.
			static awst::WTuple checkedType({awst::WType::uint64Type(), awst::WType::boolType()});
			auto value = awst::makeEvalOnce(std::move(_value), _loc);
			auto pair = awst::makeTupleExpression(&checkedType, _loc);
			pair->items = {value, awst::makeNumericCompare(value, awst::NumericComparison::Lt,
				awst::makeIntegerConstant(enumeration->numberOfMembers(), _loc), _loc)};
			auto checked = awst::makeNode<awst::CheckedMaybe>(_loc, m_targetRepresentation);
			checked->expr = std::move(pair);
			checked->comment = "enum ABI argument out of range";
			_value = std::move(checked);
		}
	return TypeCoercion::signExtendSignedWiden(
		std::move(_value), sourceType, targetType, _loc);
}

} // namespace puyasol::builder
