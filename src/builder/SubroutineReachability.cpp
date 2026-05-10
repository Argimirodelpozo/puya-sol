#include "builder/SubroutineReachability.h"
#include "splitter/AwstWalker.h"

#include <queue>
#include <set>
#include <unordered_map>

namespace puyasol::builder
{
namespace
{

/// Collect every SubroutineID reachable from a Block, using the generic
/// AwstWalker so we don't have to hand-write per-Expression-subclass
/// recursion. The walker visits every Expression slot in the block; we
/// pick out SubroutineCallExpression targets.
///
/// const_cast is safe here: our callback always returns nullptr, so the
/// walker only reads Expression nodes — no mutation occurs.
void collectRefs(awst::Block const& _block, std::set<std::string>& _refs)
{
	puyasol::splitter::walkBlock(const_cast<awst::Block&>(_block),
		[&_refs](awst::Expression const& e) -> std::shared_ptr<awst::Expression>
		{
			if (auto const* call = dynamic_cast<awst::SubroutineCallExpression const*>(&e))
				if (auto const* sid = std::get_if<awst::SubroutineID>(&call->target))
					_refs.insert(sid->target);
			return nullptr;
		});
}

void collectMethodRefs(awst::ContractMethod const& _method, std::set<std::string>& _refs)
{
	if (_method.body)
		collectRefs(*_method.body, _refs);
}

} // anonymous namespace

std::vector<std::shared_ptr<awst::RootNode>> filterToReachableSubroutines(
	std::vector<std::shared_ptr<awst::RootNode>> _roots)
{
	std::set<std::string> reachable;
	std::queue<std::string> worklist;

	// Seed with contract method references.
	for (auto const& root: _roots)
	{
		auto const* contract = dynamic_cast<awst::Contract const*>(root.get());
		if (!contract)
			continue;
		std::set<std::string> refs;
		collectMethodRefs(contract->approvalProgram, refs);
		collectMethodRefs(contract->clearProgram, refs);
		for (auto const& method: contract->methods)
			collectMethodRefs(method, refs);
		for (auto const& id: refs)
		{
			reachable.insert(id);
			worklist.push(id);
		}
	}

	// Build ID → Subroutine map for the worklist phase.
	std::unordered_map<std::string, awst::Subroutine const*> subMap;
	for (auto const& root: _roots)
		if (auto const* sub = dynamic_cast<awst::Subroutine const*>(root.get()))
			subMap[sub->id] = sub;

	// Transitively find all reachable subroutines.
	while (!worklist.empty())
	{
		std::string id = worklist.front();
		worklist.pop();
		auto it = subMap.find(id);
		if (it == subMap.end())
			continue;
		std::set<std::string> refs;
		if (it->second->body)
			collectRefs(*it->second->body, refs);
		for (auto const& ref: refs)
		{
			if (reachable.find(ref) == reachable.end())
			{
				reachable.insert(ref);
				worklist.push(ref);
			}
		}
	}

	// Filter: keep contracts (always) + reachable subroutines + everything else.
	std::vector<std::shared_ptr<awst::RootNode>> filtered;
	filtered.reserve(_roots.size());
	for (auto& root: _roots)
	{
		if (dynamic_cast<awst::Contract const*>(root.get()))
			filtered.push_back(std::move(root));
		else if (auto const* sub = dynamic_cast<awst::Subroutine const*>(root.get()))
		{
			if (reachable.count(sub->id))
				filtered.push_back(std::move(root));
		}
		else
			filtered.push_back(std::move(root));
	}
	return filtered;
}

} // namespace puyasol::builder
