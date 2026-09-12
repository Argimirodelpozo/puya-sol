#pragma once

#include "builder/sol-ast/SolFunctionCall.h"

namespace puyasol::builder::sol_ast
{

/// Type conversion calls: uint256(x), address(y), bytes32(z), etc.
///
/// Integer conversions use source/target solc facts in ConversionPlan; enum
/// conversions remain checked. Other categories have one conversion owner.
class SolTypeConversion: public SolFunctionCall
{
public:
	using SolFunctionCall::SolFunctionCall;

	std::shared_ptr<awst::Expression> toAwst() override;

private:
	/// Handle enum range check: assert(x < numMembers)
	std::shared_ptr<awst::Expression> handleEnumConversion();

};

} // namespace puyasol::builder::sol_ast
