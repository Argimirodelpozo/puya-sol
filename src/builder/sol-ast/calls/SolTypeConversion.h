#pragma once

#include "builder/sol-ast/SolFunctionCall.h"
#include "builder/sol-eb/TypeConversions.h"

namespace puyasol::builder::sol_ast
{

/// Type conversion calls: uint256(x), address(y), bytes32(z), etc.
///
/// Dispatches to TypeConversionRegistry for most conversions. Falls back to
/// inline handling for edge cases (address(0) constant, complex bytes
/// conversions, narrowing casts with masking).
class SolTypeConversion: public SolFunctionCall
{
public:
	SolTypeConversion(
		eb::BuilderContext& _ctx,
		solidity::frontend::FunctionCall const& _call);

	std::shared_ptr<awst::Expression> toAwst() override;

private:
	/// Handle enum range check: assert(x < numMembers)
	std::shared_ptr<awst::Expression> handleEnumConversion();

	/// Try TypeConversionRegistry, then inline fallbacks.
	std::shared_ptr<awst::Expression> handleGenericConversion(awst::WType const* _targetType);

	/// address(0) → zero address constant
	std::shared_ptr<awst::Expression> tryAddressZeroConstant();

	/// Narrowing integer casts: mask to target bit width.
	std::shared_ptr<awst::Expression> applyNarrowingMask(
		std::shared_ptr<awst::Expression> _expr, awst::WType const* _targetType);

	/// Integer → bytes[N] conversion via itob + padding/truncation.
	std::shared_ptr<awst::Expression> handleIntToBytes(
		std::shared_ptr<awst::Expression> _expr, int _byteWidth);

	/// Biguint → bytes[N] conversion.
	std::shared_ptr<awst::Expression> handleBiguintToBytes(
		std::shared_ptr<awst::Expression> _expr, int _byteWidth);

	/// Left-pad bytes to N using concat(bzero(N), expr) then extract last N.
	std::shared_ptr<awst::Expression> leftPadToN(
		std::shared_ptr<awst::Expression> _expr, int _n);

	/// Extract last N bytes from an 8-byte itob result.
	std::shared_ptr<awst::Expression> extractLastN(
		std::shared_ptr<awst::Expression> _expr, int _n);
};

} // namespace puyasol::builder::sol_ast
