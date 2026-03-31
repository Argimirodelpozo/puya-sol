#include "builder/sol-ast/SolStatement.h"

namespace puyasol::builder::sol_ast
{

awst::SourceLocation StatementContext::makeLoc(
	solidity::langutil::SourceLocation const& _solLoc) const
{
	awst::SourceLocation loc;
	loc.file = sourceFile;
	loc.line = _solLoc.start >= 0 ? _solLoc.start : 0;
	loc.endLine = _solLoc.end >= 0 ? _solLoc.end : 0;
	return loc;
}

} // namespace puyasol::builder::sol_ast
