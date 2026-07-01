#include "builder/sol-types/SolIntType.h"
#include "builder/sol-types/TypeCoercion.h"

#include <libsolidity/ast/Types.h>

namespace puyasol::builder
{

std::pair<std::string, std::string> SolIntType::pow2NAndHalf() const
{
	return TypeCoercion::pow2NAndHalf(bits);
}

std::optional<SolIntType> SolIntType::fromSol(solidity::frontend::Type const* _type)
{
	using namespace solidity::frontend;
	if (!_type)
		return std::nullopt;
	if (auto const* udvt = dynamic_cast<UserDefinedValueType const*>(_type))
		_type = &udvt->underlyingType();
	if (auto const* intType = dynamic_cast<IntegerType const*>(_type))
		return SolIntType{intType->numBits(), intType->isSigned()};
	return std::nullopt;
}

std::optional<SolIntType> SolIntType::fromArc4(awst::WType const* _type)
{
	auto const* arc4 = dynamic_cast<awst::ARC4UIntN const*>(_type);
	if (!arc4)
		return std::nullopt;
	return SolIntType{static_cast<unsigned>(arc4->n()), arc4->isSigned()};
}

} // namespace puyasol::builder
