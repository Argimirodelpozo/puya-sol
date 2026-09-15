#include "builder/eb/EffectScan.h"
#include "builder/context/ProgramAnalysis.h"
#include "builder/solc/SolcFacts.h"
#include "builder/context/ContractContext.h"
#include "builder/types/TypeMapper.h"

#include <libsolidity/ast/ASTVisitor.h>

namespace puyasol::builder
{

bool EffectScan::requiresSequencing(solidity::frontend::Expression const& expression,
	eb::ContractContext& context)
{
	using namespace solidity::frontend;
	struct Scan: ASTConstVisitor
	{
		eb::ContractContext& context;
		bool found = false;
		explicit Scan(eb::ContractContext& ctx): context(ctx) {}
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
				auto const* function = SolcFacts::resolveInternalCall(call, context.currentContract);
				if (function)
				{
					auto const& effects = analysis.parameterMutations(context.currentContract, *function);
					found |= !effects.mutatedParameterIndices.empty()
						|| !effects.assemblyEffects.canBeRemoved;
				}
				else if (type->kind() == FunctionType::Kind::Internal)
					found = true;
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
	} scan(context);
	expression.accept(scan);
	return scan.found;
}

} // namespace puyasol::builder
