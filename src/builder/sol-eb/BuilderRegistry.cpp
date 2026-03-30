#include "builder/sol-eb/BuilderRegistry.h"
#include "builder/sol-eb/SolIntegerBuilder.h"
#include "builder/sol-eb/SolBoolBuilder.h"
#include "builder/sol-eb/SolAddressBuilder.h"
#include "builder/sol-eb/SolArrayBuilder.h"
#include "builder/sol-eb/SolStringBuilder.h"
#include "builder/sol-eb/SolStructBuilder.h"
#include "builder/sol-eb/SolEnumBuilder.h"
#include "builder/sol-eb/SolFixedBytesBuilder.h"

namespace puyasol::builder::eb
{

BuilderRegistry::BuilderRegistry()
{
	using Cat = solidity::frontend::Type::Category;

	registerInstance(Cat::Integer, [](BuilderContext& ctx, solidity::frontend::Type const* solType, std::shared_ptr<awst::Expression> expr) -> std::unique_ptr<InstanceBuilder> {
		auto const* intType = dynamic_cast<solidity::frontend::IntegerType const*>(solType);
		if (!intType) return nullptr;
		return std::make_unique<SolIntegerBuilder>(ctx, intType, std::move(expr));
	});

	registerInstance(Cat::Bool, [](BuilderContext& ctx, solidity::frontend::Type const* solType, std::shared_ptr<awst::Expression> expr) -> std::unique_ptr<InstanceBuilder> {
		return std::make_unique<SolBoolBuilder>(ctx, std::move(expr));
	});

	registerInstance(Cat::Address, [](BuilderContext& ctx, solidity::frontend::Type const* solType, std::shared_ptr<awst::Expression> expr) -> std::unique_ptr<InstanceBuilder> {
		return std::make_unique<SolAddressBuilder>(ctx, solType, std::move(expr));
	});

	registerInstance(Cat::Enum, [](BuilderContext& ctx, solidity::frontend::Type const* solType, std::shared_ptr<awst::Expression> expr) -> std::unique_ptr<InstanceBuilder> {
		auto const* enumType = dynamic_cast<solidity::frontend::EnumType const*>(solType);
		if (!enumType) return nullptr;
		return std::make_unique<SolEnumBuilder>(ctx, enumType, std::move(expr));
	});

	registerInstance(Cat::FixedBytes, [](BuilderContext& ctx, solidity::frontend::Type const* solType, std::shared_ptr<awst::Expression> expr) -> std::unique_ptr<InstanceBuilder> {
		auto const* fbType = dynamic_cast<solidity::frontend::FixedBytesType const*>(solType);
		if (!fbType) return nullptr;
		return std::make_unique<SolFixedBytesBuilder>(ctx, fbType, std::move(expr));
	});

	registerInstance(Cat::Array, [](BuilderContext& ctx, solidity::frontend::Type const* solType, std::shared_ptr<awst::Expression> expr) -> std::unique_ptr<InstanceBuilder> {
		auto const* arrType = dynamic_cast<solidity::frontend::ArrayType const*>(solType);
		if (!arrType) return nullptr;
		return std::make_unique<SolArrayBuilder>(ctx, arrType, std::move(expr));
	});

	registerInstance(Cat::StringLiteral, [](BuilderContext& ctx, solidity::frontend::Type const* solType, std::shared_ptr<awst::Expression> expr) -> std::unique_ptr<InstanceBuilder> {
		return std::make_unique<SolStringBuilder>(ctx, solType, std::move(expr));
	});

	registerInstance(Cat::Struct, [](BuilderContext& ctx, solidity::frontend::Type const* solType, std::shared_ptr<awst::Expression> expr) -> std::unique_ptr<InstanceBuilder> {
		auto const* structType = dynamic_cast<solidity::frontend::StructType const*>(solType);
		if (!structType) return nullptr;
		return std::make_unique<SolStructBuilder>(ctx, structType, std::move(expr));
	});
}

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
