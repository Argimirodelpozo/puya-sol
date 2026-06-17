#include "builder/SubroutineReachability.h"
#include "splitter/AwstWalker.h"

#include <queue>
#include <set>
#include <unordered_map>

namespace puyasol::builder
{
namespace
{

/// Collect SubroutineIDs reachable from a Block via AwstWalker.
/// const_cast is safe: callback always returns nullptr (read-only).
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

	// Seed with contract method and logic-sig program references.
	for (auto const& root: _roots)
	{
		std::set<std::string> refs;
		if (auto const* contract = dynamic_cast<awst::Contract const*>(root.get()))
		{
			collectMethodRefs(contract->approvalProgram, refs);
			collectMethodRefs(contract->clearProgram, refs);
			for (auto const& method: contract->methods)
				collectMethodRefs(method, refs);
		}
		else if (auto const* lsig = dynamic_cast<awst::LogicSignature const*>(root.get()))
		{
			// Lsig program is a reachability root; seed subroutines its body calls.
			if (lsig->program && lsig->program->body)
				collectRefs(*lsig->program->body, refs);
		}
		else
			continue;
		for (auto const& id: refs)
		{
			reachable.insert(id);
			worklist.push(id);
		}
	}

	std::unordered_map<std::string, awst::Subroutine const*> subMap;
	for (auto const& root: _roots)
		if (auto const* sub = dynamic_cast<awst::Subroutine const*>(root.get()))
			subMap[sub->id] = sub;

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

	// Keep contracts always, reachable subroutines, and all other roots.
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
