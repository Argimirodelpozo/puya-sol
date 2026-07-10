/// @file SolInlineAssembly.cpp
/// Migrated from InlineAssemblyBuilder.cpp.

#include "builder/sol-ast/stmts/SolInlineAssembly.h"
#include "builder/sol-eb/ContractContext.h"
#include "builder/assembly/AssemblyBuilder.h"
#include "builder/sol-types/SolcConstFold.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/storage/StorageLayout.h"
#include "builder/storage/StorageMapper.h"
#include "builder/storage/TransientStorage.h"
#include "Logger.h"

#include <libsolidity/ast/ASTUtils.h>
#include <libsolidity/ast/Types.h>
#include <libsolutil/Numeric.h>

#include <algorithm>
#include "builder/sol-types/SolIntType.h"

namespace
{
// Constant-variable resolution HOISTED to the shared
// builder::SolcConstFold::constantVarEvmWord (fable-review.md item 1: one
// canonical constant-folding home) — this file now just consumes it.
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
		// guard, constantVarEvmWord → ConstantEvaluator could recurse into
		// the cycle.
		if (solidity::frontend::isConstantVariableRecursive(*varDecl)) continue;

		auto resolved = builder::SolcConstFold::constantVarEvmWord(*varDecl);
		if (resolved)
		{
			std::ostringstream oss;
			oss << *resolved;
			constants[yulId->name.str()] = oss.str();
		}
	}

	// Resolve a Solidity VariableDeclaration to its AWST name (Context::awstVarName:
	// locals → name__<declId>, params/returns bare). AssemblyBuilder names outer-var
	// refs decl-based off solc's externalReferences via this callback.
	auto declNameFn = [this](solidity::frontend::VariableDeclaration const& _vd) {
		return m_blk.awstVarName(_vd);
	};

	// local storage aliases: local name → state-var decl. `uint256[] storage x = a;`
	// keeps the initializer in the VariableDeclarationStatement, not in
	// VariableDeclaration::value() — so walk the scope block for each .slot local.
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

	// `sstore(v.slot, value)` where `v` is a scalar app-global state var:
	// route the write to `v`'s own app-global state (so a later high-level read
	// of `v` sees it) instead of the generic __dyn_storage blob. Strictly
	// app-global scalars (shouldUseBoxStorage == false); mappings/arrays/structs/
	// box vars/locals are excluded and fall through to the numeric-slot path.
	std::map<std::string, AssemblyBuilder::StateVarSlot> stateVarSlots;
	for (auto const& [yulId, extInfo]: annotation.externalReferences)
	{
		if (extInfo.suffix != "slot" || !extInfo.declaration) continue;
		auto const* varDecl = dynamic_cast<VariableDeclaration const*>(extInfo.declaration);
		if (!varDecl || !varDecl->isStateVariable() || varDecl->isConstant()) continue;
		if (builder::StorageMapper::shouldUseBoxStorage(*varDecl)) continue;
		// Restrict to FULL-WIDTH uint256 scalars: `sstore(x.slot, word)` then equals
		// writing x's value directly. Sub-word vars (uint8/int8/bytesN) are packed
		// multiple-per-slot in EVM, so `sstore(packedSlot, word)` sets several vars at
		// once and reads need masking — the route-to-one-var shortcut can't replicate
		// that (would corrupt e.g. variable_cleanup_sstore). Structs likewise (raw word
		// vs ARC4 layout). Those keep the numeric-slot/__dyn_storage path.
		auto const* intType = dynamic_cast<IntegerType const*>(varDecl->type());
		if (!intType || intType->isSigned() || intType->numBits() != 256) continue;
		stateVarSlots[yulId->name.str()] =
			{varDecl->name(), m_blk.typeMapper().map(varDecl->type())};
	}

	// Struct-storage-ref local bound from a storage-ref-returning function
	// (`Items storage ptr = Lib.get();` where get's body sets `x.slot`): puya
	// can't hold the mapping-bearing struct value, so the frontend models ptr as
	// a biguint slot handle (see SolInternalCall: storage-return + inline-asm →
	// biguint). `ptr.slot` is that handle — resolve to the local, not the struct.
	std::map<std::string, std::string> structRefSlotLocals;
	for (auto const& [yulId, extInfo]: annotation.externalReferences)
	{
		if (extInfo.suffix != "slot" || !extInfo.declaration) continue;
		auto const* ptr = dynamic_cast<VariableDeclaration const*>(extInfo.declaration);
		if (!ptr || !ptr->isLocalVariable()) continue;
		if (storageLocalAliases.count(ptr->name())) continue; // handled as state-var alias
		auto const* block = dynamic_cast<Block const*>(ptr->scope());
		if (!block) continue;
		for (auto const& stmt: block->statements())
		{
			auto const* vds = dynamic_cast<VariableDeclarationStatement const*>(stmt.get());
			if (!vds || !vds->initialValue()) continue;
			bool declaresVar = false;
			for (auto const& vd: vds->declarations())
				if (vd && vd->name() == ptr->name()) declaresVar = true;
			if (!declaresVar) continue;
			auto const* call = dynamic_cast<FunctionCall const*>(vds->initialValue());
			if (call)
			{
				auto const* fd = dynamic_cast<FunctionDefinition const*>(
					ASTNode::referencedDeclaration(call->expression()));
				if (fd && !fd->returnParameters().empty()
					&& fd->returnParameters()[0]->referenceLocation()
						== VariableDeclaration::Location::Storage
					&& fd->isImplemented()
					&& std::any_of(fd->body().statements().begin(),
						fd->body().statements().end(),
						[](auto const& s){ return dynamic_cast<InlineAssembly const*>(s.get()); }))
					structRefSlotLocals[yulId->name.str()] = ptr->name();
			}
			break;
		}
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
	// Blob-backed memory aggregates referenced in assembly resolve to their
	// uint64 memory-pointer offset (not the aggregate value).
	std::map<std::string, std::string> blobOffsetVars;
	// Base names of external refs that are DYNAMIC CALLDATA pointers (params,
	// locals, or calldata return vars). Suffixed refs (`x.offset := 0`) register
	// under the dotted name ("x.offset"), so m_locals-based detection misses the
	// base — this set carries the authoritative answer from the decl's
	// referenceLocation (calldata_assign_from_nowhere: a `bytes calldata` RETURN
	// var repointed in asm, never a real input param).
	std::set<std::string> calldataPointerNames;
	for (auto const& [yulId, extInfo]: annotation.externalReferences)
	{
		if (!extInfo.declaration) continue;
		auto const* varDecl = dynamic_cast<VariableDeclaration const*>(extInfo.declaration);
		if (!varDecl || varDecl->isConstant()) continue;

		// Key m_locals/paramBitWidths/blobOffsetVars by the SAME name the resolver
		// produces (externalRefAwstName): mangled for value-ref locals + fn-ptr
		// .selector/.address, bare for .slot/.offset/.length + state vars.
		std::string name = AssemblyBuilder::externalRefAwstName(
			extInfo, yulId->name.str(), declNameFn);
		if (varDecl->referenceLocation()
				== solidity::frontend::VariableDeclaration::Location::CallData)
		{
			std::string base = name;
			if (auto dot = base.rfind('.'); dot != std::string::npos)
			{
				std::string sfx = base.substr(dot + 1);
				if (sfx == "offset" || sfx == "length")
					base = base.substr(0, dot);
			}
			auto const* t = m_blk.typeMapper().map(varDecl->type());
			if (t == awst::WType::bytesType() || t == awst::WType::stringType()
				|| t->kind() == awst::WTypeKind::ARC4DynamicArray
				|| t->kind() == awst::WTypeKind::ReferenceArray)
				calldataPointerNames.insert(base);
		}
		if (auto blobOff = m_blk.findBlobAggregate(varDecl->id()); !blobOff.empty())
			blobOffsetVars[name] = blobOff;
		bool found = false;
		for (auto const& [pName, pType]: augmentedParams)
			if (pName == name) found = true;
		if (!found)
		{
			auto* type = m_blk.typeMapper().map(varDecl->type());
			augmentedParams.emplace_back(name, type);
		}

		if (auto it = builder::SolIntType::fromSol(varDecl->annotation().type); it && it->bits < 64)
			paramBitWidths[name] = it->bits;
	}
	for (auto const& [n, bw]: m_blk.fn.paramBitWidths)
		paramBitWidths.emplace(n, bw);

	AssemblyBuilder asmTranslator(m_blk.typeMapper(), m_blk.sourceFile(), contextName);
	asmTranslator.setFrameIsProgram(m_blk.fn.frameIsProgram);
	asmTranslator.setSeededCalldataPointers(m_blk.fn.seededCalldataPointers);
	asmTranslator.setEvmSelector(m_blk.fn.evmSelector);
	asmTranslator.setCalldataPointerNames(std::move(calldataPointerNames));
	auto stmts = asmTranslator.buildBlock(
		m_node.operations().root(),
		augmentedParams,
		m_blk.fn.returnType,
		constants,
		paramBitWidths,
		storageSlotVars,
		boxKeyedStructSlots,
		blobOffsetVars,
		structRefSlotLocals,
		stateVarSlots,
		annotation.externalReferences,
		declNameFn,
		// Only the function's own params are real calldata args; externalReferences appended
		// to augmentedParams above (return vars, outer locals) are NOT in the EVM calldata buffer.
		m_blk.fn.params.size());
	// An unconditional top-level halt (EVM return/revert → AVM program exit) makes
	// everything after this block dead — flag the enclosing block so SolBlock skips
	// the rest (puya rejects unreachable code).
	if (asmTranslator.haltEmitted())
		m_blk.terminated = true;
	return stmts;
}

} // namespace puyasol::builder::sol_ast
