/// @file SolInlineAssembly.cpp
/// Migrated from InlineAssemblyBuilder.cpp.

#include "builder/sol-ast/stmts/SolInlineAssembly.h"
#include "builder/sol-ast/AsmScan.h"
#include "builder/sol-eb/ContractContext.h"
#include "builder/assembly/AssemblyBuilder.h"
#include "builder/assembly/YulPrePass.h"
#include "builder/sol-types/SolcConstFold.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/storage/StorageLayout.h"
#include "builder/contract/StateVarWalker.h"
#include "builder/storage/StorageMapper.h"

#include <libsolutil/Keccak256.h>
#include "builder/storage/StorageMapper.h"
#include "builder/storage/TransientStorage.h"
#include "Logger.h"

#include <libsolidity/ast/ASTUtils.h>
#include <libsolidity/ast/Types.h>
#include <libsolutil/Numeric.h>

#include <optional>

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
	std::map<std::string, AssemblyBuilder::SlotRoute> slotRoutes;
	std::vector<AssemblyBuilder::SlotRoute> slotDataRegions;
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
	// Struct-MEMBER array aliases: `uint256[] storage x = s.x;` → (s decl, "x").
	// These resolve to slot(struct) + memberOffset and route to the member's
	// storage inside the struct box (SlotRoute::StructMemberArrayRoot).
	std::map<std::string, std::pair<VariableDeclaration const*, std::string>> memberArrayAliases;
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
				if (auto const* initMA = dynamic_cast<MemberAccess const*>(vds->initialValue()))
				{
					auto const* baseId = dynamic_cast<Identifier const*>(&initMA->expression());
					auto const* baseVar = baseId
						? dynamic_cast<VariableDeclaration const*>(baseId->annotation().referencedDeclaration)
						: nullptr;
					if (baseVar && baseVar->isStateVariable()
						&& dynamic_cast<StructType const*>(baseVar->type()))
						memberArrayAliases[varDecl->name()] = {baseVar, initMA->memberName()};
					break;
				}
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
					&& builder::containsInlineAssembly(fd->body()))
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

			// Compile-time slot routes: connect CONSTANT-slot asm sload/sstore to the
			// named vars' real storage (see AssemblyBuilder::SlotRoute). Scalars route
			// to their app-global; a dynamic array's root slot is its LENGTH and its
			// keccak256(root) data region maps slot K+i -> element i. The keccak runs
			// HERE, in the compiler (zero opcodes) — routing is constant comparison.
			forEachStateVar(*contractDef, [&](solidity::frontend::VariableDeclaration const* svDecl)
			{
				if (!svDecl || svDecl->isConstant() || svDecl->immutable()) return;
				if (svDecl->referenceLocation() == VariableDeclaration::Location::Transient) return;
				auto const* vi = layout.getVarInfo(svDecl->name());
				if (!vi) return;
				auto const* arrT = dynamic_cast<solidity::frontend::ArrayType const*>(svDecl->type());
				if (arrT && arrT->isDynamicallySized() && !arrT->isByteArrayOrString()
					&& builder::StorageMapper::shouldUseBoxStorage(*svDecl))
				{
					auto* arc4Elem = m_blk.typeMapper().mapSolTypeToARC4(arrT->baseType());
					if (builder::StorageMapper::computeEncodedElementSize(arc4Elem) != 32)
						return;   // tier-1: 32-byte elements only
					AssemblyBuilder::SlotRoute root;
					root.kind = AssemblyBuilder::SlotRoute::Kind::ArrayRoot;
					root.varName = svDecl->name();
					slotRoutes[vi->slot.str()] = root;

					// K = keccak256(32-byte BE root slot) — compile-time.
					auto slotWord = solidity::toBigEndian(vi->slot);
					auto k = solidity::u256(solidity::util::keccak256(slotWord));
					AssemblyBuilder::SlotRoute data;
					data.kind = AssemblyBuilder::SlotRoute::Kind::ArrayData;
					data.varName = svDecl->name();
					data.dataBase = k.str();
					slotDataRegions.push_back(std::move(data));
				}
				else if (vi->isFullSlot
					&& svDecl->type()->isValueType()   // structs share the slot repr; route can't model them
					&& !builder::StorageMapper::shouldUseBoxStorage(*svDecl))
				{
					AssemblyBuilder::SlotRoute r;
					r.kind = AssemblyBuilder::SlotRoute::Kind::Scalar;
					r.varName = svDecl->name();
					r.wtype = vi->wtype;
					slotRoutes[vi->slot.str()] = r;
				}
			});

			// Struct-member array aliases: `uint256[] storage x = s.x; sstore(x.slot, L)`.
			// Resolve x.slot to slot(s) + storageOffsetsOfMember(x) and route it to the
			// member array INSIDE s's box (COW length write / count read). The constant
			// is registered ONLY together with its route — a constant without a route
			// would silently fall through to the box-per-slot cell, invisible to
			// high-level reads of s.x (the exact silent-wrong-slot class the
			// unmodeled-.slot hard error exists to stop).
			for (auto const& [yulId, extInfo]: annotation.externalReferences)
			{
				if (extInfo.suffix != "slot" || !extInfo.declaration) continue;
				auto const* varDecl = dynamic_cast<VariableDeclaration const*>(extInfo.declaration);
				if (!varDecl || !varDecl->isLocalVariable()) continue;
				auto maIt = memberArrayAliases.find(varDecl->name());
				if (maIt == memberArrayAliases.end()) continue;
				auto const* structVar = maIt->second.first;
				auto const& fieldName = maIt->second.second;

				if (!builder::StorageMapper::shouldUseBoxStorage(*structVar)) continue;
				auto const* structWType = dynamic_cast<awst::ARC4Struct const*>(
					m_blk.typeMapper().map(structVar->type()));
				auto const* structType = dynamic_cast<StructType const*>(structVar->type());
				if (!structWType || !structType) continue;

				// tier-1: dynamic arrays of 32-byte-encoded elements only
				auto const* localArrT = dynamic_cast<ArrayType const*>(varDecl->type());
				if (!localArrT || !localArrT->isDynamicallySized()
					|| localArrT->isByteArrayOrString()) continue;
				auto* arc4Elem = m_blk.typeMapper().mapSolTypeToARC4(localArrT->baseType());
				if (builder::StorageMapper::computeEncodedElementSize(arc4Elem) != 32) continue;

				auto const* vi = layout.getVarInfo(structVar->name());
				if (!vi) continue;
				auto memberOff = structType->storageOffsetsOfMember(fieldName);
				if (memberOff.second != 0) continue;   // arrays always start a fresh slot
				auto absSlot = vi->slot + memberOff.first;
				std::string slotStr = absSlot.str();

				AssemblyBuilder::SlotRoute r;
				r.kind = AssemblyBuilder::SlotRoute::Kind::StructMemberArrayRoot;
				r.varName = structVar->name();
				r.fieldName = fieldName;
				r.wtype = structWType;
				slotRoutes[slotStr] = r;
				constants[yulId->name.str()] = slotStr;
			}

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
				std::string slotStr;
				unsigned byteOffset = 0;
				bool resolved = false;
				if (isTransient)
				{
					auto* ts = m_blk.builderCtx().transientStorage;
					if (ts)
					{
						if (auto const* tv = ts->getVarInfo(varDecl->name()))
						{
							slotStr = std::to_string(tv->slot);
							byteOffset = tv->byteOffset;
							resolved = true;
						}
					}
				}
				else
				{
					if (auto const* varInfo = layout.getVarInfo(varDecl->name()))
					{
						slotStr = varInfo->slot.str();
						byteOffset = varInfo->byteOffset;
						resolved = true;
					}
				}
				if (!resolved) continue;

				if (suffix == "slot")
				{
					constants[yulName] = slotStr;
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
	// STATIC calldata pointers (structs / fixed arrays): their bare Yul name is the
	// byte OFFSET of their data (`s := s2`, `s := t` read/write it). Kept separate
	// from the dynamic set — statics have only __cd_off_<name>, no length local.
	std::set<std::string> calldataStaticPtrNames;
	// Signed intN (N<=64) locals: name→bits, for sign-extended bare Yul reads.
	std::map<std::string, unsigned> signedParamBits;
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
			// A SIZED ReferenceArray (`uint[2] calldata`) is a STATIC pointer
			// (fixed offset, no length local); only an UNSIZED one is dynamic.
			// The old unconditional ReferenceArray match in the dynamic branch
			// swallowed the sized case too, so `uint[2] calldata` got registered
			// with a nonexistent __cd_len_ (dynamic protocol) — the static
			// branch's ReferenceArray case was dead.
			auto const* refArr = dynamic_cast<awst::ReferenceArray const*>(t);
			bool dynamicRefArr = refArr && !refArr->arraySize();
			bool sizedRefArr = refArr && refArr->arraySize().has_value();
			if (t == awst::WType::bytesType() || t == awst::WType::stringType()
				|| t->kind() == awst::WTypeKind::ARC4DynamicArray
				|| dynamicRefArr)
				calldataPointerNames.insert(base);
			else if (t->kind() == awst::WTypeKind::ARC4Struct
				|| t->kind() == awst::WTypeKind::ARC4StaticArray
				|| sizedRefArr)
				calldataStaticPtrNames.insert(base);
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
		// Signed intN (N<=64) value locals are uint64-backed 64-bit TC; a bare Yul
		// read must present the sign-extended 256-bit word (EVM identifier = full
		// word). Wider signed (64<N<256) are biguint-backed canonical already.
		if (auto it = builder::SolIntType::fromSol(varDecl->annotation().type);
			it && it->isSigned && it->bits <= 64)
			signedParamBits[name] = it->bits;
	}
	for (auto const& [n, bw]: m_blk.fn.paramBitWidths)
		paramBitWidths.emplace(n, bw);

	AssemblyBuilder asmTranslator(m_blk.typeMapper(), m_blk.sourceFile(), contextName);
	asmTranslator.setFrameIsProgram(m_blk.fn.frameIsProgram);
	asmTranslator.setSeededCalldataPointers(m_blk.fn.seededCalldataPointers);
	asmTranslator.setCalldataSolTypes(m_blk.fn.paramSolTypes);
	asmTranslator.setCalldataPointerNames(std::move(calldataPointerNames));
	asmTranslator.setCalldataStaticPtrNames(std::move(calldataStaticPtrNames));
	asmTranslator.setSlotRoutes(std::move(slotRoutes), std::move(slotDataRegions));
	asmTranslator.setSignedParamBits(std::move(signedParamBits));
	// EXPERIMENTAL Yul optimiser pre-pass (possible_solc item 5). Off unless
	// --yul-prepass / PUYA_SOL_YUL_PREPASS. The pre-pass OWNS the new tree and the
	// rebuilt external-ref map, both of which must outlive buildBlock — the walker
	// holds Identifier pointers into them.
	std::optional<YulPrePassResult> prePass;
	solidity::yul::Block const* asmRoot = &m_node.operations().root();
	auto const* asmExternalRefs = &annotation.externalReferences;
	if (yulPrePassEnabled() && annotation.analysisInfo)
	{
		prePass = runYulPrePass(
			m_node.operations().root(),
			m_node.dialect(),
			*annotation.analysisInfo,
			annotation.externalReferences,
			// Stage 1: ExpressionSimplifier + StructuralSimplifier (canonicalise
			// expressions/structure). No pruning/LoadResolver/SSA yet.
			/*_stepAbbreviations=*/"st");
		asmRoot = prePass->block.get();
		asmExternalRefs = &prePass->externalRefs;
	}

	auto stmts = asmTranslator.buildBlock(
		*asmRoot,
		augmentedParams,
		m_blk.fn.returnType,
		constants,
		paramBitWidths,
		storageSlotVars,
		boxKeyedStructSlots,
		blobOffsetVars,
		structRefSlotLocals,
		stateVarSlots,
		*asmExternalRefs,
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
