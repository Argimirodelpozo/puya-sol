/// @file SolInlineAssembly.cpp

#include "builder/ast/stmts/SolInlineAssembly.h"
#include "awst/NameGen.h"
#include "builder/codec/SelectorSemantics.h"
#include "builder/context/ProgramAnalysis.h"
#include "builder/context/ContractContext.h"
#include "builder/yul/AssemblyBuilder.h"
#include "builder/solc/SolcConstFold.h"
#include "builder/solc/SolcFacts.h"
#include "builder/types/TypeMapper.h"
#include "builder/target/EvmLayoutMode.h"
#include "builder/storage/StorageLayout.h"
#include "builder/storage/StateVarWalker.h"
#include "builder/storage/StorageMapper.h"

#include <libsolutil/Keccak256.h>
#include "builder/storage/TransientStorage.h"
#include "Logger.h"

#include <libsolidity/ast/ASTUtils.h>
#include <libsolidity/ast/Types.h>
#include <libsolutil/Numeric.h>

#include <algorithm>
#include <functional>
#include "builder/types/SolIntType.h"
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

/// Per-block overlays share one classification of each solc external reference.
/// Contract layout routes are independent of those mutable local bindings.
class AssemblyBindings
{
public:
	using Params = std::vector<std::pair<std::string, awst::WType const*>>;
	Params params;
	std::map<std::string, std::string> constants, structRefSlotLocals, storageSlotVars, blobOffsetVars;
	std::map<std::string, AssemblyBuilder::BoxKeyedSlot> boxKeyedStructSlots;
	std::map<std::string, AssemblyBuilder::StateVarSlot> stateVarSlots;
	std::map<std::string, AssemblyBuilder::SlotRoute> slotRoutes;
	std::vector<AssemblyBuilder::SlotRoute> slotDataRegions;
	std::map<std::string, unsigned> paramBitWidths, signedParamBits;
	std::map<std::string, std::string> wordBindings;
	std::set<std::string> calldataPointerNames, calldataStaticPtrNames;

	AssemblyBindings(BlockContext& block, InlineAssembly const& node)
		: params(block.fn.params), blk(block)
	{
		for (auto const& [name, type]: params) paramNames.insert(name);
		std::vector<Reference> references;
		auto const* contract = blk.builderCtx().currentContract;
		for (auto const& [yulId, info]: node.annotation().externalReferences)
			if (auto const* declaration = dynamic_cast<VariableDeclaration const*>(info.declaration))
			{
				references.push_back({*declaration, info, yulId->name.str()});
				if (!contract && declaration->isStateVariable() && !declaration->isConstant())
					contract = declaration->annotation().contract;
			}

		// Prefer the host's solc layout, including inherited layout-at offsets.
		StorageLayout fallback;
		auto const* layout = contract == blk.builderCtx().currentContract
			? blk.builderCtx().storageLayout : nullptr;
		if (contract)
		{
			if (!layout)
			{
				fallback.computeLayout(*contract, blk.typeMapper());
				layout = &fallback;
			}
			registerStateVarSlotRoutes(blk, *contract, *layout, slotRoutes, slotDataRegions);
		}
		for (auto const& reference: references) bind(reference, layout);
	}

private:
	struct Reference
	{
		VariableDeclaration const& declaration;
		InlineAssemblyAnnotation::ExternalIdentifierInfo const& info;
		std::string name;
	};
	BlockContext& blk;
	std::set<std::string> paramNames;

	void bind(Reference const& ref, StorageLayout const* layout)
	{
		auto const& vd = ref.declaration;
		auto const& suffix = ref.info.suffix;
		if (vd.isConstant())
		{
			if (!isConstantVariableRecursive(vd))
				if (auto word = SolcConstFold::constantVarEvmWord(vd))
					constants[ref.name] = word->str();
			return;
		}
		bindParameter(ref);
		auto slot = blk.scope.bindings.slotStorageRefs.get(vd.id());
		bool const slotMode = blk.typeMapper().profile().evmStorageLayout;
		if (!vd.isStateVariable() && vd.type()->dataStoredIn(DataLocation::Storage))
		{
			// solc CopyTranslate gives reference locals a slot, never a packed offset.
			if (suffix == "offset") constants[ref.name] = "0";
			else if (suffix == "slot" && (slotMode || slot))
			{
				auto const* local = dynamic_cast<awst::VarExpression const*>(slot.get());
				structRefSlotLocals[ref.name] = local ? local->name : blk.scope.awstVarName(vd);
			}
		}
		if (!slotMode && suffix == "slot")
		{
			if (vd.isLocalVariable())
			{
				auto const* alias = blk.scope.bindings.storageAliases.find(vd.id());
				if (alias && alias->kind == StorageAlias::Kind::StateRead)
				{
					auto value = awst::unwrapStateGet(alias->expr);
					if (auto const* box = dynamic_cast<awst::BoxValueExpression const*>(value.get());
						box && dynamic_cast<awst::ARC4Struct const*>(box->wtype))
						boxKeyedStructSlots[ref.name] = {box->key, box->wtype};
				}
			}
			else if (vd.isStateVariable() && !blk.builderCtx().storageMapper.shouldUseBoxStorage(vd))
			{
				// Only a full-width uint256 maps one EVM word to one named cell.
				if (auto const* integer = dynamic_cast<IntegerType const*>(vd.type());
					integer && !integer->isSigned() && integer->numBits() == 256)
					stateVarSlots[ref.name] = {
						blk.builderCtx().storageMapper.physicalBindingFor(vd).key, blk.typeMapper().map(vd.type())};
			}
		}
		if (!layout) return;
		if (vd.isStateVariable())
			bindLayout(ref, vd, *layout);
		else if (vd.isLocalVariable() && !slotMode && !slot && suffix == "slot")
		{
			// Runtime bindings supersede declaration-ID initializer provenance.
			auto const& initializers = blk.typeMapper().analysis().localInitializers;
			auto initial = initializers.find(vd.id());
			if (initial == initializers.end()) return;
			auto const& source = SolcFacts::unparenthesized(*initial->second);
			if (auto const* member = SolcFacts::expressionAs<MemberAccess>(&source))
			{
				auto const* base = dynamic_cast<VariableDeclaration const*>(
					ASTNode::referencedDeclaration(SolcFacts::unparenthesized(member->expression())));
				if (base && base->isStateVariable()) bindMemberArray(ref, *base, member->memberName(), *layout);
			}
			else if (auto const* state = dynamic_cast<VariableDeclaration const*>(ASTNode::referencedDeclaration(source));
				state && state->isStateVariable())
				bindLayout(ref, *state, *layout);
		}
	}

	void bindMemberArray(Reference const& ref, VariableDeclaration const& state,
		std::string const& field, StorageLayout const& layout)
	{
		if (!blk.builderCtx().storageMapper.shouldUseBoxStorage(state)) return;
		auto const* structure = dynamic_cast<StructType const*>(state.type());
		auto const* represented = dynamic_cast<awst::ARC4Struct const*>(blk.typeMapper().map(state.type()));
		auto const* array = dynamic_cast<ArrayType const*>(ref.declaration.type());
		if (!structure || !represented || !array || !array->isDynamicallySized() || array->isByteArrayOrString()) return;
		auto const* root = layout.getVarInfoById(state.id());
		if (!root) return;
		auto offset = structure->storageOffsetsOfMember(field);
		if (offset.second != 0) return;
		std::string slot = (root->slot + offset.first).str();

		// Register the constant only with its named-box route; an unmodeled
		// .slot must never fall through to an unrelated box-per-slot cell.
		AssemblyBuilder::SlotRoute route;
		route.kind = AssemblyBuilder::SlotRoute::Kind::StructMemberArrayRoot;
		route.varName = blk.builderCtx().storageMapper.physicalBindingFor(state).key;
		route.fieldName = field;
		route.wtype = represented;
		route.elementSize = computeEncodedElementSize(
			blk.typeMapper().mapSolTypeToARC4(array->baseType())).fixedBytes<int>().value_or(0);
		slotRoutes[slot] = std::move(route);
		constants[ref.name] = std::move(slot);
	}

	void bindLayout(Reference const& ref, VariableDeclaration const& state, StorageLayout const& layout)
	{
		std::string slot;
		unsigned offset = 0;
		if (state.referenceLocation() == VariableDeclaration::Location::Transient)
		{
			auto const* transient = blk.builderCtx().transientStorage;
			auto const* info = transient ? transient->getVarInfoById(state.id()) : nullptr;
			if (!info) return;
			slot = std::to_string(info->slot);
			offset = info->byteOffset;
		}
		else
		{
			auto const* info = layout.getVarInfoById(state.id());
			if (!info) return;
			slot = info->slot.str();
			offset = info->byteOffset;
		}
		if (ref.info.suffix == "slot")
		{
			constants[ref.name] = std::move(slot);
			storageSlotVars[ref.name] = blk.builderCtx().storageMapper.physicalBindingFor(state).key;
		}
		else if (ref.info.suffix == "offset")
			constants[ref.name] = std::to_string(offset);
	}

	void bindParameter(Reference const& ref)
	{
		auto const& vd = ref.declaration;
		// Match the resolver: declaration-based value names, dotted coordinates.
		std::string name = AssemblyBuilder::externalRefAwstName(ref.info, ref.name,
			[&](auto const& declaration) { return blk.scope.awstVarName(declaration); });
		if (auto word = blk.scope.bindings.assemblyWords.get(vd.id()); !word.empty())
			wordBindings.emplace(name, std::move(word));
		if ((ref.info.suffix == "slot" || ref.info.suffix == "offset")
			&& (vd.isStateVariable() || vd.referenceLocation() == VariableDeclaration::Location::Storage))
		{
			if (paramNames.insert(name).second) params.emplace_back(name, awst::WType::biguintType());
			return;
		}
		if (vd.referenceLocation() == VariableDeclaration::Location::CallData)
		{
			std::string base = name;
			if (auto dot = base.rfind('.'); dot != std::string::npos)
				if (auto suffix = base.substr(dot + 1); suffix == "offset" || suffix == "length")
					base.resize(dot);
			// ABI-dynamic structs/fixed arrays still occupy ONE calldata stack item.
			if (vd.type()->stackItems().size() == 2) calldataPointerNames.insert(base);
			else if (dynamic_cast<ReferenceType const*>(vd.type())) calldataStaticPtrNames.insert(base);
		}
		if (auto offset = blk.scope.bindings.blobAggregates.get(vd.id()); !offset.empty())
			blobOffsetVars[name] = offset;
		if (paramNames.insert(name).second) params.emplace_back(name, blk.typeMapper().map(vd.type()));
		if (auto integer = SolIntType::fromSol(vd.annotation().type))
		{
			if (integer->bits < 64) paramBitWidths[name] = integer->bits;
			if (integer->isSigned && integer->bits <= 64) signedParamBits[name] = integer->bits;
		}
	}
};

} // anonymous namespace

std::vector<std::shared_ptr<awst::Statement>> SolInlineAssembly::toAwst()
{
	Logger::instance().debug("translating inline assembly block", m_loc);

	std::string contextName = m_blk.builderCtx().contractName;
	if (contextName.empty())
		contextName = "free";
	// NameGen is scoped per contract; a public library body is also emitted
	// as a freestanding root outside that scope. Include the host identity.
	contextName += m_blk.builderCtx().currentContract
		? "_host_" + std::to_string(m_blk.builderCtx().currentContract->id()) : "_root";
	contextName += "_" + std::to_string(m_blk.fn.callableId)
		+ "_asm_" + std::to_string(m_node.id())
		// One solc body can be lowered as a library root, deployable library,
		// or specialized/modified body. Each emission owns its Yul helpers.
		+ "_emit_" + std::to_string(awst::NameGen::next("SolInlineAssembly.emit"));

	AssemblyBindings bindings(m_blk, m_node);

	// Resolve a Solidity VariableDeclaration to its AWST name (Context::awstVarName:
	// locals → name__<declId>, params/returns bare). AssemblyBuilder names outer-var
	// refs decl-based off solc's externalReferences via this callback.
	auto declNameFn = [this](solidity::frontend::VariableDeclaration const& _vd) {
		return m_blk.scope.awstVarName(_vd);
	};

	AssemblyBuilder asmTranslator(m_blk.typeMapper(), m_blk.sourceFile(), contextName,
		m_blk.scope.isInConstructor());
	asmTranslator.setTransientStorage(m_blk.builderCtx().transientStorage);
	asmTranslator.setWordBindings(std::move(bindings.wordBindings));
	asmTranslator.setFrameIsProgram(m_blk.fn.frameIsProgram);
	asmTranslator.setFunctionCalldata(m_blk.fn.hasAssemblyCalldata);
	asmTranslator.setCalldataSolTypes(m_blk.fn.parameterSolTypes());
	asmTranslator.setBoxKeyStructParams(m_blk.fn.boxKeyStructParams);
	asmTranslator.setCalldataPointerNames(std::move(bindings.calldataPointerNames));
	asmTranslator.setCalldataStaticPtrNames(std::move(bindings.calldataStaticPtrNames));
	asmTranslator.setSlotRoutes(std::move(bindings.slotRoutes), std::move(bindings.slotDataRegions));
	asmTranslator.setSignedParamBits(std::move(bindings.signedParamBits));
	asmTranslator.setReturnSolTypes(m_blk.fn.returnSolTypes());
	asmTranslator.setReturnWirePlan(
		m_blk.fn.encodeReturnsAtBuildTime ? &m_blk.fn.returnWirePlan : nullptr,
		m_blk.fn.returnAsmWrap);
	if (builder::SelectorSemantics::enabled(m_blk.typeMapper()))
		asmTranslator.setSelectorRoutes(builder::SelectorSemantics::routes(m_blk.builderCtx()));
	return asmTranslator.buildBlock(
		*m_blk.typeMapper().analysis().preparedAssemblies.at(m_node.id()),
		bindings.params,
		m_blk.fn.returnType,
		bindings.constants,
		bindings.paramBitWidths,
		bindings.storageSlotVars,
		bindings.boxKeyedStructSlots,
		bindings.blobOffsetVars,
		bindings.structRefSlotLocals,
		bindings.stateVarSlots,
		declNameFn,
		// Only the function's own params are real calldata args; externalReferences appended
		// to augmentedParams above (return vars, outer locals) are NOT in the EVM calldata buffer.
		m_blk.fn.params.size());
}

} // namespace puyasol::builder::sol_ast
