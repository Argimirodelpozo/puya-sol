#pragma once

#include "builder/sol-eb/BuilderContext.h"
#include "builder/sol-eb/NodeBuilder.h"

#include <libsolidity/ast/Types.h>

#include <functional>
#include <memory>
#include <unordered_map>

namespace puyasol::builder::eb
{

/// Registry that maps Solidity types to builder factories.
///
/// Dispatches on `solidity::frontend::Type::Category`. The factory receives
/// the full Solidity type so it can inspect type parameters (bits, signedness,
/// element types, struct fields, etc.) to construct the appropriate builder.
///
/// During migration, tryBuildInstance() returns nullptr for unregistered
/// categories, allowing the old ExpressionBuilder code path to handle them.
class BuilderRegistry
{
public:
	/// Factory: (context, Solidity type, AWST expression) → instance builder.
	/// The factory receives the full Solidity type to inspect parameters.
	using InstanceFactory = std::function<std::unique_ptr<InstanceBuilder>(
		BuilderContext& _ctx,
		solidity::frontend::Type const* _solType,
		std::shared_ptr<awst::Expression> _expr)>;

	/// Register an instance builder factory for a Solidity type category.
	void registerInstance(
		solidity::frontend::Type::Category _category,
		InstanceFactory _factory);

	/// Try to create an InstanceBuilder for the given Solidity type and expression.
	/// Returns nullptr if no builder is registered for this type category.
	std::unique_ptr<InstanceBuilder> tryBuildInstance(
		BuilderContext& _ctx,
		solidity::frontend::Type const* _solType,
		std::shared_ptr<awst::Expression> _expr) const;

private:
	std::unordered_map<int, InstanceFactory> m_factories;
};

} // namespace puyasol::builder::eb
