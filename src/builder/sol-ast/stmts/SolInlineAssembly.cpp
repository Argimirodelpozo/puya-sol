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

	// Literal fast-path. Two cases ConstantEvaluator can't or won't fold:
	//   - hex literals typed as address/bytesN/etc — TypeProvider::forLiteral
	//     returns AddressType / FixedBytesType, and constantToTypedValue
	//     bails on anything that isn't RationalNumberType/StringLiteralType.
	//     So `address constant e = 0x12…12;` yields a monostate value out of
	//     ConstantEvaluator, and `e` falls through to a VarExpression.
	//   - non-hex string literals — packed left-aligned into a 32-byte word
	//     using EVM's bytesN convention.
	auto applyBytesNShift = [&](solidity::u256 _val) -> solidity::u256 {
		if (auto const* fixedBytes = dynamic_cast<FixedBytesType const*>(_varDecl.type()))
		{
			size_t shiftBits = (32 - fixedBytes->numBytes()) * 8;
			_val <<= shiftBits;
		}
		return _val;
	};

	if (auto const* literal = dynamic_cast<Literal const*>(initExpr))
	{
		std::string const& value = literal->value();
		auto const* exprType = initExpr->annotation().type;
		// Bool: ConstantEvaluator's constantToTypedValue handles only
		// RationalNumberType and StringLiteralType, so `bool constant d =
		// true;` falls through to monostate. Map true/false directly.
		if (dynamic_cast<BoolType const*>(exprType))
			return value == "true" ? solidity::u256(1) : solidity::u256(0);
		if (value.size() > 2 && value.substr(0, 2) == "0x")
		{
			try { return applyBytesNShift(solidity::u256(value)); }
			catch (...) {}
		}
		else if (!dynamic_cast<RationalNumberType const*>(exprType))
		{
			// String literal: pack as bytesN (left-aligned). Skip for
			// rational — those are simple numeric values (e.g. "2") that
			// ConstantEvaluator handles correctly below.
			solidity::u256 numVal = 0;
			for (char ch: value)
				numVal = (numVal << 8) | static_cast<unsigned char>(ch);
			size_t shiftBits = (32 - value.size()) * 8;
			numVal <<= shiftBits;
			return numVal;
		}
	}

	// Chained constant: `const bb = b;` where b's value is itself a
	// non-rational literal (address/bytesN/bool) that ConstantEvaluator
	// can't fold to a rational. Recurse via this function so the literal
	// fast-path above kicks in for the leaf, then re-apply the bytesN
	// shift for the outer declaration.
	if (auto const* identifier = dynamic_cast<Identifier const*>(initExpr))
	{
		if (auto const* refDecl = dynamic_cast<VariableDeclaration const*>(
				identifier->annotation().referencedDeclaration))
		{
			if (refDecl->isConstant())
			{
				auto inner = resolveConstantU256(*refDecl);
				if (inner)
				{
					// Strip the inner bytesN shift (if any) before re-applying
					// the outer one — otherwise chains like `bytes3 cc = c;
					// bytes3 ccc = cc;` would shift twice.
					if (auto const* innerFixedBytes =
						dynamic_cast<FixedBytesType const*>(refDecl->type()))
					{
						size_t innerShift = (32 - innerFixedBytes->numBytes()) * 8;
						*inner >>= innerShift;
					}
					return applyBytesNShift(*inner);
				}
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
		if (!varDecl || !varDecl->isConstant()) continue;
		// Skip cyclic constant chains (`const a = b; const b = a;`) — solc's
		// isConstantVariableRecursive returns true for cycles. Without this
		// guard, resolveConstantU256 → ConstantEvaluator could recurse into
		// the cycle.
		if (solidity::frontend::isConstantVariableRecursive(*varDecl)) continue;

		auto resolved = resolveConstantU256(*varDecl);
		if (resolved)
		{
			std::ostringstream oss;
			oss << *resolved;
			constants[yulId->name.str()] = oss.str();
		}
	}

	// Build a map of local storage aliases: local_var_name → state_var_declaration.
	// Needed because `uint256[] storage x = a;` stores the initializer in the
	// VariableDeclarationStatement, NOT in VariableDeclaration::value().
	// For each external reference that is a local variable with a .slot suffix,
	// find its scope block and walk its statements to find the initialiser.
	std::map<std::string, VariableDeclaration const*> storageLocalAliases;
	{
		for (auto const& [yulId, extInfo]: annotation.externalReferences)
		{
			if (extInfo.suffix != "slot") continue;
			auto const* varDecl = dynamic_cast<VariableDeclaration const*>(extInfo.declaration);
			if (!varDecl || !varDecl->isLocalVariable()) continue;

			// Walk the scope block containing this local var to find its initialiser.
			auto const* block = dynamic_cast<Block const*>(varDecl->scope());
			if (!block) continue;

			for (auto const& stmt: block->statements())
			{
				auto const* vds = dynamic_cast<VariableDeclarationStatement const*>(stmt.get());
				if (!vds || !vds->initialValue()) continue;
				bool declaresVar = false;
				for (auto const& vd: vds->declarations())
					if (vd && vd->name() == varDecl->name()) declaresVar = true;
				if (!declaresVar) continue;

				// Found the declaration statement. Extract the initialiser.
				auto const* initId = dynamic_cast<Identifier const*>(vds->initialValue());
				if (!initId) break;
				auto const* sv = dynamic_cast<VariableDeclaration const*>(
					initId->annotation().referencedDeclaration);
				if (sv && sv->isStateVariable())
					storageLocalAliases[varDecl->name()] = sv;
				break;
			}
		}
	}

	// Extract storage slot/offset references using StorageLayout.
	// Computes EVM-compatible (slot, offset) pairs for state variables.
	std::map<std::string, std::string> storageSlotVars;
	// Box-keyed struct storage pointers surfaced via `.slot` (a struct-in-box
	// alias such as `TickInfo storage info = self.ticks[tick]`). Distinct from
	// storageSlotVars (numeric EVM slots) — keyed on the dotted yul name.
	// Resolved from the storage-alias registry (ScopeState), independent of the
	// state-var StorageLayout below: these aliases live in a box, not an EVM
	// slot, and appear in library functions that reference no state variable
	// (so the `if (contractDef)` block below never runs for them).
	std::map<std::string, AssemblyBuilder::BoxKeyedSlot> boxKeyedStructSlots;
	for (auto const& [yulId, extInfo]: annotation.externalReferences)
	{
		if (extInfo.suffix != "slot" || !extInfo.declaration) continue;
		auto const* varDecl = dynamic_cast<VariableDeclaration const*>(extInfo.declaration);
		if (!varDecl || !varDecl->isLocalVariable()) continue;
		auto const* alias = m_blk.findStorageAlias(varDecl->id());
		if (!alias || !alias->expr || alias->kind != StorageAlias::Kind::StateRead)
			continue;
		awst::Expression const* e = alias->expr.get();
		if (auto const* sg = dynamic_cast<awst::StateGet const*>(e))
			e = sg->field.get();
		if (auto const* boxv = dynamic_cast<awst::BoxValueExpression const*>(e))
			if (dynamic_cast<awst::ARC4Struct const*>(boxv->wtype))
				boxKeyedStructSlots[yulId->name.str()] = {boxv->key, boxv->wtype};
	}
	{
		// Prefer the currently-being-built contract — its layout reflects the
		// derived class's `layout at N` annotation and inherited-var ordering.
		// Falls back to the declaring contract of any referenced state var
		// (which is correct for free functions / libraries).
		ContractDefinition const* contractDef = m_blk.builderCtx().currentContract;
		if (!contractDef)
		{
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
		}

		if (contractDef)
		{
			StorageLayout layout;
			layout.computeLayout(*contractDef, m_blk.typeMapper());

			for (auto const& [yulId, extInfo]: annotation.externalReferences)
			{
				if (!extInfo.declaration) continue;
				auto const* varDecl = dynamic_cast<VariableDeclaration const*>(extInfo.declaration);
				if (!varDecl || varDecl->isConstant()) continue;

				// For local storage references (`uint256[] storage x = a`), the
				// declaration is the local var, not the state var. Follow the
				// initial value to find the underlying state var.
				if (!varDecl->isStateVariable())
				{
					if (!varDecl->isLocalVariable()) continue;
					// Local storage references: `uint256[] storage x = a;`
					// The initialiser lives in the parent VariableDeclarationStatement,
					// not in VariableDeclaration::value(). We must look it up from the
					// storageLocalAliases map built by traversing the function body.
					auto aliasIt = storageLocalAliases.find(varDecl->name());
					if (aliasIt == storageLocalAliases.end()) continue;
					VariableDeclaration const* sv = aliasIt->second;
					if (!sv || !sv->isStateVariable()) continue;
					varDecl = sv;
				}

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
		storageSlotVars,
		boxKeyedStructSlots);
}

} // namespace puyasol::builder::sol_ast
