/// @file SolMetaTypeAccess.cpp
/// type(X).max/min/name/interfaceId.
/// Migrated from MemberAccessBuilder.cpp lines 380-687.

#include "builder/sol-ast/members/SolMetaTypeAccess.h"
#include "builder/ExpressionUtils.h"
#include "builder/sol-types/TypeMapper.h"
#include "Logger.h"

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
			// Default slot: uint64 for ≤64 bits, biguint otherwise — matches
			// how we map integer types elsewhere.
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
					// type(intN).min = -2^(N-1). Encode as two's complement
					// in the storage slot width (uint64 for N<=64, biguint
					// for N>64), matching how SolLiteral encodes negative
					// integer literals for the same context. Without this
					// mirroring the comparison `intN_min == -2**(N-1)`
					// would have operands of different widths that never
					// reinterpret-cast to equal biguint values.
					//
					//   bits <= 64:  2^64  - 2^(bits-1)   (uint64 slot)
					//   bits  > 64:  2^256 - 2^(bits-1)   (biguint slot)
					//   bits == 256: simplifies to 2^255.
					solidity::u256 twoPowBitsMinus1 = solidity::u256(1) << (bits - 1);
					solidity::u256 minVal;
					if (bits <= 64)
					{
						// u64 wrap: 2^64 - 2^(bits-1) fits in u256 range.
						solidity::u256 twoPow64 = solidity::u256(1) << 64;
						minVal = twoPow64 - twoPowBitsMinus1;
					}
					else if (bits == 256)
						minVal = twoPowBitsMinus1;
					else
						// u256 subtraction wraps modulo 2^256, yielding
						// 2^256 - 2^(bits-1) — the 256-bit two's complement.
						minVal = solidity::u256(0) - twoPowBitsMinus1;
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

	// type(C).creationCode / type(C).runtimeCode: EVM concept of contract
	// bytecode. No direct AVM analog — contracts are deployed as AVM
	// applications and don't expose their TEAL bytecode as a value.
	// Stub as a 32-byte placeholder so Solidity patterns that only check
	// `.length > 0` or use the bytes as an opaque hash input still compile.
	// Any code that actually depends on the content of EVM bytecode will
	// silently produce wrong results.
	if (member == "creationCode" || member == "runtimeCode")
	{
		Logger::instance().warning(
			"type(C)." + member + " has no AVM analog — stubbed as 32 zero bytes;"
			" code that depends on the actual bytecode will misbehave",
			m_loc);
		auto bc = std::make_shared<awst::BytesConstant>();
		bc->sourceLocation = m_loc;
		bc->wtype = awst::WType::bytesType();
		bc->encoding = awst::BytesEncoding::Base16;
		bc->value = std::vector<uint8_t>(32, 0);
		return bc;
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
