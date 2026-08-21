#pragma once

#include "awst/Node.h"
#include "builder/sol-types/TypeMapper.h"

#include <libsolidity/ast/AST.h>

#include <map>
#include <string>
#include <vector>

namespace puyasol::builder
{

/// Transient state variables (`transient T x;`, EIP-1153).
///
/// AVM: packed into scratch slot AssemblyBuilder::TRANSIENT_SLOT (bzero'd
/// in the approval preamble). Scratch is per-txn, so the blob clears
/// between top-level app calls and persists across callsub within one call.
/// Packing rules match StorageLayout.cpp (same slot-sharing / alignment).
/// buildRead/buildWrite emit load/store intrinsics directly so writes
/// aren't DCE'd and are visible to callsub frames within the same call.
class TransientStorage
{
public:
	static constexpr unsigned SLOT_SIZE = 32; // bytes per slot (EVM word)
	static constexpr unsigned MAX_SLOTS = 5;  // blob = 5 * 32 = 160 bytes

	struct TransientVar
	{
		std::string name;
		int64_t declId;
		unsigned slot;       // transient-namespace slot (independent from regular storage)
		unsigned byteOffset; // byte offset within the slot
		unsigned byteSize;   // width in bytes
		awst::WType const* wtype;
		// Kept for buildRead sign-extension of signed sub-256 (e.g. int128).
		// Owned by the Solidity AST (lifetime = compilation).
		solidity::frontend::Type const* solType = nullptr;
	};

	/// Collect transient vars and compute packed layout.
	void collectVars(solidity::frontend::ContractDefinition const& _contract, TypeMapper& _typeMapper);

	/// True if the contract has any transient variables.
	bool hasTransientVars() const { return !m_vars.empty(); }

	/// Slot count (independent from regular storage).
	unsigned totalSlots() const { return m_totalSlots; }

	/// Blob size in bytes (totalSlots * 32).
	unsigned blobSize() const { return m_totalSlots * SLOT_SIZE; }

	/// True iff _var is a tracked transient state variable.
	bool isTransient(solidity::frontend::VariableDeclaration const& _var) const;

	/// Variable layout by name, or nullptr.
	TransientVar const* getVarInfo(std::string const& _name) const;

	/// Variable layout by AST declaration id, or nullptr.
	TransientVar const* getVarInfoById(int64_t _declId) const;

	/// Read expression for a transient variable: extract + type coercion.
	/// Declaration identity is required because inherited contracts may contain
	/// distinct transient variables with the same source name.
	std::shared_ptr<awst::Expression> buildRead(
		solidity::frontend::VariableDeclaration const& _var,
		awst::WType const* _type,
		awst::SourceLocation const& _loc) const;

	/// Write statement for a transient variable (truncates to declared byte width).
	std::shared_ptr<awst::Statement> buildWrite(
		solidity::frontend::VariableDeclaration const& _var,
		std::shared_ptr<awst::Expression> _value,
		awst::SourceLocation const& _loc) const;

private:
	std::vector<TransientVar> m_vars;
	std::map<std::string, size_t> m_varByName;
	std::map<int64_t, size_t> m_varById;
	unsigned m_totalSlots = 0;
};

} // namespace puyasol::builder
