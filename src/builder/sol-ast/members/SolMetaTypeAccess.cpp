/// @file SolMetaTypeAccess.cpp
/// type(X).max/min/name/interfaceId.
/// Migrated from MemberAccessBuilder.cpp lines 380-687.

#include "builder/sol-ast/members/SolMetaTypeAccess.h"
#include "builder/SelectorSemantics.h"
#include "builder/EvmFeaturePolicy.h"
#include "builder/SolcFacts.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/sol-types/TypeCoercion.h"
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
			// solc already computes both bounds as a 256-bit two's-complement u256
			// (min() = s2u(minValue()), max() = the signed/unsigned max). Route through
			// the shared canonicaliser for the right slot width + wtype — replaces the
			// hand-rolled TC math that had to stay in lockstep with SolLiteral.
			solidity::u256 tc = (member == "max") ? intType->max() : intType->min();
			return builder::TypeCoercion::canonicalIntConstant(tc, intType->numBits(), m_loc);
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

	// EVM bytecode introspection has no AVM meaning: the deployed program is
	// TEAL, so any answer here — including solc's real object — describes a
	// contract that does not exist on chain. Hard error via the central policy.
	if (member == "creationCode" || member == "runtimeCode")
	{
		builder::EvmFeaturePolicy::report(
			member == "creationCode" ? builder::EvmFeature::CreationCode
				: builder::EvmFeature::RuntimeCode,
			m_ctx.typeMapper.profile(), m_loc);
		return awst::makeBytesConstant({}, m_loc);
	}

	// type(I).interfaceId uses solc's EIP-165 value under --evm-selectors.
	// Compatibility mode XORs ARC-4 MethodConstants so interface IDs remain
	// consistent with the selector values exposed by that mode.
	if (member == "interfaceId")
	{
		auto* targetType = m_ctx.typeMapper.map(m_memberAccess.annotation().type);
		if (auto const* contractType = dynamic_cast<ContractType const*>(typeArg))
		{
			if (builder::SelectorSemantics::enabled(m_ctx.typeMapper))
				return awst::makeBytesConstant(
					builder::SolcFacts::interfaceId(
						contractType->contractDefinition()),
					m_loc, awst::BytesEncoding::Base16, targetType);

			std::shared_ptr<awst::Expression> acc;
			// interfaceFunctionList(false) = own functions only, no inherited (mirrors solc).
			for (auto const& it: contractType->contractDefinition().interfaceFunctionList(false))
			{
				// ARC-4 selector so the XOR matches the XOR of f.selector values.
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
