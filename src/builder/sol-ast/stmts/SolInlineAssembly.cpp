/// @file SolInlineAssembly.cpp

#include "builder/sol-ast/stmts/SolInlineAssembly.h"
#include "builder/SelectorSemantics.h"
#include "builder/ProgramAnalysis.h"
#include "builder/sol-eb/ContractContext.h"
#include "builder/assembly/AssemblyBuilder.h"
#include "builder/sol-types/SolcConstFold.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/storage/EvmLayoutMode.h"
#include "builder/storage/StorageLayout.h"
#include "builder/contract/StateVarWalker.h"
#include "builder/storage/StorageMapper.h"

#include <libsolutil/Keccak256.h>
#include "builder/storage/TransientStorage.h"
#include "Logger.h"

#include <libsolidity/ast/ASTUtils.h>
#include <libsolidity/ast/Types.h>
#include <libsolutil/Numeric.h>

#include <algorithm>
#include <functional>
#include "builder/sol-types/SolIntType.h"
// yul nodes BY VALUE (the AST aliases are std::variant, which needs
// complete types). Kept out of AssemblyBuilder.h so only the TUs that
// actually instantiate them pay the ~223k lines.
#include <libyul/AST.h>
#include <libyul/Dialect.h>

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

namespace
{

/// Classify solc's external variable references once for constants, named-cell
/// routes and live storage-reference locals.
struct ExternalBindings
{
	std::map<std::string, std::string> constants, structRefSlotLocals;
	std::map<std::string, AssemblyBuilder::BoxKeyedSlot> boxKeyedStructSlots;
	std::map<std::string, AssemblyBuilder::StateVarSlot> stateVarSlots;
};

ExternalBindings collectExternalBindings(BlockContext& blk, InlineAssembly const& node)
{
	ExternalBindings bindings;
	for (auto const& [yulId, reference]: node.annotation().externalReferences)
	{
		auto const* vd = dynamic_cast<VariableDeclaration const*>(reference.declaration);
		if (!vd) continue;
		auto const name = yulId->name.str();
		if (vd->isConstant())
		{
			if (!solidity::frontend::isConstantVariableRecursive(*vd))
				if (auto value = builder::SolcConstFold::constantVarEvmWord(*vd))
					bindings.constants[name] = value->str();
			continue;
		}
		auto slot = blk.scope.bindings.slotStorageRefs.get(vd->id());
		if (!vd->isStateVariable() && vd->type()->dataStoredIn(DataLocation::Storage))
		{
			// Solc IRGeneratorForStatements::CopyTranslate: reference locals
			// carry a slot, never a packed-value byte offset.
			if (reference.suffix == "offset") bindings.constants[name] = "0";
			else if (reference.suffix == "slot" && (blk.typeMapper().profile().evmStorageLayout || slot))
			{
				auto const* local = dynamic_cast<awst::VarExpression const*>(slot.get());
				bindings.structRefSlotLocals[name] = local ? local->name : blk.scope.awstVarName(*vd);
			}
		}
		if (blk.typeMapper().profile().evmStorageLayout || reference.suffix != "slot") continue;
		if (vd->isLocalVariable())
		{
			auto const* alias = blk.scope.bindings.storageAliases.find(vd->id());
			if (!alias || alias->kind != StorageAlias::Kind::StateRead) continue;
			auto value = awst::unwrapStateGet(alias->expr);
			if (auto const* box = dynamic_cast<awst::BoxValueExpression const*>(value.get()))
				if (dynamic_cast<awst::ARC4Struct const*>(box->wtype))
					bindings.boxKeyedStructSlots[name] = {box->key, box->wtype};
		}
		else if (vd->isStateVariable() && !blk.builderCtx().storageMapper.shouldUseBoxStorage(*vd))
		{
			// Only a full-width uint256 maps one EVM word to one named cell.
			// Packed/signed values and aggregates keep the normal slot route.
			if (auto const* integer = dynamic_cast<IntegerType const*>(vd->type());
				integer && !integer->isSigned() && integer->numBits() == 256)
				bindings.stateVarSlots[name] = {
					blk.builderCtx().storageMapper.physicalBindingFor(*vd).key, blk.typeMapper().map(vd->type())};
		}
	}
	return bindings;
}

/// toAwst scan: local storage aliases (`uint256[] storage x = a;` keeps the initializer in the VariableDeclarationStatement, so …
void collectStorageLocalAliases(
	BlockContext& blk, InlineAssembly const& node,
	std::map<int64_t, VariableDeclaration const*>& storageLocalAliases,
	std::map<int64_t, std::pair<VariableDeclaration const*, std::string>>& memberArrayAliases)
{
	auto const& annotation = node.annotation();
	for (auto const& [yulId, extInfo]: annotation.externalReferences)
	{
		if (extInfo.suffix != "slot") continue;
		auto const* varDecl = dynamic_cast<VariableDeclaration const*>(extInfo.declaration);
		if (!varDecl || !varDecl->isLocalVariable()) continue;

		// The analyzed declaration ID supplies source provenance. A runtime slot
		// binding supersedes it, including in named storage mode.
		if (blk.scope.bindings.slotStorageRefs.get(varDecl->id())) continue;
		auto const& initializers = blk.typeMapper().analysis().localInitializers;
		auto initial = initializers.find(varDecl->id());
		if (initial == initializers.end()) continue;

		if (auto const* initMA = dynamic_cast<MemberAccess const*>(initial->second))
		{
			auto const* baseId = dynamic_cast<Identifier const*>(&initMA->expression());
			auto const* baseVar = baseId
				? dynamic_cast<VariableDeclaration const*>(
					baseId->annotation().referencedDeclaration)
				: nullptr;
			if (baseVar && baseVar->isStateVariable()
				&& dynamic_cast<StructType const*>(baseVar->type()))
				memberArrayAliases[varDecl->id()] = {
					baseVar, initMA->memberName()};
			continue;
		}
		auto const* initId = dynamic_cast<Identifier const*>(initial->second);
		if (!initId)
			continue;
		auto const* sv = dynamic_cast<VariableDeclaration const*>(
			initId->annotation().referencedDeclaration);
		if (sv && sv->isStateVariable())
			storageLocalAliases[varDecl->id()] = sv;
	}
}

/// toAwst scan: compile-time slot routes + layout-derived slot/offset constants (StorageLayout of the current or declaring contract).
void registerStateVarSlotRoutes(
	BlockContext& blk, ContractDefinition const& contractDef,
	StorageLayout const& layout,
	std::map<std::string, AssemblyBuilder::SlotRoute>& slotRoutes,
	std::vector<AssemblyBuilder::SlotRoute>& slotDataRegions)
{
	forEachStateVar(contractDef, [&](solidity::frontend::VariableDeclaration const* svDecl)
	{
		if (blk.typeMapper().profile().evmStorageLayout) return;   // no named-cell routes in slot space
		if (!svDecl || svDecl->isConstant() || svDecl->immutable()) return;
		if (svDecl->referenceLocation() == VariableDeclaration::Location::Transient) return;
		auto const* vi = layout.getVarInfoById(svDecl->id());
		if (!vi) return;
		auto physicalName =
			blk.builderCtx().storageMapper.physicalBindingFor(*svDecl).key;
		auto const* arrT = dynamic_cast<solidity::frontend::ArrayType const*>(svDecl->type());
		if (arrT && arrT->isDynamicallySized() && !arrT->isByteArrayOrString()
			&& blk.builderCtx().storageMapper.shouldUseBoxStorage(*svDecl))
		{
			auto* arc4Elem = blk.typeMapper().mapSolTypeToARC4(arrT->baseType());
			auto elemSize = builder::computeEncodedElementSize(arc4Elem).fixedBytes<int>().value_or(0);
			AssemblyBuilder::SlotRoute root;
			root.kind = AssemblyBuilder::SlotRoute::Kind::ArrayRoot;
			root.varName = physicalName;
			root.wtype = blk.typeMapper().map(arrT);
			root.elementSize = elemSize;
			slotRoutes[vi->slot.str()] = root;

			// Raw EVM data slots coincide with ARC4 element boundaries only
			// for one-word encodings. Root length routing above is generic;
			// packed and multi-slot data regions remain an explicit layout
			// boundary instead of being silently mis-routed.
			if (elemSize == 32)
			{
				auto slotWord = solidity::toBigEndian(vi->slot);
				auto k = solidity::u256(solidity::util::keccak256(slotWord));
				AssemblyBuilder::SlotRoute data;
				data.kind = AssemblyBuilder::SlotRoute::Kind::ArrayData;
				data.varName = physicalName;
				data.dataBase = k.str();
				data.elementSize = elemSize;
				slotDataRegions.push_back(std::move(data));
			}
		}
		else if (vi->isFullSlot
			&& svDecl->type()->isValueType()   // structs share the slot repr; route can't model them
			&& !blk.builderCtx().storageMapper.shouldUseBoxStorage(*svDecl))
		{
			AssemblyBuilder::SlotRoute r;
			r.kind = AssemblyBuilder::SlotRoute::Kind::Scalar;
			r.varName = physicalName;
			r.wtype = vi->wtype;
			slotRoutes[vi->slot.str()] = r;
		}
	});

}

/// Layout-scan piece: struct-member array aliases — `uint256[] storage x = s.x; sstore(x.slot, L)`.
void registerMemberArrayRoutes(
	BlockContext& blk, InlineAssembly const& node,
	StorageLayout const& layout,
	std::map<int64_t, std::pair<VariableDeclaration const*, std::string>> const& memberArrayAliases,
	std::map<std::string, std::string>& constants,
	std::map<std::string, AssemblyBuilder::SlotRoute>& slotRoutes)
{
	auto const& annotation = node.annotation();
	// Struct-member array aliases: `uint256[] storage x = s.x; sstore(x.slot, L)`.
	// Resolve x.slot to slot(s) + storageOffsetsOfMember(x) and route it to the
	// member array INSIDE s's box (COW length write / count read). The constant
	// is registered ONLY together with its route — a constant without a route
	// would silently fall through to the box-per-slot cell, invisible to
	// high-level reads of s.x (the exact silent-wrong-slot class the
	// unmodeled-.slot hard error exists to stop).
	for (auto const& [yulId, extInfo]: annotation.externalReferences)
	{
		if (blk.typeMapper().profile().evmStorageLayout) break;   // member arrays live at real slots
		if (extInfo.suffix != "slot" || !extInfo.declaration) continue;
		auto const* varDecl = dynamic_cast<VariableDeclaration const*>(extInfo.declaration);
		if (!varDecl || !varDecl->isLocalVariable()) continue;
		auto maIt = memberArrayAliases.find(varDecl->id());
		if (maIt == memberArrayAliases.end()) continue;
		auto const* structVar = maIt->second.first;
		auto const& fieldName = maIt->second.second;

		if (!blk.builderCtx().storageMapper.shouldUseBoxStorage(*structVar)) continue;
		auto const* structWType = dynamic_cast<awst::ARC4Struct const*>(
			blk.typeMapper().map(structVar->type()));
		auto const* structType = dynamic_cast<StructType const*>(structVar->type());
		if (!structWType || !structType) continue;

		// The root slot is the element count for every dynamic array.
		// Element construction is type-directed in the route handler, so
		// nested/static/dynamic element shapes share this registration.
		auto const* localArrT = dynamic_cast<ArrayType const*>(varDecl->type());
		if (!localArrT || !localArrT->isDynamicallySized()
			|| localArrT->isByteArrayOrString()) continue;

		auto const* vi = layout.getVarInfoById(structVar->id());
		if (!vi) continue;
		auto memberOff = structType->storageOffsetsOfMember(fieldName);
		if (memberOff.second != 0) continue;   // arrays always start a fresh slot
		auto absSlot = vi->slot + memberOff.first;
		std::string slotStr = absSlot.str();

		AssemblyBuilder::SlotRoute r;
		r.kind = AssemblyBuilder::SlotRoute::Kind::StructMemberArrayRoot;
		r.varName =
			blk.builderCtx().storageMapper.physicalBindingFor(*structVar).key;
		r.fieldName = fieldName;
		r.wtype = structWType;
		r.elementSize = builder::computeEncodedElementSize(
			blk.typeMapper().mapSolTypeToARC4(localArrT->baseType())).fixedBytes<int>().value_or(0);
		slotRoutes[slotStr] = r;
		constants[yulId->name.str()] = slotStr;
	}

}

/// Layout-scan piece: per-ref slot/offset constants (following local storage aliases to the underlying state var; transient vars …
void registerLayoutConstants(
	BlockContext& blk, InlineAssembly const& node,
	StorageLayout const& layout,
	std::map<int64_t, VariableDeclaration const*> const& storageLocalAliases,
	std::map<std::string, std::string>& constants,
	std::map<std::string, std::string>& storageSlotVars)
{
	auto const& annotation = node.annotation();
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
			// Slot mode: the local IS a biguint slot var (registered above);
			// resolving through the alias to a CONSTANT would go stale on
			// pointer rebinds.
			if (blk.typeMapper().profile().evmStorageLayout) continue;
			// Local storage references: `uint256[] storage x = a;`
			// The initialiser lives in the parent VariableDeclarationStatement,
			// not in VariableDeclaration::value(). We must look it up from the
			// storageLocalAliases map from analyzed declaration-ID provenance.
			auto aliasIt = storageLocalAliases.find(varDecl->id());
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
			auto* ts = blk.builderCtx().transientStorage;
			if (ts)
			{
				if (auto const* tv = ts->getVarInfoById(varDecl->id()))
				{
					slotStr = std::to_string(tv->slot);
					byteOffset = tv->byteOffset;
					resolved = true;
				}
			}
		}
		else
		{
			if (auto const* varInfo = layout.getVarInfoById(varDecl->id()))
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
			storageSlotVars[yulName] =
				blk.builderCtx().storageMapper.physicalBindingFor(*varDecl).key;
		}
		else if (suffix == "offset")
		{
			constants[yulName] = std::to_string(byteOffset);
		}
	}
}

void collectSlotRoutesAndLayoutConstants(
	BlockContext& blk, InlineAssembly const& node,
	std::map<int64_t, std::pair<VariableDeclaration const*, std::string>> const& memberArrayAliases,
	std::map<int64_t, VariableDeclaration const*> const& storageLocalAliases,
	std::map<std::string, std::string>& constants,
	std::map<std::string, AssemblyBuilder::SlotRoute>& slotRoutes,
	std::vector<AssemblyBuilder::SlotRoute>& slotDataRegions,
	std::map<std::string, std::string>& storageSlotVars)
{
	auto const& annotation = node.annotation();

	// Prefer the currently-being-built contract — its layout reflects the
	// derived class's `layout at N` annotation and inherited-var ordering.
	// Falls back to the declaring contract of any referenced state var
	// (which is correct for free functions / libraries).
	ContractDefinition const* contractDef = blk.builderCtx().currentContract;
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
	if (!contractDef)
		return;

	StorageLayout fallbackLayout;
	auto const* layout = blk.builderCtx().currentContract == contractDef
		? blk.builderCtx().storageLayout
		: nullptr;
	if (!layout)
	{
		fallbackLayout.computeLayout(*contractDef, blk.typeMapper());
		layout = &fallbackLayout;
	}

	registerStateVarSlotRoutes(
		blk, *contractDef, *layout, slotRoutes, slotDataRegions);

	registerMemberArrayRoutes(
		blk, node, *layout, memberArrayAliases, constants, slotRoutes);

	registerLayoutConstants(
		blk, node, *layout, storageLocalAliases, constants, storageSlotVars);
}

/// toAwst scan: augmented param list (external refs join the function's own params), sub-64-bit widths, blob-offset vars, calldata …
std::vector<std::pair<std::string, awst::WType const*>> collectAugmentedParams(
	BlockContext& blk, InlineAssembly const& node,
	std::function<std::string(solidity::frontend::VariableDeclaration const&)> const& declNameFn,
	std::map<std::string, unsigned>& paramBitWidths,
	std::map<std::string, std::string>& blobOffsetVars,
	std::set<std::string>& calldataPointerNames,
	std::set<std::string>& calldataStaticPtrNames,
	std::map<std::string, unsigned>& signedParamBits)
{
	auto const& annotation = node.annotation();
	// paramBitWidths: sub-64-bit widths. blobOffsetVars: blob-backed memory
	// aggregates resolve to their uint64 memory-pointer offset (not the
	// aggregate value). calldataPointerNames: base names of external refs
	// that are DYNAMIC CALLDATA pointers (params, locals, or calldata return
	// vars) — suffixed refs (`x.offset := 0`) register under the dotted name,
	// so m_locals-based detection misses the base; this set carries the
	// authoritative answer from the decl's referenceLocation.
	// calldataStaticPtrNames: STATIC calldata pointers (structs / fixed
	// arrays) whose bare Yul name is the byte OFFSET of their data — statics
	// have only __cd_off_<name>, no length local. signedParamBits: signed
	// intN (N<=64) locals for sign-extended bare Yul reads.
	auto augmentedParams = blk.fn.params;
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
		// A storage coordinate is a full-width word, not the declaration's
		// value. Huge arrays still expose solc's .slot/.offset facts even when
		// their whole-value representation cannot fit on AVM.
		if ((extInfo.suffix == "slot" || extInfo.suffix == "offset")
			&& (varDecl->isStateVariable()
				|| varDecl->referenceLocation() == VariableDeclaration::Location::Storage))
		{
			augmentedParams.emplace_back(name, awst::WType::biguintType());
			continue;
		}
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
			// solc already owns the recursive ABI rule: a fixed array or struct is
			// dynamic when any contained member is dynamic. Use that fact instead
			// of enumerating WType shapes one level at a time.
			if (varDecl->type()->isDynamicallyEncoded())
				calldataPointerNames.insert(base);
			else if (dynamic_cast<solidity::frontend::ReferenceType const*>(
					varDecl->type()))
				calldataStaticPtrNames.insert(base);
		}
		if (auto blobOff = blk.scope.bindings.blobAggregates.get(varDecl->id()); !blobOff.empty())
			blobOffsetVars[name] = blobOff;
		bool found = false;
		for (auto const& [pName, pType]: augmentedParams)
			if (pName == name) found = true;
		if (!found)
		{
			auto* type = blk.typeMapper().map(varDecl->type());
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
	for (auto const& [n, bw]: blk.fn.paramBitWidths)
		paramBitWidths.emplace(n, bw);

	return augmentedParams;
}

} // anonymous namespace

std::vector<std::shared_ptr<awst::Statement>> SolInlineAssembly::toAwst()
{
	Logger::instance().debug("translating inline assembly block", m_loc);

	std::string contextName = m_blk.builderCtx().contractName;
	if (contextName.empty())
		contextName = "free";
	contextName += "_" + std::to_string(m_blk.fn.callableId)
		+ "_asm_" + std::to_string(m_node.id());

	auto bindings = collectExternalBindings(m_blk, m_node);

	// Resolve a Solidity VariableDeclaration to its AWST name (Context::awstVarName:
	// locals → name__<declId>, params/returns bare). AssemblyBuilder names outer-var
	// refs decl-based off solc's externalReferences via this callback.
	auto declNameFn = [this](solidity::frontend::VariableDeclaration const& _vd) {
		return m_blk.scope.awstVarName(_vd);
	};

	std::map<int64_t, VariableDeclaration const*> storageLocalAliases;
	std::map<int64_t, std::pair<VariableDeclaration const*, std::string>> memberArrayAliases;
	collectStorageLocalAliases(m_blk, m_node, storageLocalAliases, memberArrayAliases);

	std::map<std::string, AssemblyBuilder::SlotRoute> slotRoutes;
	std::vector<AssemblyBuilder::SlotRoute> slotDataRegions;
	std::map<std::string, std::string> storageSlotVars;
	collectSlotRoutesAndLayoutConstants(
		m_blk, m_node, memberArrayAliases, storageLocalAliases,
		bindings.constants, slotRoutes, slotDataRegions, storageSlotVars);

	std::map<std::string, unsigned> paramBitWidths;
	std::map<std::string, std::string> blobOffsetVars;
	std::set<std::string> calldataPointerNames;
	std::set<std::string> calldataStaticPtrNames;
	std::map<std::string, unsigned> signedParamBits;
	auto augmentedParams = collectAugmentedParams(
		m_blk, m_node, declNameFn, paramBitWidths, blobOffsetVars,
		calldataPointerNames, calldataStaticPtrNames, signedParamBits);

	AssemblyBuilder asmTranslator(m_blk.typeMapper(), m_blk.sourceFile(), contextName,
		m_blk.scope.isInConstructor());
	asmTranslator.setTransientStorage(m_blk.builderCtx().transientStorage);
	asmTranslator.setFrameIsProgram(m_blk.fn.frameIsProgram);
	asmTranslator.setSeededCalldataPointers(&m_blk.fn.seededCalldataPointers);
	asmTranslator.setCalldataSolTypes(m_blk.fn.paramSolTypes);
	asmTranslator.setBoxKeyStructParams(m_blk.fn.boxKeyStructParams);
	asmTranslator.setCalldataPointerNames(std::move(calldataPointerNames));
	asmTranslator.setCalldataStaticPtrNames(std::move(calldataStaticPtrNames));
	asmTranslator.setSlotRoutes(std::move(slotRoutes), std::move(slotDataRegions));
	asmTranslator.setSignedParamBits(std::move(signedParamBits));
	asmTranslator.setReturnSolTypes(m_blk.fn.returnSolTypes);
	asmTranslator.setReturnWirePlan(
		m_blk.fn.encodeReturnsAtBuildTime ? &m_blk.fn.returnWirePlan : nullptr,
		m_blk.fn.returnAsmWrap);
	asmTranslator.setSelectorRoutes(
		builder::SelectorSemantics::routes(m_blk.builderCtx()));
	return asmTranslator.buildBlock(
		*m_blk.typeMapper().analysis().preparedAssemblies.at(m_node.id()),
		augmentedParams,
		m_blk.fn.returnType,
		bindings.constants,
		paramBitWidths,
		storageSlotVars,
		bindings.boxKeyedStructSlots,
		blobOffsetVars,
		bindings.structRefSlotLocals,
		bindings.stateVarSlots,
		declNameFn,
		// Only the function's own params are real calldata args; externalReferences appended
		// to augmentedParams above (return vars, outer locals) are NOT in the EVM calldata buffer.
		m_blk.fn.params.size());
}

} // namespace puyasol::builder::sol_ast
