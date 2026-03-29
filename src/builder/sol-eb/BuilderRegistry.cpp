#include "builder/sol-eb/BuilderRegistry.h"

namespace puyasol::builder::eb
{

void BuilderRegistry::registerInstance(
	solidity::frontend::Type::Category _category,
	InstanceFactory _factory)
{
	m_factories[static_cast<int>(_category)] = std::move(_factory);
}

std::unique_ptr<InstanceBuilder> BuilderRegistry::tryBuildInstance(
	BuilderContext& _ctx,
	solidity::frontend::Type const* _solType,
	std::shared_ptr<awst::Expression> _expr) const
{
	if (!_solType)
		return nullptr;

	auto it = m_factories.find(static_cast<int>(_solType->category()));
	if (it != m_factories.end())
		return it->second(_ctx, _solType, std::move(_expr));

	return nullptr;
}

} // namespace puyasol::builder::eb
