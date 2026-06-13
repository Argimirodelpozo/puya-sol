/// @file SolMetaTypeAccess.cpp
/// type(X).max/min/name/interfaceId.
/// Migrated from MemberAccessBuilder.cpp lines 380-687.

#include "builder/sol-ast/members/SolMetaTypeAccess.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/itxn/InnerCallHandlers.h"
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
				// solc's IntegerType::max() handles both signed (2^(N-1)-1)
				// and unsigned (2^N-1) and returns a u256 — same value we
				// were computing/looking up by hand.
				std::ostringstream oss;
				oss << intType->max();
				val = oss.str();
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

			auto e = awst::makeIntegerConstant(val, m_loc, wtype);
			return e;
		}

		// type(EnumType).max / .min
		if (auto const* enumType = dynamic_cast<EnumType const*>(typeArg))
		{
			auto e = awst::makeIntegerConstant((member == "max")
				? std::to_string(enumType->numberOfMembers() - 1)
				: std::string("0"), m_loc);
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

		return awst::makeUtf8BytesConstant(typeName, m_loc, awst::WType::stringType());
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
		return awst::makeBytesConstant(std::vector<uint8_t>(32, 0), m_loc);
	}

	// type(Interface).interfaceId → bytes4 ERC-165 ID. EVM Solidity bakes
	// in the XOR of keccak selectors (solc's interfaceId()); our selectors
	// are sha512_256 (routers/f.selector/encodeCall), so the ID is the XOR
	// of MethodConstants instead — `supportsInterface` implementations that
	// XOR runtime selectors against type(I).interfaceId stay consistent
	// on-chain. The byte VALUE diverges from EVM (same class as the
	// selector convention itself; see EVM_DIVERGENCE.md "Encoding model").
	if (member == "interfaceId")
	{
		auto* targetType = m_ctx.typeMapper.map(m_memberAccess.annotation().type);
		if (auto const* contractType = dynamic_cast<ContractType const*>(typeArg))
		{
			std::shared_ptr<awst::Expression> acc;
			// Mirrors solc's interfaceId(): interfaceFunctionList(false) — the
			// interface's OWN functions, inherited ones excluded.
			for (auto const& it: contractType->contractDefinition().interfaceFunctionList(false))
			{
				// ARC-4 canonical form (router selector) so the XOR equals
				// the XOR of the members' f.selector values.
				auto const* fd = dynamic_cast<solidity::frontend::FunctionDefinition const*>(
					&it.second->declaration());
				auto sig = fd
					? eb::InnerCallHandlers::buildMethodSelector(m_ctx, fd)
					: it.second->externalSignature();
				auto sel = awst::makeMethodConstant(
					sig, awst::WType::bytesType(), m_loc);
				if (!acc)
					acc = std::move(sel);
				else
					acc = awst::makeBytesBinOp(
						std::move(acc), awst::BytesBinaryOperator::BitXor,
						std::move(sel), m_loc);
			}
			if (acc)
				return awst::makeReinterpretCast(std::move(acc), targetType, m_loc);
		}
		return awst::makeBytesConstant(
			{0, 0, 0, 0}, m_loc, awst::BytesEncoding::Base16, targetType);
	}

	return nullptr;
}

} // namespace puyasol::builder::sol_ast
