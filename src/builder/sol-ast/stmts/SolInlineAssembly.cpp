/// @file SolInlineAssembly.cpp
/// Migrated from InlineAssemblyBuilder.cpp.

#include "builder/sol-ast/stmts/SolInlineAssembly.h"
#include "builder/ExpressionBuilder.h"
#include "builder/assembly/AssemblyBuilder.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/storage/StorageLayout.h"
#include "builder/storage/TransientStorage.h"
#include "Logger.h"

#include <libsolidity/ast/Types.h>
#include <libsolutil/Numeric.h>

namespace
{
solidity::frontend::IntegerType const* resolveIntegerType(solidity::frontend::Type const* _type)
{
	if (!_type) return nullptr;
	if (auto const* intType = dynamic_cast<solidity::frontend::IntegerType const*>(_type))
		return intType;
	if (auto const* udvt = dynamic_cast<solidity::frontend::UserDefinedValueType const*>(_type))
		return dynamic_cast<solidity::frontend::IntegerType const*>(&udvt->underlyingType());
	return nullptr;
}

/// Recursively resolve a constant variable's value to a u256 integer.
/// Follows reference chains (e.g., `uint constant aa = a;` where `a = 2`).
/// @param _varDecl  The constant VariableDeclaration to resolve.
/// @return The resolved u256 value, or nullopt if unresolvable.
std::optional<solidity::u256> resolveConstantU256(
	solidity::frontend::VariableDeclaration const& _varDecl,
	int _depth = 0)
{
	using namespace solidity::frontend;
	if (_depth > 10 || !_varDecl.isConstant() || !_varDecl.value())
		return std::nullopt;

	auto const* initExpr = _varDecl.value().get();
	auto const* exprType = initExpr->annotation().type;

	// 1. RationalNumberType (numeric literals and compile-time constant expressions)
	if (auto const* ratType = dynamic_cast<RationalNumberType const*>(exprType))
	{
		if (!ratType->isFractional())
		{
			auto const& val = ratType->value();
			solidity::u256 intVal = solidity::u256(val.numerator() / val.denominator());
			// For FixedBytesType (bytesN), left-shift to match EVM representation
			if (auto const* fixedBytes = dynamic_cast<FixedBytesType const*>(_varDecl.type()))
			{
				size_t shiftBits = (32 - fixedBytes->numBytes()) * 8;
				intVal <<= shiftBits;
			}
			return intVal;
		}
	}

	// 2. Bool constants: true → 1, false → 0
	if (dynamic_cast<BoolType const*>(exprType))
	{
		if (auto const* literal = dynamic_cast<Literal const*>(initExpr))
			return (literal->value() == "true") ? solidity::u256(1) : solidity::u256(0);
	}

	// 3. Literal (hex or string)
	if (auto const* literal = dynamic_cast<Literal const*>(initExpr))
	{
		std::string value = literal->value();
		if (value.size() > 2 && value.substr(0, 2) == "0x")
		{
			try
			{
				return solidity::u256(value);
			}
			catch (...) {}
		}
		else
		{
			solidity::u256 numVal = 0;
			for (char ch: value)
				numVal = (numVal << 8) | static_cast<unsigned char>(ch);
			size_t shiftBits = (32 - value.size()) * 8;
			numVal <<= shiftBits;
			return numVal;
		}
	}

	// 4. Identifier referencing another constant — follow the chain
	if (auto const* identifier = dynamic_cast<Identifier const*>(initExpr))
	{
		if (auto const* refDecl = dynamic_cast<VariableDeclaration const*>(
				identifier->annotation().referencedDeclaration))
			return resolveConstantU256(*refDecl, _depth + 1);
	}

	return std::nullopt;
}
}

namespace puyasol::builder::sol_ast
{

using namespace solidity::frontend;

SolInlineAssembly::SolInlineAssembly(
	StatementContext& _ctx,
	InlineAssembly const& _node,
	awst::SourceLocation _loc,
	std::vector<std::pair<std::string, awst::WType const*>> const& _functionParams,
	awst::WType const* _returnType,
	std::map<std::string, unsigned> const& _functionParamBitWidths)
	: SolStatement(_ctx, std::move(_loc)), m_node(_node),
	  m_functionParams(_functionParams), m_returnType(_returnType),
	  m_functionParamBitWidths(_functionParamBitWidths)
{
}

std::vector<std::shared_ptr<awst::Statement>> SolInlineAssembly::toAwst()
{
	Logger::instance().debug("translating inline assembly block", m_loc);

	std::string contextName = m_ctx.sourceFile;
	auto lastDot = contextName.rfind('.');
	if (lastDot != std::string::npos)
		contextName = contextName.substr(0, lastDot);
	auto lastSlash = contextName.rfind('/');
	if (lastSlash != std::string::npos)
		contextName = contextName.substr(lastSlash + 1);

	// Extract constant values from external references
	std::map<std::string, std::string> constants;
	auto const& annotation = m_node.annotation();
	for (auto const& [yulId, extInfo]: annotation.externalReferences)
	{
		if (!extInfo.declaration) continue;
		auto const* varDecl = dynamic_cast<VariableDeclaration const*>(extInfo.declaration);
		if (!varDecl || !varDecl->isConstant()) continue;

		auto resolved = resolveConstantU256(*varDecl);
		if (resolved)
		{
			std::ostringstream oss;
			oss << *resolved;
			constants[yulId->name.str()] = oss.str();
		}
	}

	// Extract storage slot/offset references using StorageLayout.
	// Computes EVM-compatible (slot, offset) pairs for state variables.
	std::map<std::string, std::string> storageSlotVars;
	{
		// Find the contract from any state variable reference
		ContractDefinition const* contractDef = nullptr;
		for (auto const& [yulId, extInfo]: annotation.externalReferences)
		{
			if (!extInfo.declaration) continue;
			auto const* varDecl = dynamic_cast<VariableDeclaration const*>(extInfo.declaration);
			if (varDecl && varDecl->isStateVariable() && !varDecl->isConstant())
			{
				contractDef = dynamic_cast<ContractDefinition const*>(varDecl->scope());
				break;
			}
		}

		if (contractDef && m_ctx.typeMapper)
		{
			StorageLayout layout;
			layout.computeLayout(*contractDef, *m_ctx.typeMapper);

			for (auto const& [yulId, extInfo]: annotation.externalReferences)
			{
				if (!extInfo.declaration) continue;
				auto const* varDecl = dynamic_cast<VariableDeclaration const*>(extInfo.declaration);
				if (!varDecl || varDecl->isConstant() || !varDecl->isStateVariable()) continue;

				std::string yulName = yulId->name.str();
				std::string suffix = extInfo.suffix;

				// Transient vars live in a separate slot namespace; resolve
				// via TransientStorage, which mirrors the packed layout but
				// with its own slot numbering (EIP-1153).
				bool isTransient = varDecl->referenceLocation() == VariableDeclaration::Location::Transient;
				unsigned slotNum = 0, byteOffset = 0;
				bool resolved = false;
				if (isTransient)
				{
					auto* ts = m_ctx.exprBuilder ? m_ctx.exprBuilder->transientStorage() : nullptr;
					if (ts)
					{
						if (auto const* tv = ts->getVarInfo(varDecl->name()))
						{
							slotNum = tv->slot;
							byteOffset = tv->byteOffset;
							resolved = true;
						}
					}
				}
				else
				{
					if (auto const* varInfo = layout.getVarInfo(varDecl->name()))
					{
						slotNum = varInfo->slot;
						byteOffset = varInfo->byteOffset;
						resolved = true;
					}
				}
				if (!resolved) continue;

				if (suffix == "slot")
				{
					constants[yulName] = std::to_string(slotNum);
					storageSlotVars[yulName] = varDecl->name();
				}
				else if (suffix == "offset")
				{
					constants[yulName] = std::to_string(byteOffset);
				}
			}
		}
	}

	// Build augmented params
	auto augmentedParams = m_functionParams;
	std::map<std::string, unsigned> paramBitWidths;
	for (auto const& [yulId, extInfo]: annotation.externalReferences)
	{
		if (!extInfo.declaration) continue;
		auto const* varDecl = dynamic_cast<VariableDeclaration const*>(extInfo.declaration);
		if (!varDecl || varDecl->isConstant()) continue;

		std::string name = yulId->name.str();
		bool found = false;
		for (auto const& [pName, pType]: augmentedParams)
			if (pName == name) found = true;
		if (!found)
		{
			auto* type = m_ctx.typeMapper->map(varDecl->type());
			augmentedParams.emplace_back(name, type);
		}

		if (auto const* intType = resolveIntegerType(varDecl->annotation().type))
		{
			if (intType->numBits() < 64)
				paramBitWidths[name] = intType->numBits();
		}
	}
	for (auto const& [n, bw]: m_functionParamBitWidths)
		paramBitWidths.emplace(n, bw);

	AssemblyBuilder asmTranslator((*m_ctx.typeMapper), m_ctx.sourceFile, contextName);
	return asmTranslator.buildBlock(
		m_node.operations().root(),
		augmentedParams,
		m_returnType,
		constants,
		paramBitWidths,
		storageSlotVars);
}

} // namespace puyasol::builder::sol_ast
