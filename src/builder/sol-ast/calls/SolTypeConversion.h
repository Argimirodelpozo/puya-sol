#pragma once

#include "builder/sol-ast/SolFunctionCall.h"
#include "builder/sol-eb/TypeConversions.h"

namespace puyasol::builder::sol_ast
{

/// Type conversion calls: uint256(x), address(y), bytes32(z), etc.
///
/// Integer conversions use source/target solc facts in ConversionPlan; enum
/// conversions remain checked. Other categories use TypeConversionRegistry
/// and representation-specific fallbacks, sharing the once-lowered operand.
class SolTypeConversion: public SolFunctionCall
{
public:
	SolTypeConversion(
		eb::ContractContext& _ctx,
		solidity::frontend::FunctionCall const& _call);

	std::shared_ptr<awst::Expression> toAwst() override;

private:
	/// Handle enum range check: assert(x < numMembers)
	std::shared_ptr<awst::Expression> handleEnumConversion();

	/// Representation-specific fallback after category dispatch.
	std::shared_ptr<awst::Expression> handleGenericConversion(
		std::shared_ptr<awst::Expression> _value, awst::WType const* _targetType);

	/// address(0) → zero address constant
	std::shared_ptr<awst::Expression> tryAddressZeroConstant();

	/// Integer → bytes[N] conversion via itob + padding/truncation.
	std::shared_ptr<awst::Expression> handleIntToBytes(
		std::shared_ptr<awst::Expression> _expr, int _byteWidth);

	/// Biguint → bytes[N] conversion.
	std::shared_ptr<awst::Expression> handleBiguintToBytes(
		std::shared_ptr<awst::Expression> _expr, int _byteWidth);

	/// Extract last N bytes from an 8-byte itob result.
	std::shared_ptr<awst::Expression> extractLastN(
		std::shared_ptr<awst::Expression> _expr, int _n);
};

} // namespace puyasol::builder::sol_ast
