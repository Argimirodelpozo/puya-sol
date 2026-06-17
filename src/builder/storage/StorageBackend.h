#pragma once

/// @file StorageBackend.h
/// Dispatch facade over the three state backends (AppGlobal, Box, Transient).
/// Callers with a VariableDeclaration use emitReadForVar/emitWriteForVar;
/// sites that already know the backend call StorageMapper directly.

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
	/// `_transient` may be nullptr (no transient state); dispatch skips isTransient.
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

	/// Dispatch a state-var write. Returns a Statement; AppGlobal/Box wrap
	/// createStateWrite in an ExpressionStatement.
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
