#include "builder/assembly/YulPrePass.h"

#include <libyul/optimiser/Suite.h>
#include <libyul/optimiser/Disambiguator.h>
#include <libyul/optimiser/NameDispenser.h>
#include <libyul/optimiser/OptimiserStep.h>
#include <libyul/optimiser/ASTWalker.h>
#include <libyul/Dialect.h>
#include <libyul/AsmAnalysisInfo.h>
#include <libyul/YulString.h>

#include <cstdlib>
#include <set>
#include <variant>

using namespace solidity;
using namespace solidity::yul;

namespace puyasol::builder
{
static bool g_yulPrePass = false;
void setYulPrePass(bool _on) { g_yulPrePass = _on; }
bool yulPrePassEnabled()
{
	static bool const env = std::getenv("PUYA_SOL_YUL_PREPASS") != nullptr;
	return g_yulPrePass || env;
}
} // namespace puyasol::builder

namespace
{

using ExtInfo = frontend::InlineAssemblyAnnotation::ExternalIdentifierInfo;

/// Walks the optimised tree and re-keys every external-reference identifier by
/// its (reserved, hence stable) name. ASTWalker::operator()(Assignment) visits
/// variableNames, so assignment TARGETS (external writes) are re-keyed too.
struct ExtRefRebuilder: public ASTWalker
{
	std::map<std::string, ExtInfo> const& byName;
	std::map<Identifier const*, ExtInfo>& out;

	ExtRefRebuilder(std::map<std::string, ExtInfo> const& _byName, std::map<Identifier const*, ExtInfo>& _out):
		byName(_byName), out(_out) {}

	using ASTWalker::operator();
	void operator()(Identifier const& _id) override
	{
		auto it = byName.find(_id.name.str());
		if (it != byName.end())
			out.emplace(&_id, it->second);
	}
};

} // namespace

namespace puyasol::builder
{

YulPrePassResult runYulPrePass(
	Block const& _root,
	Dialect const& _dialect,
	AsmAnalysisInfo const& _analysisInfo,
	std::map<Identifier const*, ExtInfo> const& _externalRefs,
	std::string_view _stepAbbreviations
)
{
	// 1. Snapshot external names → info (a Yul external name maps to exactly one
	//    Solidity decl within a block) and the reserved-name set.
	std::map<std::string, ExtInfo> byName;
	std::set<YulName> reserved;
	for (auto const& [id, info]: _externalRefs)
	{
		byName.emplace(id->name.str(), info);
		reserved.insert(id->name);
	}

	// 2. Disambiguate — a fresh tree; reserved names survive verbatim.
	Block astRoot = std::get<Block>(Disambiguator(_dialect, _analysisInfo, reserved)(_root));

	// 3. Mandatory "hgfo" prelude, then the requested step subset.
	NameDispenser dispenser{_dialect, astRoot, reserved};
	OptimiserStepContext context{_dialect, dispenser, reserved, std::nullopt};
	OptimiserSuite suite(context);
	suite.runSequence("hgfo", astRoot);
	if (!_stepAbbreviations.empty())
		suite.runSequence(_stepAbbreviations, astRoot);

	// 4. Rebuild the pointer-keyed external-reference map against the new tree.
	YulPrePassResult result;
	result.block = std::make_unique<Block>(std::move(astRoot));
	ExtRefRebuilder rebuilder{byName, result.externalRefs};
	rebuilder(*result.block);
	return result;
}

} // namespace puyasol::builder
