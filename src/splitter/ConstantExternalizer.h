#pragma once

#include "awst/Node.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace puyasol::splitter
{

/// Detects large BytesConstant nodes in AWST subroutines and externalizes
/// them to box storage. Adds a `__load_constants` ABI method that reads
/// from a box and stores each constant chunk into a scratch space slot.
/// Other code reads constants via `gloads` from the __load_constants
/// transaction's scratch (previous txn in the group).
///
/// Calling convention:
///   Group txn N-1: app call to __load_constants() → stores to scratch
///   Group txn N:   app call to verify() → reads via gloads
class ConstantExternalizer
{
public:
	struct ExternalizedConstant
	{
		size_t scratchSlot;
		size_t boxOffset;
		size_t length;
		std::vector<uint8_t> value;
	};

	struct Result
	{
		bool didExternalize = false;
		std::string boxName;        // box key name (e.g., "__constants")
		std::vector<ExternalizedConstant> constants;
		size_t totalBoxSize = 0;
	};

	/// Externalize large constants from a contract and its subroutines.
	/// Constants exceeding sizeThreshold bytes are moved to box storage
	/// and replaced with gloads reads from the previous transaction's scratch.
	/// A __load_constants ABI method is added to the contract.
	Result externalize(
		awst::Contract& _contract,
		std::vector<std::shared_ptr<awst::RootNode>>& _subroutines,
		size_t _sizeThreshold = 2048
	);

private:
	// Scratch slot assignments start here (high range to avoid conflicts)
	static constexpr size_t ScratchSlotBase = 240;

	struct ConstantEntry
	{
		std::string hash;
		std::vector<uint8_t> value;
		size_t scratchSlot;
		size_t boxOffset;
	};

	std::map<std::string, ConstantEntry> m_constants; // hash → entry
	size_t m_nextSlot = ScratchSlotBase;
	size_t m_nextOffset = 0;

	/// Hash a byte vector for deduplication.
	static std::string hashBytes(std::vector<uint8_t> const& _bytes);

	/// Phase 1: Walk expressions to find and register large constants.
	void findConstants(awst::Expression const& _expr, size_t _threshold);
	void findConstantsInStatement(awst::Statement const& _stmt, size_t _threshold);
	void findConstantsInBlock(awst::Block const& _block, size_t _threshold);

	/// Phase 2: Replace large BytesConstant nodes with gloads reads.
	/// Returns the (possibly new) expression.
	std::shared_ptr<awst::Expression> replaceInExpression(
		std::shared_ptr<awst::Expression> _expr
	);
	void replaceInStatement(std::shared_ptr<awst::Statement>& _stmt);
	void replaceInBlock(std::shared_ptr<awst::Block>& _block);

	/// Build a gloads expression that reads from the previous transaction's
	/// scratch slot.
	std::shared_ptr<awst::Expression> buildGloadsRead(
		size_t _scratchSlot,
		awst::SourceLocation const& _loc
	);

	/// Build the __load_constants contract method.
	awst::ContractMethod buildLoadConstantsMethod(
		awst::SourceLocation const& _loc,
		std::string const& _contractId,
		std::string const& _boxName
	);
};

} // namespace puyasol::splitter
