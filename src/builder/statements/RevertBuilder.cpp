/// @file RevertBuilder.cpp
/// Handles revert statements.

#include "builder/statements/StatementBuilder.h"
#include "builder/assembly/AssemblyBuilder.h"
#include "builder/storage/StorageMapper.h"
#include "Logger.h"

namespace puyasol::builder
{

bool StatementBuilder::visit(solidity::frontend::RevertStatement const& _node)
{
	auto loc = makeLoc(_node.location());

	// Extract custom error name from the errorCall expression
	std::string errorName = "revert";
	auto const& errorCall = _node.errorCall();
	if (auto const* ident = dynamic_cast<solidity::frontend::Identifier const*>(&errorCall.expression()))
		errorName = ident->name();
	else if (auto const* ma = dynamic_cast<solidity::frontend::MemberAccess const*>(&errorCall.expression()))
		errorName = ma->memberName();

	Logger::instance().debug("revert mapped to assert(false, \"" + errorName + "\")", loc);
	auto assertExpr = std::make_shared<awst::AssertExpression>();
	assertExpr->sourceLocation = loc;
	assertExpr->wtype = awst::WType::voidType();

	auto falseLit = std::make_shared<awst::BoolConstant>();
	falseLit->sourceLocation = loc;
	falseLit->wtype = awst::WType::boolType();
	falseLit->value = false;

	assertExpr->condition = falseLit;
	assertExpr->errorMessage = errorName;

	auto stmt = std::make_shared<awst::ExpressionStatement>();
	stmt->sourceLocation = loc;
	stmt->expr = assertExpr;
	push(stmt);
	return false;
}


} // namespace puyasol::builder
