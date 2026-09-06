#pragma once

#include "awst/Node.h"
#include "builder/ScratchLayout.h"
#include "builder/sol-types/TypeMapper.h"

#include <libsolidity/ast/ASTForward.h>
#include "builder/sol-types/SolcFwd.h"

#include <map>
#include <string>
#include <vector>

namespace puyasol::builder
{

/// Transient state variables (`transient T x;`, EIP-1153).
///
/// AVM: packed into the scratch slot right after the memory pages (bzero'd
/// in the approval preamble). Scratch is per-txn, so the blob clears
/// between top-level app calls and persists across callsub within one call.
/// Logical slots/offsets come directly from solc's transient layout. Native
/// address high bytes live in a separate shadow, never in a logical word.
/// buildRead/buildWrite emit load/store intrinsics directly so writes
/// aren't DCE'd and are visible to callsub frames within the same call.
class TransientStorage
{
public:
	static constexpr unsigned SLOT_SIZE = 32; // bytes per slot (EVM word)
	static constexpr unsigned MAX_SLOTS = 5;  // declared logical words; raw Yul has 128

	struct TransientVar
	{
		unsigned slot;       // transient-namespace slot (independent from regular storage)
		unsigned byteOffset; // byte offset within the slot
		unsigned byteSize;   // width in bytes
		bool hasAddressShadow = false;
		awst::WType const* wtype;
		// Canonical scalar facts for the shared word codec, including UDVTs.
		// Owned by the Solidity AST (lifetime = compilation).
		solidity::frontend::Type const* solType = nullptr;
	};

	/// Collect solc's transient declarations and packed layout. Also latches the
	/// scratch slot the blob lives in — the slot right after the memory pages,
	/// so it depends on --evm-memory-slots (ScratchLayout::transientSlot).
	void collectVars(solidity::frontend::ContractDefinition const& _contract, TypeMapper& _typeMapper);

	/// Scratch slot holding the packed transient blob.
	int scratchSlot() const { return m_scratchSlot; }
	int addressShadowSlot() const { return ScratchLayout::transientAddressShadowSlot; }
	unsigned addressShadowSize() const { return m_hasAddressShadow ? m_totalSlots * 12 : 0; }

	/// True if the contract has any transient variables.
	bool hasTransientVars() const { return !m_vars.empty(); }

	/// True iff _var is a tracked transient state variable.
	bool isTransient(solidity::frontend::VariableDeclaration const& _var) const;

	/// Variable layout by AST declaration id, or nullptr.
	TransientVar const* getVarInfoById(int64_t _declId) const;

	/// Read expression for a transient variable: extract + type coercion.
	/// Declaration identity is required because inherited contracts may contain
	/// distinct transient variables with the same source name.
	std::shared_ptr<awst::Expression> buildRead(
		solidity::frontend::VariableDeclaration const& _var,
		awst::SourceLocation const& _loc) const;

	/// Write statement for a transient variable (truncates to declared byte width).
	std::shared_ptr<awst::Statement> buildWrite(
		solidity::frontend::VariableDeclaration const& _var,
		std::shared_ptr<awst::Expression> _value,
		awst::SourceLocation const& _loc) const;

	/// A raw word write replaces the complete logical slot, so any address in
	/// it becomes a zero-extended EVM-domain address. Other slots keep their
	/// native high bytes. `_slot` must already be pinned by the caller.
	void clearAddressShadowForWord(
		std::shared_ptr<awst::Expression> const& _slot,
		std::vector<std::shared_ptr<awst::Statement>>& _out,
		awst::SourceLocation const& _loc) const;

private:
	int m_scratchSlot = 5;
	std::vector<TransientVar> m_vars;
	std::map<int64_t, size_t> m_varById;
	unsigned m_totalSlots = 0;
	bool m_hasAddressShadow = false;
};

} // namespace puyasol::builder
