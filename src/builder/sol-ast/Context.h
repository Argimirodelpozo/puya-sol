#pragma once

/// @file Context.h
/// Typed nested contexts for Solidity AST traversal.
///
/// Two layers of state:
///
///  1. **Lexical scope state** — `unchecked` blocks, var-name shadowing,
///     enclosing loop, modifier placeholder body, inConstructor flag.
///     These stay on the typed per-scope contexts (BlockContext,
///     FunctionContext) and resolve via parent-chain walks when looked
///     up. They genuinely depend on lexical nesting.
///
///  2. **Decl-id-keyed bindings** — storage aliases, fn-ptr targets,
///     constant-folded locals, slot-storage refs, mapping-key params,
///     modifier param remaps, super-call MRO targets. All keyed by
///     globally-unique AST decl IDs. These live in a single flat
///     `ScopeState` owned by the TranslationContext at the top of the
///     chain. Looked up in O(1) without virtual dispatch.
///
/// The `Context` base caches a `ScopeState*` so every level (Translation,
/// Function, Block) reaches the same flat state. `setX`/`findX`/`eraseX`
/// are non-virtual hashmap ops on it.

#include "awst/Node.h"

#include <libsolidity/ast/AST.h>

#include <map>
#include <memory>
#include <string>
#include <unordered_map>
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

/// Typed local storage-pointer alias.
///
/// Solidity lets you bind a local pointer to part of a state container:
///   `mapping(K=>V) storage m = stateMap;`    // MappingHolder
///   `T[] storage p = stateArr;`              // StateRead
///   `T storage e = container[i];`            // IndexedPath
///   `T storage f = s.field;`                 // FieldPath
///   `(_, T storage e, _) = (...);`           // TupleSlice (destructuring)
///
/// Each shape has a distinct expression form: a BytesConstant for the
/// mapping holder, a StateGet wrapping the state-var, an IndexExpression
/// for the indexed path, etc. Consumers that resolve a pointer use the
/// expression — but historically they had to `dynamic_cast` the
/// `shared_ptr<Expression>` to figure out *which* shape they got, with
/// the casts spread across SolIdentifier, SolIndexAccessHandlers,
/// SolArrayMethod, etc., and no compile-time signal of which shapes a
/// producer was allowed to register.
///
/// The `Kind` tag makes the producer's intent explicit at the call site
/// (via the small named factory methods below), keeps the existing
/// expression payload available for consumers that still need it, and
/// gives consumers a single enum to switch on instead of an
/// if/`dynamic_cast` ladder. The actual write-vs-read shape of `expr`
/// must match the tag — factories are the only sanctioned way to build
/// one to keep that invariant local.
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

/// Flat translation-time scope state. All decl-id-keyed bindings live
/// here in a single struct owned by TranslationContext at the chain
/// root. Decl IDs are globally unique, so the maps grow monotonically
/// over a single contract's translation and never collide across
/// functions; they're dropped when TranslationContext goes out of
/// scope. Per-block reset is unnecessary — when the next function
/// starts, it references its own (fresh) decl IDs and the prior
/// bindings are inert.
struct ScopeState
{
	/// Local storage-pointer alias: `T storage p = …`. The map value
	/// carries both the bound expression and a tag for the shape (see
	/// `StorageAlias`). Consumers resolve a pointer by reading the tag
	/// (cheap switch) and/or inspecting the expression.
	std::unordered_map<int64_t, StorageAlias> storageAliases;

	/// Local `function (…) returns (…)` variable → its bound
	/// FunctionDefinition. Used by SolInternalCall to lower an indirect
	/// `f()` call through a fn-ptr local as a direct callsub.
	std::unordered_map<int64_t, solidity::frontend::FunctionDefinition const*> funcPtrTargets;

	/// Folded compile-time constant value for a local declaration.
	/// Sentinel return is 0; reads check `>0`.
	std::unordered_map<int64_t, unsigned long long> constantLocals;

	/// Slot-based storage refs for local pointers (`T storage p = base[i]`).
	std::unordered_map<int64_t, std::shared_ptr<awst::Expression>> slotStorageRefs;

	/// Function param/return decl ID → its name as a runtime bytes value
	/// (used as the box-key prefix for a `mapping(K=>V) storage` param).
	std::unordered_map<int64_t, std::string> mappingKeyParams;

	/// Modifier-inliner param remap: when the same modifier is applied
	/// multiple times in a function, each instance's locals get a unique
	/// mangled name. Set/erased explicitly by the inliner around each
	/// expansion.
	std::unordered_map<int64_t, ParamRemap> paramRemaps;

	/// `super.X()` MRO resolution map: AST decl ID → mangled super name.
	/// Set up per-function before its body is translated; cleared between
	/// function bodies.
	std::unordered_map<int64_t, std::string> superTargetNames;
};

/// Common base for every scope level. Holds an upward parent pointer for
/// the lexical-scope walks and a cached `ScopeState*` to the chain root's
/// flat decl-id-keyed state. Non-virtual where possible; virtual
/// destructor so `delete` of a base pointer works once we start storing
/// them through Context*.
class Context
{
public:
	virtual ~Context() = default;

	/// Walk one level up. Returns nullptr at the root (TranslationContext).
	Context* parent() const { return m_parent; }

	/// The flat decl-id-keyed scope state shared across every level of
	/// the chain. Always points to the TranslationContext's owned
	/// ScopeState.
	ScopeState& scopeState() const { return *m_state; }

	// ── Lexical-scope state (parent-chain walks) ────────────────────

	/// True if any ancestor scope is inside an `unchecked { }` block.
	virtual bool isUnchecked() const
	{
		return m_parent && m_parent->isUnchecked();
	}

	/// Variable-name → AST decl ID for shadow detection. Returns 0 if the
	/// name isn't bound in any enclosing block. Inner-block bindings shadow
	/// outer ones (chain walk returns the innermost hit).
	virtual int64_t lookupVarId(std::string const& _name) const
	{
		return m_parent ? m_parent->lookupVarId(_name) : 0;
	}

	/// True iff the enclosing function is a constructor body. Constructor-only
	/// behaviour (e.g. immutable writes via direct app-global init) gates on
	/// this flag.
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

	solidity::frontend::FunctionDefinition const* findFuncPtrTarget(int64_t _declId) const
	{
		auto it = m_state->funcPtrTargets.find(_declId);
		return it != m_state->funcPtrTargets.end() ? it->second : nullptr;
	}

	unsigned long long findConstantLocal(int64_t _declId) const
	{
		auto it = m_state->constantLocals.find(_declId);
		return it != m_state->constantLocals.end() ? it->second : 0ULL;
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

	void setFuncPtrTarget(int64_t _declId,
		solidity::frontend::FunctionDefinition const* _target)
	{
		m_state->funcPtrTargets[_declId] = _target;
	}

	void eraseFuncPtrTarget(int64_t _declId)
	{
		m_state->funcPtrTargets.erase(_declId);
	}

	void setConstantLocal(int64_t _declId, unsigned long long _value)
	{
		m_state->constantLocals[_declId] = _value;
	}

	void setSlotStorageRef(int64_t _declId, std::shared_ptr<awst::Expression> _expr)
	{
		m_state->slotStorageRefs[_declId] = std::move(_expr);
	}

	void setMappingKeyParam(int64_t _declId, std::string _name)
	{
		m_state->mappingKeyParams[_declId] = std::move(_name);
	}

	/// Toggle the enclosing function's constructor flag. Walks the chain
	/// to find the FunctionContext that owns `inConstructor`.
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

	/// Get the AWST variable name for a declaration, handling shadowing.
	/// If the name is already taken by a different declaration in an outer
	/// block, appends "__<id>" to make it unique. Bindings are inserted
	/// into the innermost enclosing BlockContext.
	std::string resolveVarName(std::string const& _name, int64_t _declId);

	/// Look up the AWST variable name for a referenced declaration.
	/// Returns `_name__declId` if such a unique-name binding exists in
	/// any enclosing block, otherwise the bare `_name`.
	std::string lookupVarName(std::string const& _name, int64_t _declId) const;

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

/// Top-level translation context: per-contract state we share across
/// every function and statement. Owns the flat ScopeState that all
/// nested contexts reach via the cached `m_state` pointer.
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
		// Wire the base's ScopeState pointer to our owned state.
		// (Can't be done in the initializer list because scopeState_ is
		// declared after the base.)
		m_state = &scopeState_;
	}

	// Non-copyable / non-movable: m_state caches a pointer into our own
	// scopeState_ member, which would dangle after a move/copy. Force
	// in-place construction (e.g. `optional::emplace(args...)` instead of
	// `optional::emplace(TranslationContext{args...})`).
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

/// Loop-level context: control-flow targets for continue inside this loop.
/// `forLoopPost` is the post-step (e.g., `i++`) to splice in before
/// `LoopContinue`. `doWhileCondBreak` is the bottom-of-body condition check
/// for do/while. At most one of the two is set.
///
/// Currently *not* in the parent chain — referenced laterally through
/// `BlockContext::enclosingLoop`. We can weave it in later if loop-local
/// state ever needs scope lookup.
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
	FunctionContext& fn;
	BlockContext* outer = nullptr;
	LoopContext const* enclosingLoop = nullptr;
	std::shared_ptr<awst::Block> placeholderBody;

	/// True iff this block is itself an `unchecked { }` block. The
	/// effective unchecked-status (this + any ancestor) is exposed via
	/// `isUnchecked()`, which walks the chain.
	bool unchecked = false;

	/// Variable-name → AST decl ID for shadowing checks. Inserts go to the
	/// current block (the innermost when reading via `lookupVarId`). This
	/// genuinely needs lexical nesting, so it stays per-block.
	std::map<std::string, int64_t> varNameToId;

	bool isUnchecked() const override
	{
		return unchecked || (m_parent && m_parent->isUnchecked());
	}

	int64_t lookupVarId(std::string const& _name) const override
	{
		auto it = varNameToId.find(_name);
		if (it != varNameToId.end())
			return it->second;
		return m_parent ? m_parent->lookupVarId(_name) : 0;
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
