#pragma once

#include "builder/eb/NodeBuilder.h"

#include <libsolidity/ast/Types.h>

#include <memory>

namespace puyasol::builder::eb
{

/// Dispatches non-integer Solidity conversions by target type category.
/// Integer conversions use the source-aware ConversionPlan.
class TypeConversions
{
public:
	/// Try to handle a type conversion.
	/// Returns nullptr if not handled by these category-specific handlers.
	static std::unique_ptr<InstanceBuilder> tryConvert(
		ContractContext& _ctx,
		solidity::frontend::Type const* _targetSolType,
		awst::WType const* _targetWType,
		std::shared_ptr<awst::Expression> _arg,
		awst::SourceLocation const& _loc);

private:
	// Handlers
	static std::unique_ptr<InstanceBuilder> convertToBool(
		ContractContext& _ctx,
		solidity::frontend::Type const* _targetSolType,
		awst::WType const* _targetWType,
		std::shared_ptr<awst::Expression> _arg,
		awst::SourceLocation const& _loc);

	static std::unique_ptr<InstanceBuilder> convertToAddress(
		ContractContext& _ctx,
		solidity::frontend::Type const* _targetSolType,
		awst::WType const* _targetWType,
		std::shared_ptr<awst::Expression> _arg,
		awst::SourceLocation const& _loc);

	static std::unique_ptr<InstanceBuilder> convertToFixedBytes(
		ContractContext& _ctx,
		solidity::frontend::Type const* _targetSolType,
		awst::WType const* _targetWType,
		std::shared_ptr<awst::Expression> _arg,
		awst::SourceLocation const& _loc);
};

} // namespace puyasol::builder::eb
