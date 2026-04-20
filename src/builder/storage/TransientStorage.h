#pragma once

#include "awst/Node.h"
#include "builder/sol-types/TypeMapper.h"

#include <libsolidity/ast/AST.h>

#include <map>
#include <string>
#include <vector>

namespace puyasol::builder
{

/// Manages Solidity transient state variables (`transient T x;`).
///
/// Transient storage resets per-transaction (EVM EIP-1153). On AVM the
/// blob lives in scratch slot AssemblyBuilder::TRANSIENT_SLOT, bzero'd
/// once per app call in the approval preamble — scratch is per-txn, so
/// the blob clears implicitly between top-level app calls and persists
/// across callsub within one app call.
///
/// Layout: | var0 (32B) | var1 (32B) | ... | varN (32B) |
///
/// buildRead / buildWrite emit load/store intrinsics against the scratch
/// slot directly, so named-transient-var writes can't be DCE'd and are
/// visible to subsequent `this.f()` callsub frames within the same call.
class TransientStorage
{
public:
	static constexpr unsigned SLOT_SIZE = 32; // bytes per slot (EVM word)
	static constexpr unsigned MAX_SLOTS = 5;

	/// Collect transient state variables from a contract and assign offsets.
	void collectVars(solidity::frontend::ContractDefinition const& _contract, TypeMapper& _typeMapper);

	/// Returns true if the contract has any transient variables.
	bool hasTransientVars() const { return !m_vars.empty(); }

	/// Get the total blob size in bytes.
	unsigned blobSize() const { return static_cast<unsigned>(m_vars.size()) * SLOT_SIZE; }

	/// Check if a variable declaration is a transient state variable we track.
	bool isTransient(solidity::frontend::VariableDeclaration const& _var) const;

	/// Get the byte offset for a transient variable. Returns -1 if not found.
	int getOffset(std::string const& _name) const;

	/// Build a read expression for a transient variable.
	/// Extracts 32 bytes at the variable's offset, reinterprets as the target type.
	std::shared_ptr<awst::Expression> buildRead(
		std::string const& _name, awst::WType const* _type,
		awst::SourceLocation const& _loc) const;

	/// Build a write statement for a transient variable.
	/// Replaces 32 bytes at the variable's offset in the blob.
	std::shared_ptr<awst::Statement> buildWrite(
		std::string const& _name, std::shared_ptr<awst::Expression> _value,
		awst::SourceLocation const& _loc) const;

private:
	struct TransientVar
	{
		std::string name;
		unsigned offset; // byte offset in blob
		awst::WType const* wtype;
	};

	std::vector<TransientVar> m_vars;
	std::map<std::string, unsigned> m_offsetMap; // name → index into m_vars
};

} // namespace puyasol::builder
