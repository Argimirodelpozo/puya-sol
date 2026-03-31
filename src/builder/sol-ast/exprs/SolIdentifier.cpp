/// @file SolIdentifier.cpp
/// Variable/constant/state variable resolution.
/// Migrated from IdentifierBuilder.cpp.

#include "builder/sol-ast/exprs/SolIdentifier.h"
#include "builder/storage/StorageMapper.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/sol-types/TypeCoercion.h"

#include <libsolidity/ast/AST.h>

namespace puyasol::builder::sol_ast
{

using namespace solidity::frontend;

SolIdentifier::SolIdentifier(
	eb::BuilderContext& _ctx,
	Identifier const& _node)
	: SolExpression(_ctx, _node), m_ident(_node)
{
}

std::shared_ptr<awst::Expression> SolIdentifier::toAwst()
{
	std::string name = m_ident.name();

	// 'this' → global CurrentApplicationAddress
	if (name == "this")
	{
		auto call = std::make_shared<awst::IntrinsicCall>();
		call->sourceLocation = m_loc;
		call->opCode = "global";
		call->immediates = {std::string("CurrentApplicationAddress")};
		call->wtype = awst::WType::accountType();
		return call;
	}

	auto const* decl = m_ident.annotation().referencedDeclaration;
	if (decl)
	{
		// Parameter remaps (modifier parameters)
		auto remapIt = m_ctx.paramRemaps.find(decl->id());
		if (remapIt != m_ctx.paramRemaps.end())
		{
			auto var = std::make_shared<awst::VarExpression>();
			var->sourceLocation = m_loc;
			var->name = remapIt->second.name;
			var->wtype = remapIt->second.type;
			return var;
		}

		// Storage pointer aliases
		auto aliasIt = m_ctx.storageAliases.find(decl->id());
		if (aliasIt != m_ctx.storageAliases.end())
			return aliasIt->second;
	}

	// Variable references
	if (auto const* varDecl = dynamic_cast<VariableDeclaration const*>(decl))
	{
		// Constants/immutables: inline the value
		if ((varDecl->isConstant() || (varDecl->immutable() && !m_ctx.inConstructor))
			&& varDecl->value())
		{
			auto val = buildExpr(*varDecl->value());

			// bytes[N] constant from integer literal → BytesConstant
			auto* targetType = m_ctx.typeMapper.map(varDecl->type());
			if (auto const* bytesType = dynamic_cast<awst::BytesWType const*>(targetType))
			{
				if (auto* intConst = dynamic_cast<awst::IntegerConstant*>(val.get()))
				{
					int len = bytesType->length().value_or(0);
					std::vector<unsigned char> bytes(static_cast<size_t>(len), 0);
					std::string numStr = intConst->value;
					std::vector<unsigned char> bignum;
					for (char c : numStr)
					{
						int digit = c - '0';
						int carry = digit;
						for (auto& b : bignum)
						{
							int v = b * 10 + carry;
							b = static_cast<unsigned char>(v & 0xFF);
							carry = v >> 8;
						}
						while (carry > 0)
						{
							bignum.push_back(static_cast<unsigned char>(carry & 0xFF));
							carry >>= 8;
						}
					}
					for (size_t i = 0; i < bignum.size() && i < bytes.size(); ++i)
						bytes[bytes.size() - 1 - i] = bignum[i];

					auto bc = std::make_shared<awst::BytesConstant>();
					bc->sourceLocation = val->sourceLocation;
					bc->wtype = targetType;
					bc->encoding = awst::BytesEncoding::Base16;
					bc->value = std::move(bytes);
					return bc;
				}
			}
			// String → bytes[N] right-pad
			if (auto const* bwt = dynamic_cast<awst::BytesWType const*>(targetType))
			{
				if (bwt->length().has_value() && *bwt->length() > 0)
				{
					if (auto padded = builder::TypeCoercion::stringToBytesN(
							val.get(), targetType, *bwt->length(), val->sourceLocation))
						return padded;
				}
			}
			// String → bytes cast
			if (targetType == awst::WType::bytesType()
				&& val->wtype == awst::WType::stringType())
			{
				auto cast = std::make_shared<awst::ReinterpretCast>();
				cast->sourceLocation = val->sourceLocation;
				cast->wtype = awst::WType::bytesType();
				cast->expr = std::move(val);
				return cast;
			}
			return val;
		}

		if (varDecl->isStateVariable())
		{
			auto* type = m_ctx.typeMapper.map(varDecl->type());
			auto kind = builder::StorageMapper::shouldUseBoxStorage(*varDecl)
				? awst::AppStorageKind::Box
				: awst::AppStorageKind::AppGlobal;

			// Dynamic arrays in box storage: placeholder
			if (type && type->kind() == awst::WTypeKind::ReferenceArray
				&& kind == awst::AppStorageKind::Box)
			{
				auto placeholder = std::make_shared<awst::VarExpression>();
				placeholder->sourceLocation = m_loc;
				placeholder->name = name;
				placeholder->wtype = type;
				return placeholder;
			}

			// Constants/immutables (redundant check for safety)
			if (varDecl->isConstant() || (varDecl->immutable() && !m_ctx.inConstructor))
			{
				if (varDecl->value())
					return buildExpr(*varDecl->value());
			}

			return m_ctx.storageMapper.createStateRead(name, type, kind, m_loc);
		}
	}

	// Regular local variable
	auto e = std::make_shared<awst::VarExpression>();
	e->sourceLocation = m_loc;
	e->name = name;
	if (decl)
	{
		if (auto const* vd = dynamic_cast<VariableDeclaration const*>(decl))
			e->wtype = m_ctx.typeMapper.map(vd->type());
	}
	if (!e->wtype || e->wtype == awst::WType::voidType())
	{
		if (m_solType)
			e->wtype = m_ctx.typeMapper.map(m_solType);
	}
	return e;
}

} // namespace puyasol::builder::sol_ast
