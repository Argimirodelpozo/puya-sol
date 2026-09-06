#include "builder/sol-types/ConversionPlan.h"
#include "builder/sol-types/TypeCoercion.h"
#include "builder/sol-types/SolIntType.h"

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
		_value = TypeCoercion::implicitNumericCast(std::move(_value), m_targetRepresentation, _loc);
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
	case Context::ExplicitInteger: break; // handled above; not an implicit conversion
	}
	TypeCoercion::assertImplicitlyConvertible(m_source, m_target, _loc, site);
	_value = TypeCoercion::coerceForAssignment(
		std::move(_value), m_targetRepresentation, _loc, _pre);
	return TypeCoercion::signExtendSignedWiden(
		std::move(_value), m_source, m_target, _loc);
}

} // namespace puyasol::builder
