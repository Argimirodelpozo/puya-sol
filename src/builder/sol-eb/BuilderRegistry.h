#pragma once

#include "builder/sol-eb/ContractContext.h"
#include "builder/sol-eb/NodeBuilder.h"

#include <libsolidity/ast/Types.h>

#include <functional>
#include <memory>
#include <unordered_map>

namespace puyasol::builder::eb
{

/// Maps Solidity type categories to InstanceBuilder factories.
/// tryBuildInstance() returns nullptr for unregistered categories (falls through).
class BuilderRegistry
{
public:
	BuilderRegistry();

	using InstanceFactory = std::function<std::unique_ptr<InstanceBuilder>(
		ContractContext& _ctx,
		solidity::frontend::Type const* _solType,
		std::shared_ptr<awst::Expression> _expr)>;

	/// Register an instance builder factory for a Solidity type category.
	void registerInstance(
		solidity::frontend::Type::Category _category,
		InstanceFactory _factory);

	/// Try to create an InstanceBuilder for the given Solidity type and expression.
	/// Returns nullptr if no builder is registered for this type category.
	std::unique_ptr<InstanceBuilder> tryBuildInstance(
		ContractContext& _ctx,
		solidity::frontend::Type const* _solType,
		std::shared_ptr<awst::Expression> _expr) const;

private:
	std::unordered_map<int, InstanceFactory> m_factories;
};

} // namespace puyasol::builder::eb
