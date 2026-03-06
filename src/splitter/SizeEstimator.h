#pragma once

#include "awst/Node.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace puyasol::splitter
{

/// Estimates compiled TEAL program size from AWST nodes.
/// Walks the AWST tree and sums up approximate instruction counts
/// per node type, providing a per-method breakdown and total.
class SizeEstimator
{
public:
	struct Estimate
	{
		size_t totalInstructions = 0;
		size_t estimatedBytes = 0;
		std::map<std::string, size_t> methodSizes; // method name → instructions
	};

	/// Estimate the compiled size of a contract and its referenced subroutines.
	Estimate estimate(
		awst::Contract const& _contract,
		std::vector<std::shared_ptr<awst::RootNode>> const& _subroutines
	);

	// AVM size thresholds (with 3 extra pages = 4 pages × 2048 = 8192 bytes max)
	static constexpr size_t AVMMaxBytes = 8192;
	static constexpr size_t WarnThresholdBytes = 6000;
	static constexpr size_t SplitThresholdBytes = 14000;  // in estimatedBytes units (totalInstructions × 3)
	static constexpr size_t HelperTargetBytes = 8000;     // documentation only (not used in code)

	/// Estimate instruction count for a single expression.
	static size_t estimateExpression(awst::Expression const& _expr);

	/// Estimate instruction count for a single statement.
	static size_t estimateStatement(awst::Statement const& _stmt);

	/// Estimate instruction count for a block of statements.
	static size_t estimateBlock(awst::Block const& _block);

	/// Estimate the ABI codec overhead (argument decode + return encode)
	/// that puya will generate when a subroutine becomes an ABI method.
	/// Returns instruction count units (~2 bytes each).
	static size_t estimateABICodecCost(awst::Subroutine const& _sub);

	/// Estimate the ABI-encoded byte size of a WType.
	/// Returns the approximate number of raw data bytes needed for ARC4 encoding.
	static size_t estimateABIEncodedSize(awst::WType const* _wtype);

private:
	/// Estimate instruction count for a contract method.
	size_t estimateMethod(awst::ContractMethod const& _method);
};

} // namespace puyasol::splitter
