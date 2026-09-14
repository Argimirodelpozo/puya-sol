#pragma once

/// @file SubroutineRegistry.hpp
/// Whole-unit integrity checks for AWST roots. Producers emit
/// helpers on demand; this validates identity and reference invariants without
/// performing post-build dead-code elimination. Header-only (one consumer).

#include "awst/Node.h"
#include "awst/Visit.h"
#include "Logger.h"

#include <map>
#include <memory>
#include <set>
#include <vector>

namespace puyasol::builder
{

namespace subroutine_registry_detail
{
inline void collectReferences(
	awst::Statement const* _body,
	std::map<std::string, awst::SourceLocation>& _references)
{
	if (!_body)
		return;
	awst::visitExpressions(*_body, [&](awst::Expression const& expression) {
		auto const* call = dynamic_cast<awst::SubroutineCallExpression const*>(&expression);
		if (!call)
			return;
		if (auto const* id = std::get_if<awst::SubroutineID>(&call->target))
			_references.try_emplace(id->target, call->sourceLocation);
	});
}
} // namespace subroutine_registry_detail

/// Reject duplicate root identities and unresolved root subroutine calls.
inline bool validateAwstRoots(
	std::vector<std::shared_ptr<awst::RootNode>> const& _roots)
{
	using subroutine_registry_detail::collectReferences;
	std::map<std::string, awst::Subroutine const*> definitions;
	std::map<std::string, awst::SourceLocation> references;
	std::set<std::string> rootIds;
	bool valid = true;
	auto define = [&](auto const& node) {
		if (node.id.empty() || !rootIds.insert(node.id).second)
		{
			Logger::instance().error("empty or duplicate AWST root id '" + node.id + "'",
				node.sourceLocation);
			valid = false;
		}
	};

	for (auto const& root: _roots)
	{
		if (auto const* subroutine = dynamic_cast<awst::Subroutine const*>(root.get()))
		{
			define(*subroutine);
			definitions.emplace(subroutine->id, subroutine);
			collectReferences(subroutine->body.get(), references);
			continue;
		}
		if (auto const* contract = dynamic_cast<awst::Contract const*>(root.get()))
		{
			define(*contract);
			collectReferences(contract->approvalProgram.body.get(), references);
			collectReferences(contract->clearProgram.body.get(), references);
			for (auto const& method: contract->methods)
				collectReferences(method.body.get(), references);
			continue;
		}
		if (auto const* logicSig = dynamic_cast<awst::LogicSignature const*>(root.get()))
		{
			define(*logicSig);
			if (logicSig->program)
				collectReferences(logicSig->program->body.get(), references);
		}
	}

	for (auto const& [id, location]: references)
		if (!definitions.count(id))
		{
			Logger::instance().error(
				"unresolved root subroutine id '" + id + "'", location);
			valid = false;
		}

	return valid;
}

} // namespace puyasol::builder
