#include "builder/sol-ast/EffectScan.h"
#include "builder/ProgramAnalysis.h"
#include "builder/sol-ast/Context.h"
#include "builder/sol-eb/ContractContext.h"

#include <libsolidity/ast/ASTVisitor.h>

namespace puyasol::builder
{

bool EffectScan::mayWrite(solidity::frontend::Expression const& expression,
	eb::ContractContext& context, sol_ast::Context const& scope)
{
	using namespace solidity::frontend;
	int64_t caller = 0;
	for (auto const* current = &scope; current; current = current->parent())
		if (auto const* function = dynamic_cast<sol_ast::FunctionContext const*>(current))
		{
			caller = function->callableId;
			break;
		}
	struct Scan: ASTConstVisitor
	{
		eb::ContractContext& context;
		int64_t caller;
		bool found = false;
		Scan(eb::ContractContext& ctx, int64_t id): context(ctx), caller(id) {}
		bool visit(FunctionCall const& call) override
		{
			auto kind = *call.annotation().kind;
			if (kind == FunctionCallKind::TypeConversion || kind == FunctionCallKind::StructConstructorCall)
				return !found;
			auto const* type = dynamic_cast<FunctionType const*>(call.expression().annotation().type);
			if (!type || (type->stateMutability() != StateMutability::Pure
				&& type->stateMutability() != StateMutability::View))
				found = true;
			else
			{
				auto const& analysis = context.typeMapper.analysis();
				auto const* effects = analysis.parameterMutationsForCall(context.currentContract, caller, call);
				if (effects)
					found |= !effects->mutatedParameterIndices.empty();
				else if (type->kind() == FunctionType::Kind::Internal)
					for (auto const* parameter: type->parameterTypes())
						found |= parameter->dataStoredIn(DataLocation::Memory);
				// Assembly can write shared EVM memory without a reference parameter.
				if (auto const* function = dynamic_cast<FunctionDefinition const*>(
						ASTNode::referencedDeclaration(call.expression())))
					found |= analysis.callablesWithInlineAssembly.contains(function->id());
			}
			return !found;
		}
		bool visit(Assignment const&) override { found = true; return false; }
		bool visit(NewExpression const&) override { found = true; return false; }
		bool visit(UnaryOperation const& unary) override
		{
			auto op = unary.getOperator();
			found |= op == Token::Inc || op == Token::Dec || op == Token::Delete;
			return !found;
		}
	} scan(context, caller);
	expression.accept(scan);
	return scan.found;
}

} // namespace puyasol::builder
