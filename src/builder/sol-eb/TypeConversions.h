#pragma once

#include "builder/sol-eb/NodeBuilder.h"

#include <libsolidity/ast/Types.h>

#include <functional>
#include <memory>
#include <unordered_map>

namespace puyasol::builder::eb
{

/// Dispatches non-integer Solidity conversions by target type category.
/// Integer conversions use the source-aware ConversionPlan.
class TypeConversionRegistry
{
public:
	using ConvertHandler = std::function<std::unique_ptr<InstanceBuilder>(
		ContractContext& _ctx,
		solidity::frontend::Type const* _targetSolType,
		awst::WType const* _targetWType,
		std::shared_ptr<awst::Expression> _arg,
		awst::SourceLocation const& _loc)>;

	TypeConversionRegistry();

	/// Try to handle a type conversion.
	/// Returns nullptr if not handled by these category-specific handlers.
	std::unique_ptr<InstanceBuilder> tryConvert(
		ContractContext& _ctx,
		solidity::frontend::Type const* _targetSolType,
		awst::WType const* _targetWType,
		std::shared_ptr<awst::Expression> _arg,
		awst::SourceLocation const& _loc) const;

private:
	std::unordered_map<int, ConvertHandler> m_handlers;

	void registerHandler(solidity::frontend::Type::Category _cat, ConvertHandler _handler);

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
