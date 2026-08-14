#pragma once

/// @file Context.h
/// Typed nested contexts for Solidity AST traversal.
///
/// Two state layers:
///  1. Lexical (unchecked, loop, placeholder, inConstructor) — on typed
///     per-scope contexts; resolved by parent-chain walk.
///  2. Decl-id-keyed (storage aliases, fn-ptrs, MRO targets, etc.) — flat
///     ScopeState owned by TranslationContext; O(1) lookup, no virtual dispatch.
///
/// Context caches a ScopeState* so every level reaches the same flat state.

#include "awst/Node.h"
#include "builder/SourceLocConvert.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/ReturnWirePlan.h"

#include <libsolidity/ast/AST.h>

#include <map>
#include <set>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <unordered_set>

namespace puyasol::builder
{
namespace eb { class ContractContext; }
}

namespace puyasol::builder::sol_ast
{

/// Modifier-inliner param remap entry: when a modifier is applied
/// multiple times in a single function, each instance's locals get a
/// unique mangled name with their original AWST type.
struct ParamRemap
{
	std::string name;
	awst::WType const* type;
};

/// Typed local storage-pointer alias (`T storage p = …`).
///
/// Shapes:
///   `mapping(K=>V) storage m = stateMap;` // MappingHolder (BytesConstant)
///   `T[] storage p = stateArr;`           // StateRead (StateGet)
///   `T storage e = container[i];`         // IndexedPath (IndexExpression)
///   `T storage f = s.field;`              // FieldPath (FieldExpression)
///   `(_, T storage e, _) = (...);`        // TupleSlice (TupleItemExpression)
///
/// The Kind tag makes the producer's intent explicit and gives consumers
/// a switch instead of a dynamic_cast ladder. expr must match the tag;
/// use the named factory methods to uphold that invariant.
struct StorageAlias
{
	enum class Kind
	{
		MappingHolder,   ///< BytesConstant — runtime mapping holder name
		StateRead,       ///< StateGet wrapping a state-var read (or, post
		                 ///<   `makeWritableTarget`, the bare BoxValueExpression /
		                 ///<   AppStateExpression)
		IndexedPath,     ///< IndexExpression into a state container
		FieldPath,       ///< FieldExpression onto a state-struct field
		TupleSlice,      ///< TupleItemExpression — destructured tuple element
	};

	Kind kind;
	std::shared_ptr<awst::Expression> expr;

	static StorageAlias mappingHolder(std::shared_ptr<awst::Expression> _e)
		{ return {Kind::MappingHolder, std::move(_e)}; }
	static StorageAlias stateRead(std::shared_ptr<awst::Expression> _e)
		{ return {Kind::StateRead, std::move(_e)}; }
	static StorageAlias indexedPath(std::shared_ptr<awst::Expression> _e)
		{ return {Kind::IndexedPath, std::move(_e)}; }
	static StorageAlias fieldPath(std::shared_ptr<awst::Expression> _e)
		{ return {Kind::FieldPath, std::move(_e)}; }
	static StorageAlias tupleSlice(std::shared_ptr<awst::Expression> _e)
		{ return {Kind::TupleSlice, std::move(_e)}; }
};

/// Flat translation-time scope state owned by TranslationContext. All
/// decl-id-keyed bindings live here. Decl IDs are globally unique so maps
/// grow monotonically and are inert between functions; no per-block reset needed.
struct ScopeState
{
	/// Local `T storage p = …` aliases. Tag + expression; see StorageAlias.
	std::unordered_map<int64_t, StorageAlias> storageAliases;

	/// Local fn-ptr variable → its FunctionDefinition. SolInternalCall
	/// uses this to lower `f()` through a fn-ptr local as a direct callsub.
	std::unordered_map<int64_t, solidity::frontend::FunctionDefinition const*> funcPtrTargets;

	/// Slot-based storage refs for local pointers (`T storage p = base[i]`).
	std::unordered_map<int64_t, std::shared_ptr<awst::Expression>> slotStorageRefs;

	/// Function param/return decl ID → its name as a runtime bytes value
	/// (used as the box-key prefix for a `mapping(K=>V) storage` param).
	std::unordered_map<int64_t, std::string> mappingKeyParams;

	/// Struct storage-ref param decl ID → the name of its companion uint64 OFFSET param
	/// (handle-model dual handle). Present only for "offset-convention" params (those that
	/// receive an array-element ref `f(arr[i])` somewhere): `s.field` ops then hit the element
	/// slice via box_replace/box_extract(key, offset+fieldOff). Absent → whole-box (offset 0).
	std::unordered_map<int64_t, std::string> structRefOffsets;

	/// >4096 B memory aggregate: decl ID → uint64 local for EVM-memory base
	/// offset (FMP at allocation). Lives in multi-slot blob; `t.field[i]`
	/// lowers to blob read/write at base + offset. See SolIndexAccess.
	std::unordered_map<int64_t, std::string> blobAggregates;

	/// Memory-aggregate alias (handle-model copy-elision): decl ID → the source
	/// expression it aliases. `T memory b = a` registers b→a (only when neither is
	/// later reassigned) so b's references resolve to a's local — memory→memory
	/// ALIASES (EVM) instead of copying. Resolved in SolIdentifier before the var read.
	std::unordered_map<int64_t, std::shared_ptr<awst::Expression>> memoryAliases;

	/// Memory aggregate locals used as Yul pointer values in inline assembly.
	/// Promoted to blob-backed (pre-scan in ContractBuilder::buildBlock).
	std::unordered_set<int64_t> assemblyAggregates;

	/// Modifier-inliner param remap: unique mangled names per expansion
	/// when the same modifier is applied multiple times. Set/erased by the inliner.
	std::unordered_map<int64_t, ParamRemap> paramRemaps;

	/// `super.X()` MRO: decl ID → mangled name. Set per-function, cleared between bodies.
	std::unordered_map<int64_t, std::string> superTargetNames;
};

/// Common base for every scope level. Upward parent pointer for lexical
/// walks; cached ScopeState* to the flat decl-id-keyed state at the root.
/// Virtual destructor for delete-through-base-ptr.
class Context
{
public:
	virtual ~Context() = default;

	/// Walk one level up. Returns nullptr at the root (TranslationContext).
	Context* parent() const { return m_parent; }

	/// Flat scope state shared across the whole chain (owned by TranslationContext).
	ScopeState& scopeState() const { return *m_state; }

	// ── Lexical-scope state (parent-chain walks) ────────────────────

	/// True if any ancestor scope is inside an `unchecked { }` block.
	virtual bool isUnchecked() const
	{
		return m_parent && m_parent->isUnchecked();
	}

	/// True iff the enclosing function is a constructor (gates immutable writes, etc.).
	virtual bool isInConstructor() const
	{
		return m_parent && m_parent->isInConstructor();
	}

	/// The enclosing function's live-calldata-pointer set (see FunctionContext::
	/// seededCalldataPointers); nullptr outside a function scope. Parent-chain walk
	/// like isInConstructor so eb-level builders (SolIdentifier) can reach it.
	virtual std::set<std::string>* liveCalldataPointers() const
	{
		return m_parent ? m_parent->liveCalldataPointers() : nullptr;
	}

	// ── Decl-id-keyed lookups (O(1) flat) ───────────────────────────

	StorageAlias const* findStorageAlias(int64_t _declId) const
	{
		auto it = m_state->storageAliases.find(_declId);
		return it != m_state->storageAliases.end() ? &it->second : nullptr;
	}

	std::shared_ptr<awst::Expression> findMemoryAlias(int64_t _declId) const
	{
		auto it = m_state->memoryAliases.find(_declId);
		return it != m_state->memoryAliases.end() ? it->second : nullptr;
	}

	solidity::frontend::FunctionDefinition const* findFuncPtrTarget(int64_t _declId) const
	{
		auto it = m_state->funcPtrTargets.find(_declId);
		return it != m_state->funcPtrTargets.end() ? it->second : nullptr;
	}

	std::shared_ptr<awst::Expression> findSlotStorageRef(int64_t _declId) const
	{
		auto it = m_state->slotStorageRefs.find(_declId);
		return it != m_state->slotStorageRefs.end() ? it->second : nullptr;
	}

	std::string findStructRefOffset(int64_t _declId) const
	{
		auto it = m_state->structRefOffsets.find(_declId);
		return it != m_state->structRefOffsets.end() ? it->second : std::string{};
	}

	std::string findMappingKeyParam(int64_t _declId) const
	{
		auto it = m_state->mappingKeyParams.find(_declId);
		return it != m_state->mappingKeyParams.end() ? it->second : std::string{};
	}

	/// Returns the runtime base-offset local name for a blob-backed memory
	/// aggregate, or empty if `_declId` is not a blob aggregate.
	std::string findBlobAggregate(int64_t _declId) const
	{
		auto it = m_state->blobAggregates.find(_declId);
		return it != m_state->blobAggregates.end() ? it->second : std::string{};
	}

	/// Is this decl a memory aggregate used as a value in inline assembly?
	bool isAssemblyAggregate(int64_t _declId) const
	{
		return m_state->assemblyAggregates.count(_declId) > 0;
	}

	ParamRemap const* findParamRemap(int64_t _declId) const
	{
		auto it = m_state->paramRemaps.find(_declId);
		return it != m_state->paramRemaps.end() ? &it->second : nullptr;
	}

	std::string findSuperTarget(int64_t _declId) const
	{
		auto it = m_state->superTargetNames.find(_declId);
		return it != m_state->superTargetNames.end() ? it->second : std::string{};
	}

	// ── Mutators (direct map ops on the shared state) ───────────────

	void setStorageAlias(int64_t _declId, StorageAlias _alias)
	{
		m_state->storageAliases[_declId] = std::move(_alias);
	}

	void setMemoryAlias(int64_t _declId, std::shared_ptr<awst::Expression> _expr)
	{
		m_state->memoryAliases[_declId] = std::move(_expr);
	}

	void setFuncPtrTarget(int64_t _declId,
		solidity::frontend::FunctionDefinition const* _target)
	{
		m_state->funcPtrTargets[_declId] = _target;
	}

	void eraseFuncPtrTarget(int64_t _declId)
	{
		m_state->funcPtrTargets.erase(_declId);
	}

	void setSlotStorageRef(int64_t _declId, std::shared_ptr<awst::Expression> _expr)
	{
		m_state->slotStorageRefs[_declId] = std::move(_expr);
	}

	void setMappingKeyParam(int64_t _declId, std::string _name)
	{
		m_state->mappingKeyParams[_declId] = std::move(_name);
	}

	void setStructRefOffset(int64_t _declId, std::string _offsetVarName)
	{
		m_state->structRefOffsets[_declId] = std::move(_offsetVarName);
	}

	void setBlobAggregate(int64_t _declId, std::string _offsetVar)
	{
		m_state->blobAggregates[_declId] = std::move(_offsetVar);
	}

	void markAssemblyAggregate(int64_t _declId)
	{
		m_state->assemblyAggregates.insert(_declId);
	}

	/// Toggle the enclosing FunctionContext's inConstructor flag.
	void setInConstructor(bool _flag);

	void setParamRemap(int64_t _declId, ParamRemap _remap)
	{
		m_state->paramRemaps[_declId] = std::move(_remap);
	}

	void eraseParamRemap(int64_t _declId)
	{
		m_state->paramRemaps.erase(_declId);
	}

	void setSuperTarget(int64_t _declId, std::string _name)
	{
		m_state->superTargetNames[_declId] = std::move(_name);
	}

	void clearSuperTargets()
	{
		m_state->superTargetNames.clear();
	}

	std::unordered_map<int64_t, std::string> const& allSuperTargets() const
	{
		return m_state->superTargetNames;
	}

	/// AWST local name: params keep bare name (ABI-facing); locals/catch params
	/// mangle to `name__<declId>` to prevent shadow collisions in the flat AWST frame.
	std::string awstVarName(solidity::frontend::VariableDeclaration const& _vd) const;

protected:
	Context(Context* _parent, ScopeState* _state)
		: m_parent(_parent), m_state(_state) {}
	/// Inheriting-state constructor for child scopes — picks up the
	/// parent's flat ScopeState pointer.
	explicit Context(Context* _parent)
		: m_parent(_parent), m_state(_parent ? _parent->m_state : nullptr) {}

	Context* m_parent;
	ScopeState* m_state;
};

/// Top-level per-contract context. Owns the flat ScopeState all nested
/// contexts reach via the cached m_state pointer.
struct TranslationContext: Context
{
	eb::ContractContext& contractCtx;
	TypeMapper& typeMapper;
	std::string sourceFile;

	/// Flat decl-id-keyed scope state. Owned here so it lives for the
	/// lifetime of the contract translation.
	ScopeState scopeState_;

	TranslationContext(
		eb::ContractContext& _contractCtx,
		TypeMapper& _typeMapper,
		std::string _sourceFile
	)
		: Context(nullptr, nullptr),
		  contractCtx(_contractCtx),
		  typeMapper(_typeMapper),
		  sourceFile(std::move(_sourceFile))
	{
		// Wire m_state after construction (scopeState_ declared after base).
		m_state = &scopeState_;
	}

	// Non-copyable/non-movable: m_state points into scopeState_ (dangling after move).
	// Use optional::emplace(args...) not optional::emplace(TranslationContext{args...}).
	TranslationContext(TranslationContext const&) = delete;
	TranslationContext(TranslationContext&&) = delete;
	TranslationContext& operator=(TranslationContext const&) = delete;
	TranslationContext& operator=(TranslationContext&&) = delete;

	awst::SourceLocation makeLoc(solidity::langutil::SourceLocation const& _sl) const
	{
		return typeMapper.sourceMap().toAwstLoc(sourceFile, _sl);
	}

	awst::SourceLocation makeLoc(int _start, int _end) const
	{
		return typeMapper.sourceMap().toAwstLoc(sourceFile, _start, _end);
	}
};

/// Function-level context: signature info needed to translate the body.
struct FunctionContext: Context
{
	TranslationContext& tr;
	std::vector<std::pair<std::string, awst::WType const*>> params;
	awst::WType const* returnType = nullptr;
	std::map<std::string, unsigned> paramBitWidths;
	/// solc callable AST identity, used to scope synthesized helpers.
	int64_t callableId = 0;
	/// Declared solc param types by BARE name (possible_solc item 2); feeds
	/// AssemblyBuilder's EVM-ABI calldata layout. Assigned after construction.
	std::map<std::string, solidity::frontend::Type const*> paramSolTypes;

	/// Struct storage-ref params passed as a box-key handle (bytes) because the
	/// body uses `param.slot` in asm (solady storage-lib idiom). name → the ARC4
	/// struct wtype, so `param.slot` resolves to a BoxValueExpression over the
	/// param's box key. Assigned after construction (buildFreestandingSubroutine).
	std::map<std::string, awst::WType const*> boxKeyStructParams;

	/// True iff this function is a constructor body (or is being inlined
	/// into one). Set by ApprovalProgramBuilder around constructor inlining.
	bool inConstructor = false;

	/// Internal/private function: assembly `return(o,s)` exits the whole program.
	/// Public/external functions are their own frame (AssemblyBuilder::setFrameIsProgram).
	bool frameIsProgram = false;

	/// Build-time ABI return encoding (fable-review-2 D2). When set, SolReturnStatement
	/// encodes each `return` value to its ABI wire type as it builds the statement —
	/// instead of the ReturnRewriter post-pass walking the finished body to do it. The
	/// function builder populates these for non-modifier ABI-boundary methods before
	/// translating the body; `returnWirePlan` is per return element (see ReturnWirePlan.h),
	/// `returnAsmWrap` requests the `% 2^N` wrap asm bodies need (Yul is unchecked).
	bool encodeReturnsAtBuildTime = false;
	bool returnAsmWrap = false;
	std::vector<ReturnWireElem> returnWirePlan;

	/// Calldata params whose mutable (__cd_off_x, __cd_len_x) pointer locals are
	/// LIVE — seeded at an assembly block's entry or written via `x.offset := V`.
	/// Shared across the function's per-block AssemblyBuilders (else every block
	/// would re-seed from the canonical blob, clobbering an earlier block's write —
	/// calldata_offset_read_write) AND consulted by value reads of the param
	/// (SolIdentifier / the implicit-return synth read `extract3(__cd_blob, off,
	/// len)` instead of the decoded param). Points at ContractBuilder's per-function
	/// scratch so it OUTLIVES buildBlock (the implicit-return synth runs after);
	/// falls back to the owned set on freestanding paths.
	std::set<std::string>* seededCalldataPointers = &ownSeededCalldataPointers;
	std::set<std::string> ownSeededCalldataPointers;


	FunctionContext(
		TranslationContext& _tr,
		std::vector<std::pair<std::string, awst::WType const*>> _params,
		awst::WType const* _returnType,
		std::map<std::string, unsigned> _paramBitWidths
	)
		: Context(&_tr),
		  tr(_tr),
		  params(std::move(_params)),
		  returnType(_returnType),
		  paramBitWidths(std::move(_paramBitWidths))
	{}

	bool isInConstructor() const override { return inConstructor; }
	std::set<std::string>* liveCalldataPointers() const override { return seededCalldataPointers; }
};

/// Control-flow targets for continue inside a loop.
/// `forLoopPost` is spliced before LoopContinue (the `i++` step).
/// `doWhileCondBreak` is the bottom-of-body condition for do/while.
/// At most one is set. Referenced laterally via BlockContext::enclosingLoop
/// (not in the parent chain).
struct LoopContext
{
	std::shared_ptr<awst::Statement> forLoopPost;
	std::shared_ptr<awst::Statement> doWhileCondBreak;
};

/// Block/scope-level context: nesting chain, enclosing loop (for
/// continue/break), modifier placeholder body (for `_;` inlining),
/// var-name shadowing.
struct BlockContext: Context
{
	/// Set when a statement in this block unconditionally halts (assembly
	/// return/revert). SolBlock skips remaining statements to avoid puya's
	/// "unreachable code" error.
	bool terminated = false;

	FunctionContext& fn;
	BlockContext* outer = nullptr;
	LoopContext const* enclosingLoop = nullptr;
	std::shared_ptr<awst::Block> placeholderBody;

	/// True iff this block is itself an `unchecked { }` block. The
	/// effective unchecked-status (this + any ancestor) is exposed via
	/// `isUnchecked()`, which walks the chain.
	bool unchecked = false;

	bool isUnchecked() const override
	{
		return unchecked || (m_parent && m_parent->isUnchecked());
	}

	BlockContext(
		FunctionContext& _fn,
		BlockContext* _outer,
		LoopContext const* _loop,
		std::shared_ptr<awst::Block> _placeholderBody
	)
		: Context(_outer ? static_cast<Context*>(_outer) : static_cast<Context*>(&_fn)),
		  fn(_fn),
		  outer(_outer),
		  enclosingLoop(_loop),
		  placeholderBody(std::move(_placeholderBody))
	{}

	/// Construct the top-level block (function body root).
	static BlockContext top(FunctionContext& _fn)
	{
		return {_fn, nullptr, nullptr, nullptr};
	}

	/// Derive a child block context — same enclosing loop & placeholder.
	BlockContext nest()
	{
		return {fn, this, enclosingLoop, placeholderBody};
	}

	/// Derive a context whose body is the body of `_loop`.
	BlockContext withLoop(LoopContext const& _loop)
	{
		BlockContext c = nest();
		c.enclosingLoop = &_loop;
		return c;
	}

	/// Derive a context for the body of a modifier-inlined function:
	/// `_;` placeholders splice in `_body`.
	/// Same block context, carrying a modifier placeholder body.
	///
	/// Deliberately a COPY, not `nest()`: the only call site is
	/// `BlockContext::top(fn).withPlaceholder(body)`, where nesting would set
	/// the child's parent to that TEMPORARY — which dies at the end of the
	/// full expression, leaving `m_parent` dangling. `isUnchecked()` then
	/// walks freed stack memory, and when the reused slot happens to hold a
	/// pointer back into the chain it recurses forever (stack-overflow SIGSEGV
	/// in multi_modifiers, latent for as long as this existed — whether it
	/// fires depends on unrelated code layout). A copy keeps the intended
	/// meaning (a top-level block that has a placeholder) with the parent the
	/// caller already owns.
	BlockContext withPlaceholder(std::shared_ptr<awst::Block> _body) const
	{
		BlockContext c = *this;
		c.placeholderBody = std::move(_body);
		return c;
	}

	// ── Convenience accessors (bridge to underlying ContractContext) ──

	eb::ContractContext& builderCtx() const { return fn.tr.contractCtx; }
	TypeMapper& typeMapper() const { return fn.tr.typeMapper; }
	std::string const& sourceFile() const { return fn.tr.sourceFile; }
	awst::SourceLocation makeLoc(solidity::langutil::SourceLocation const& _sl) const
	{
		return fn.tr.makeLoc(_sl);
	}
};

} // namespace puyasol::builder::sol_ast
