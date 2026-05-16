#pragma once

/// @file StorageBackend.h
/// Single dispatch facade over the three Solidity-state backends:
///   - **AppGlobal** (`app_global_get`/`app_global_put`): fixed-shape
///     scalar / small-struct state that fits in a global key.
///   - **Box** (`box_get`/`box_put`/...): variable-shape and
///     per-mapping-entry state.
///   - **Transient** (scratch slot): `transient` keyword vars, packed
///     into a 5-slot blob with per-field byte-offset layout.
///
/// Before this facade, every consumer that wanted to read or write a
/// state variable had to do its own dispatch:
///
///     if (transient && transient->isTransient(var))
///         readExpr = transient->buildRead(name, type, loc);
///     else
///     {
///         auto kind = StorageMapper::shouldUseBoxStorage(var)
///             ? AppStorageKind::Box : AppStorageKind::AppGlobal;
///         readExpr = mapper.createStateRead(name, type, kind, loc);
///     }
///
/// This pattern recurs at ~7 sites across SolIdentifier, SolUnaryOperation,
/// SolAssignmentEarlyOuts, PublicGetterBuilder, etc. — each making the
/// caller aware of all three backends. Tag the dispatch with one method
/// per operation:
///
///     auto read = storageBackend.emitReadForVar(var, name, type, loc);
///     auto stmt = storageBackend.emitWriteForVar(var, name, value, loc);
///
/// Direct, kind-specific calls into StorageMapper (`createStateRead`
/// with an explicit `kind`) remain valid for sites that already know
/// the backend (compile-time constants, immutables, layout-specifier
/// paths) — this facade only adds a higher-level dispatch for sites
/// that work from a `VariableDeclaration`.

#include "awst/Node.h"

#include <libsolidity/ast/AST.h>

#include <memory>
#include <string>

namespace puyasol::builder
{

class StorageMapper;
class TransientStorage;

class StorageBackend
{
public:
	/// `_transient` may be nullptr for contracts with no transient
	/// state — dispatch then skips the isTransient probe.
	StorageBackend(StorageMapper& _mapper, TransientStorage const* _transient)
		: m_mapper(_mapper), m_transient(_transient)
	{
	}

	/// Dispatch a state-var read to the right backend.
	std::shared_ptr<awst::Expression> emitReadForVar(
		solidity::frontend::VariableDeclaration const& _var,
		std::string const& _name,
		awst::WType const* _type,
		awst::SourceLocation const& _loc) const;

	/// Dispatch a state-var write. Returns a Statement (transient backend
	/// emits load/store intrinsics); for the AppGlobal/Box backends the
	/// `createStateWrite` Expression is wrapped in an ExpressionStatement.
	std::shared_ptr<awst::Statement> emitWriteForVar(
		solidity::frontend::VariableDeclaration const& _var,
		std::string const& _name,
		std::shared_ptr<awst::Expression> _value,
		awst::SourceLocation const& _loc) const;

	/// True iff `_var` is a transient state variable known to the
	/// TransientStorage layout.
	bool isTransient(solidity::frontend::VariableDeclaration const& _var) const;

private:
	StorageMapper& m_mapper;
	TransientStorage const* m_transient;
};

} // namespace puyasol::builder
