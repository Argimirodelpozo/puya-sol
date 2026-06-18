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

#include <libsolidity/ast/AST.h>

#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace puyasol::builder
{
class TypeMapper;
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
		awst::SourceLocation loc;
		loc.file = sourceFile;
		loc.line = _sl.start >= 0 ? _sl.start : 0;
		loc.endLine = _sl.end >= 0 ? _sl.end : 0;
		return loc;
	}

	awst::SourceLocation makeLoc(int _start, int _end) const
	{
		awst::SourceLocation loc;
		loc.file = sourceFile;
		loc.line = _start >= 0 ? _start : 0;
		loc.endLine = _end >= 0 ? _end : 0;
		return loc;
	}
};

/// Function-level context: signature info needed to translate the body.
struct FunctionContext: Context
{
	TranslationContext& tr;
	std::vector<std::pair<std::string, awst::WType const*>> params;
	awst::WType const* returnType = nullptr;
	std::map<std::string, unsigned> paramBitWidths;

	/// True iff this function is a constructor body (or is being inlined
	/// into one). Set by ApprovalProgramBuilder around constructor inlining.
	bool inConstructor = false;

	/// Internal/private function: assembly `return(o,s)` exits the whole program.
	/// Public/external functions are their own frame (AssemblyBuilder::setFrameIsProgram).
	bool frameIsProgram = false;

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
	BlockContext withPlaceholder(std::shared_ptr<awst::Block> _body)
	{
		BlockContext c = nest();
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
