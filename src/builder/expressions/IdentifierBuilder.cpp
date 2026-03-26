/// @file IdentifierBuilder.cpp
/// Handles Solidity identifier resolution (variables, constants, state vars).

#include "builder/expressions/ExpressionBuilder.h"
#include "builder/storage/StorageMapper.h"
#include "builder/sol-types/TypeCoercion.h"
#include "Logger.h"

#include <libsolidity/ast/TypeProvider.h>

#include <algorithm>
#include <sstream>

namespace puyasol::builder
{

bool ExpressionBuilder::visit(solidity::frontend::Identifier const& _node)
{
	auto loc = makeLoc(_node.location());
	std::string name = _node.name();

	// Handle 'this' keyword → global CurrentApplicationAddress
	if (name == "this")
	{
		auto call = std::make_shared<awst::IntrinsicCall>();
		call->sourceLocation = loc;
		call->opCode = "global";
		call->immediates = {std::string("CurrentApplicationAddress")};
		call->wtype = awst::WType::accountType();
		push(std::move(call));
		return false;
	}

	// Check if this identifier is a remapped modifier parameter
	auto const* decl = _node.annotation().referencedDeclaration;
	if (decl)
	{
		auto remapIt = m_paramRemaps.find(decl->id());
		if (remapIt != m_paramRemaps.end())
		{
			auto var = std::make_shared<awst::VarExpression>();
			var->sourceLocation = loc;
			var->name = remapIt->second.name;
			var->wtype = remapIt->second.type;
			push(std::move(var));
			return false;
		}

		// Check for storage pointer alias (e.g. Type storage p = _mapping[key])
		auto aliasIt = m_storageAliases.find(decl->id());
		if (aliasIt != m_storageAliases.end())
		{
			push(aliasIt->second);
			return false;
		}
	}

	// Check if this is a variable reference (state, constant, or immutable)
	if (auto const* varDecl = dynamic_cast<solidity::frontend::VariableDeclaration const*>(decl))
	{
		// File-level and contract-level constants/immutables: inline the value
		if ((varDecl->isConstant() || varDecl->immutable()) && varDecl->value())
		{
			auto val = build(*varDecl->value());
			// If the declared type is bytes[N] but the literal translated as
			// an IntegerConstant (e.g. bytes32 x = 0x00), convert to BytesConstant.
			auto* targetType = m_typeMapper.map(varDecl->type());
			if (auto const* bytesType = dynamic_cast<awst::BytesWType const*>(targetType))
			{
				if (auto* intConst = dynamic_cast<awst::IntegerConstant*>(val.get()))
				{
					int len = bytesType->length().value_or(0);
					std::vector<unsigned char> bytes(static_cast<size_t>(len), 0);
					// Parse the decimal string and convert to big-endian bytes
					std::string numStr = intConst->value;
					// Simple decimal-to-bytes: process digit by digit
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
					// bignum is little-endian; copy to big-endian bytes[]
					for (size_t i = 0; i < bignum.size() && i < bytes.size(); ++i)
						bytes[bytes.size() - 1 - i] = bignum[i];

					auto bc = std::make_shared<awst::BytesConstant>();
					bc->sourceLocation = val->sourceLocation;
					bc->wtype = targetType;
					bc->encoding = awst::BytesEncoding::Base16;
					bc->value = std::move(bytes);
					push(std::move(bc));
					return false;
				}
			}
			// String constant → bytes[N]: right-pad to N bytes
			if (auto const* bwt = dynamic_cast<awst::BytesWType const*>(targetType))
			{
				if (bwt->length().has_value() && *bwt->length() > 0)
				{
					if (auto padded = TypeCoercion::stringToBytesN(
							val.get(), targetType, *bwt->length(), val->sourceLocation))
					{
						push(std::move(padded));
						return false;
					}
				}
			}
			// If declared type is bytes but inlined value is string, cast
			if (targetType == awst::WType::bytesType()
				&& val->wtype == awst::WType::stringType())
			{
				auto cast = std::make_shared<awst::ReinterpretCast>();
				cast->sourceLocation = val->sourceLocation;
				cast->wtype = awst::WType::bytesType();
				cast->expr = std::move(val);
				push(std::move(cast));
				return false;
			}
			push(std::move(val));
			return false;
		}

		if (varDecl->isStateVariable())
		{
			auto* type = m_typeMapper.map(varDecl->type());
			auto kind = StorageMapper::shouldUseBoxStorage(*varDecl)
				? awst::AppStorageKind::Box
				: awst::AppStorageKind::AppGlobal;

			// Dynamic arrays stored as box maps: don't read the array itself,
			// element access is handled in IndexAccess. Skip creating a read.
			if (type && type->kind() == awst::WTypeKind::ReferenceArray
				&& kind == awst::AppStorageKind::Box)
			{
				// Push a placeholder — actual access happens via IndexAccess
				auto placeholder = std::make_shared<awst::VarExpression>();
				placeholder->sourceLocation = loc;
				placeholder->name = name;
				placeholder->wtype = type;
				push(placeholder);
				return false;
			}

			// Constants and immutables are handled as literal values
			if (varDecl->isConstant() || varDecl->immutable())
			{
				// Try to evaluate the constant
				if (varDecl->value())
				{
					push(build(*varDecl->value()));
					return false;
				}
			}

			push(m_storageMapper.createStateRead(name, type, kind, loc));
			return false;
		}
	}

	// Regular local variable
	auto e = std::make_shared<awst::VarExpression>();
	e->sourceLocation = loc;
	e->name = name;
	if (decl)
	{
		if (auto const* vd = dynamic_cast<solidity::frontend::VariableDeclaration const*>(decl))
			e->wtype = m_typeMapper.map(vd->type());
	}
	if (!e->wtype || e->wtype == awst::WType::voidType())
	{
		auto const* solType = _node.annotation().type;
		if (solType)
			e->wtype = m_typeMapper.map(solType);
	}
	push(e);
	return false;
}

} // namespace puyasol::builder
