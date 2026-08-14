#include "builder/SubroutineRegistry.h"

#include "awst/Visit.h"
#include "Logger.h"

#include <map>

namespace puyasol::builder
{
namespace
{

void collectReferences(
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

} // namespace

bool validateRootSubroutines(
	std::vector<std::shared_ptr<awst::RootNode>> const& _roots)
{
	std::map<std::string, awst::Subroutine const*> definitions;
	std::map<std::string, awst::SourceLocation> references;
	bool valid = true;

	for (auto const& root: _roots)
	{
		if (auto const* subroutine = dynamic_cast<awst::Subroutine const*>(root.get()))
		{
			auto const [_, inserted] = definitions.emplace(subroutine->id, subroutine);
			if (!inserted)
			{
				Logger::instance().error(
					"duplicate root subroutine id '" + subroutine->id + "'",
					subroutine->sourceLocation);
				valid = false;
			}
			collectReferences(subroutine->body.get(), references);
			continue;
		}
		if (auto const* contract = dynamic_cast<awst::Contract const*>(root.get()))
		{
			collectReferences(contract->approvalProgram.body.get(), references);
			collectReferences(contract->clearProgram.body.get(), references);
			for (auto const& method: contract->methods)
				collectReferences(method.body.get(), references);
			continue;
		}
		if (auto const* logicSig = dynamic_cast<awst::LogicSignature const*>(root.get()))
			if (logicSig->program)
				collectReferences(logicSig->program->body.get(), references);
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
