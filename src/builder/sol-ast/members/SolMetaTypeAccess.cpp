/// @file SolMetaTypeAccess.cpp
/// type(X).max/min/name/interfaceId.
/// Migrated from MemberAccessBuilder.cpp lines 380-687.

#include "builder/sol-ast/members/SolMetaTypeAccess.h"
#include "builder/ExpressionUtils.h"
#include "builder/sol-types/TypeMapper.h"

#include <libsolidity/ast/AST.h>
#include <sstream>

namespace puyasol::builder::sol_ast
{

using namespace solidity::frontend;

std::shared_ptr<awst::Expression> SolMetaTypeAccess::toAwst()
{
	std::string member = memberName();
	auto const* baseType = baseExpression().annotation().type;

	// Extract type argument from MagicType or TypeType
	Type const* typeArg = nullptr;
	if (auto const* magicType = dynamic_cast<MagicType const*>(baseType))
		typeArg = magicType->typeArgument();
	else if (auto const* typeType = dynamic_cast<TypeType const*>(baseType))
		typeArg = typeType->actualType();

	// type(uintN).max / type(uintN).min
	if (member == "max" || member == "min")
	{
		if (auto const* intType = dynamic_cast<IntegerType const*>(typeArg))
		{
			unsigned bits = intType->numBits();
			auto* wtype = (bits <= 64)
				? awst::WType::uint64Type()
				: awst::WType::biguintType();

			std::string val;
			if (member == "max")
			{
				if (intType->isSigned())
				{
					// type(intN).max = 2^(N-1) - 1
					solidity::u256 maxVal = (solidity::u256(1) << (bits - 1)) - 1;
					std::ostringstream oss;
					oss << maxVal;
					val = oss.str();
				}
				else
					val = puyasol::builder::maxUintValue(bits);
			}
			else
			{
				if (intType->isSigned())
				{
					// type(intN).min = 2^(N-1) in two's complement biguint
					solidity::u256 minVal = solidity::u256(1) << (bits - 1);
					std::ostringstream oss;
					oss << minVal;
					val = oss.str();
				}
				else
					val = "0";
			}

			auto e = std::make_shared<awst::IntegerConstant>();
			e->sourceLocation = m_loc;
			e->wtype = wtype;
			e->value = val;
			return e;
		}

		// type(EnumType).max / .min
		if (auto const* enumType = dynamic_cast<EnumType const*>(typeArg))
		{
			auto e = std::make_shared<awst::IntegerConstant>();
			e->sourceLocation = m_loc;
			e->wtype = awst::WType::uint64Type();
			e->value = (member == "max")
				? std::to_string(enumType->numberOfMembers() - 1)
				: std::string("0");
			return e;
		}
	}

	// type(C).name → contract name as string
	if (member == "name" && typeArg)
	{
		std::string typeName;
		if (auto const* ct = dynamic_cast<ContractType const*>(typeArg))
			typeName = ct->contractDefinition().name();
		else
			typeName = typeArg->toString(true);

		auto strConst = std::make_shared<awst::BytesConstant>();
		strConst->sourceLocation = m_loc;
		strConst->wtype = awst::WType::stringType();
		strConst->encoding = awst::BytesEncoding::Utf8;
		strConst->value = std::vector<uint8_t>(typeName.begin(), typeName.end());
		return strConst;
	}

	// type(Interface).interfaceId → bytes4 ERC165 ID
	if (member == "interfaceId")
	{
		uint32_t interfaceIdValue = 0;
		if (auto const* contractType = dynamic_cast<ContractType const*>(typeArg))
			interfaceIdValue = contractType->contractDefinition().interfaceId();

		auto e = std::make_shared<awst::BytesConstant>();
		e->sourceLocation = m_loc;
		e->wtype = m_ctx.typeMapper.map(m_memberAccess.annotation().type);
		e->encoding = awst::BytesEncoding::Base16;
		e->value = {
			static_cast<uint8_t>((interfaceIdValue >> 24) & 0xFF),
			static_cast<uint8_t>((interfaceIdValue >> 16) & 0xFF),
			static_cast<uint8_t>((interfaceIdValue >> 8) & 0xFF),
			static_cast<uint8_t>(interfaceIdValue & 0xFF)
		};
		return e;
	}

	return nullptr;
}

} // namespace puyasol::builder::sol_ast
