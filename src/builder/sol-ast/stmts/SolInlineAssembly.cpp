/// @file SolInlineAssembly.cpp
/// Migrated from InlineAssemblyBuilder.cpp.

#include "builder/sol-ast/stmts/SolInlineAssembly.h"
#include "builder/sol-eb/ContractContext.h"
#include "builder/assembly/AssemblyBuilder.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/storage/StorageLayout.h"
#include "builder/storage/TransientStorage.h"
#include "Logger.h"

#include <libsolidity/analysis/ConstantEvaluator.h>
#include <libsolidity/ast/ASTUtils.h>
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

/// Resolve a constant variable's initializer to a u256 integer using
/// solc's ConstantEvaluator. Handles literals, binary/unary ops on
/// constants, identifier-chains via solc's own recursion. The bytesN
/// left-shift below is the only AVM-side adjustment we still need.
std::optional<solidity::u256> resolveConstantU256(
	solidity::frontend::VariableDeclaration const& _varDecl)
{
	using namespace solidity::frontend;
	if (!_varDecl.isConstant() || !_varDecl.value())
		return std::nullopt;

	auto const* initExpr = _varDecl.value().get();

	// String/hex literal fast-path: ConstantEvaluator only handles
	// numeric / bool, not raw bytes/strings. For those, parse directly.
	if (auto const* literal = dynamic_cast<Literal const*>(initExpr))
	{
		auto const* exprType = initExpr->annotation().type;
		// Bool: handled by ConstantEvaluator below as rational 0/1, but
		// only for true/false keywords; bool literals always map cleanly.
		if (!dynamic_cast<RationalNumberType const*>(exprType)
			&& !dynamic_cast<BoolType const*>(exprType))
		{
			std::string const& value = literal->value();
			if (value.size() > 2 && value.substr(0, 2) == "0x")
			{
				try { return solidity::u256(value); }
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
	}

	// Numeric / bool / identifier-chain / arithmetic over constants:
	// solc's ConstantEvaluator handles all of these (including the
	// chained-const case `const aa = a;` and constant binary ops like
	// `const x = 1 << 32;`). It walks the AST itself, so we don't need
	// our own recursion + depth cap.
	auto evaluated = ConstantEvaluator::tryEvaluate(*initExpr);
	if (!std::holds_alternative<solidity::frontend::rational>(evaluated.value))
		return std::nullopt;

	auto const& rat = std::get<solidity::frontend::rational>(evaluated.value);
	if (rat.denominator() != 1)
		return std::nullopt;
	solidity::u256 intVal = solidity::u256(rat.numerator());

	// `bytes[N]` left-shift to match the EVM big-endian representation.
	if (auto const* fixedBytes = dynamic_cast<FixedBytesType const*>(_varDecl.type()))
	{
		size_t shiftBits = (32 - fixedBytes->numBytes()) * 8;
		intVal <<= shiftBits;
	}
	return intVal;
}
}

namespace puyasol::builder::sol_ast
{

using namespace solidity::frontend;

SolInlineAssembly::SolInlineAssembly(
	BlockContext& _blk,
	InlineAssembly const& _node,
	awst::SourceLocation _loc)
	: SolStatement(_blk, std::move(_loc)), m_node(_node)
{
}

std::vector<std::shared_ptr<awst::Statement>> SolInlineAssembly::toAwst()
{
	Logger::instance().debug("translating inline assembly block", m_loc);

	std::string contextName = m_blk.sourceFile();
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
		// `isConstant()` is the per-decl flag; `isConstantVariableRecursive`
		// also walks the initializer chain to confirm it bottoms out at a
		// literal — protects `resolveConstantU256` below from chasing a
		// chain that ends at a non-static expression. Two-stage call
		// because solc asserts `isConstant()` inside the recursive form.
		if (!varDecl || !varDecl->isConstant()) continue;
		if (!solidity::frontend::isConstantVariableRecursive(*varDecl)) continue;

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
				contractDef = varDecl->annotation().contract;
				break;
			}
		}

		if (contractDef)
		{
			StorageLayout layout;
			layout.computeLayout(*contractDef, m_blk.typeMapper());

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
					auto* ts = m_blk.builderCtx().transientStorage;
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
	auto augmentedParams = m_blk.fn.params;
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
			auto* type = m_blk.typeMapper().map(varDecl->type());
			augmentedParams.emplace_back(name, type);
		}

		if (auto const* intType = resolveIntegerType(varDecl->annotation().type))
		{
			if (intType->numBits() < 64)
				paramBitWidths[name] = intType->numBits();
		}
	}
	for (auto const& [n, bw]: m_blk.fn.paramBitWidths)
		paramBitWidths.emplace(n, bw);

	AssemblyBuilder asmTranslator(m_blk.typeMapper(), m_blk.sourceFile(), contextName);
	return asmTranslator.buildBlock(
		m_node.operations().root(),
		augmentedParams,
		m_blk.fn.returnType,
		constants,
		paramBitWidths,
		storageSlotVars);
}

} // namespace puyasol::builder::sol_ast
