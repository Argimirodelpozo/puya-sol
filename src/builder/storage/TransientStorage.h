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
/// EVM semantics (EIP-1153): transient storage has its own independent
/// slot namespace (parallel to regular storage) and resets per-transaction.
/// On AVM the blob lives in scratch slot AssemblyBuilder::TRANSIENT_SLOT,
/// bzero'd once per app call in the approval preamble — scratch is
/// per-txn, so the blob clears implicitly between top-level app calls and
/// persists across callsub within one app call.
///
/// Packing follows the same rules as regular storage (StorageLayout.cpp):
/// small values (uint8, int8, address, etc.) share a 32-byte slot when
/// they fit, dynamic types start new slots, etc.
///
/// buildRead / buildWrite emit load/store intrinsics against the scratch
/// slot directly, so named-transient-var writes can't be DCE'd and are
/// visible to subsequent `this.f()` callsub frames within the same call.
class TransientStorage
{
public:
	static constexpr unsigned SLOT_SIZE = 32; // bytes per slot (EVM word)
	static constexpr unsigned MAX_SLOTS = 5;  // blob = 5 * 32 = 160 bytes

	struct TransientVar
	{
		std::string name;
		int64_t declId;
		unsigned slot;       // transient-namespace slot number (independent from regular storage)
		unsigned byteOffset; // byte offset within the slot
		unsigned byteSize;   // width in bytes
		awst::WType const* wtype;
	};

	/// Collect transient state variables from a contract and compute packed layout.
	void collectVars(solidity::frontend::ContractDefinition const& _contract, TypeMapper& _typeMapper);

	/// Returns true if the contract has any transient variables.
	bool hasTransientVars() const { return !m_vars.empty(); }

	/// Number of slots used (0-based count, independent from regular storage).
	unsigned totalSlots() const { return m_totalSlots; }

	/// Blob byte size consumed (totalSlots * 32).
	unsigned blobSize() const { return m_totalSlots * SLOT_SIZE; }

	/// Check if a variable declaration is a transient state variable we track.
	bool isTransient(solidity::frontend::VariableDeclaration const& _var) const;

	/// Get variable layout info by name, or nullptr if unknown.
	TransientVar const* getVarInfo(std::string const& _name) const;

	/// Get variable layout info by AST declaration id, or nullptr if unknown.
	TransientVar const* getVarInfoById(int64_t _declId) const;

	/// Build a read expression for a transient variable. Emits the appropriate
	/// byte-width extract + coercion for the declared type (with narrowing for
	/// packed small types).
	std::shared_ptr<awst::Expression> buildRead(
		std::string const& _name, awst::WType const* _type,
		awst::SourceLocation const& _loc) const;

	/// Build a write statement for a transient variable. Truncates the value
	/// to the declared byte width before storing.
	std::shared_ptr<awst::Statement> buildWrite(
		std::string const& _name, std::shared_ptr<awst::Expression> _value,
		awst::SourceLocation const& _loc) const;

private:
	std::vector<TransientVar> m_vars;
	std::map<std::string, size_t> m_varByName;
	std::map<int64_t, size_t> m_varById;
	unsigned m_totalSlots = 0;
};

} // namespace puyasol::builder
