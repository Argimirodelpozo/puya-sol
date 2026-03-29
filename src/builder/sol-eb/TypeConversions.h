#pragma once

#include "builder/sol-eb/NodeBuilder.h"

#include <libsolidity/ast/Types.h>

#include <functional>
#include <memory>
#include <unordered_map>

namespace puyasol::builder::eb
{

/// Registry for Solidity type conversions (e.g., address(x), uint64(x), bytes32(x)).
///
/// Dispatches on the target Solidity type category. Each handler receives the
/// target Solidity type, the already-built argument expression, and produces
/// the converted expression.
class TypeConversionRegistry
{
public:
	using ConvertHandler = std::function<std::unique_ptr<InstanceBuilder>(
		BuilderContext& _ctx,
		solidity::frontend::Type const* _targetSolType,
		awst::WType const* _targetWType,
		std::shared_ptr<awst::Expression> _arg,
		awst::SourceLocation const& _loc)>;

	TypeConversionRegistry();

	/// Try to handle a type conversion.
	/// Returns nullptr if not handled (fall through to old code).
	std::unique_ptr<InstanceBuilder> tryConvert(
		BuilderContext& _ctx,
		solidity::frontend::Type const* _targetSolType,
		awst::WType const* _targetWType,
		std::shared_ptr<awst::Expression> _arg,
		awst::SourceLocation const& _loc) const;

private:
	std::unordered_map<int, ConvertHandler> m_handlers;

	void registerHandler(solidity::frontend::Type::Category _cat, ConvertHandler _handler);

	// Handlers
	static std::unique_ptr<InstanceBuilder> convertToInteger(
		BuilderContext& _ctx,
		solidity::frontend::Type const* _targetSolType,
		awst::WType const* _targetWType,
		std::shared_ptr<awst::Expression> _arg,
		awst::SourceLocation const& _loc);

	static std::unique_ptr<InstanceBuilder> convertToBool(
		BuilderContext& _ctx,
		solidity::frontend::Type const* _targetSolType,
		awst::WType const* _targetWType,
		std::shared_ptr<awst::Expression> _arg,
		awst::SourceLocation const& _loc);

	static std::unique_ptr<InstanceBuilder> convertToAddress(
		BuilderContext& _ctx,
		solidity::frontend::Type const* _targetSolType,
		awst::WType const* _targetWType,
		std::shared_ptr<awst::Expression> _arg,
		awst::SourceLocation const& _loc);

	static std::unique_ptr<InstanceBuilder> convertToFixedBytes(
		BuilderContext& _ctx,
		solidity::frontend::Type const* _targetSolType,
		awst::WType const* _targetWType,
		std::shared_ptr<awst::Expression> _arg,
		awst::SourceLocation const& _loc);

	static std::unique_ptr<InstanceBuilder> convertToEnum(
		BuilderContext& _ctx,
		solidity::frontend::Type const* _targetSolType,
		awst::WType const* _targetWType,
		std::shared_ptr<awst::Expression> _arg,
		awst::SourceLocation const& _loc);

	/// Left-pad a bytes expression to exactly N bytes.
	static std::shared_ptr<awst::Expression> leftPadToN(
		std::shared_ptr<awst::Expression> _expr,
		int _n,
		awst::SourceLocation const& _loc);
};

} // namespace puyasol::builder::eb
