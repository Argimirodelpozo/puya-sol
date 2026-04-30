#include "builder/ExpressionBuilder.h"
#include "builder/sol-ast/SolExpressionDispatch.h"
#include "builder/sol-eb/BinaryOpBuilder.h"
#include "builder/storage/StorageMapper.h"
#include "builder/sol-types/TypeCoercion.h"
#include "Logger.h"

#include <libsolidity/ast/TypeProvider.h>

#include <algorithm>
#include <sstream>

namespace puyasol::builder
{

LibraryFunctionIdMap const ExpressionBuilder::s_emptyLibraryFunctionIds{};

std::shared_ptr<awst::Expression> ExpressionBuilder::implicitNumericCast(
	std::shared_ptr<awst::Expression> _expr,
	awst::WType const* _targetType,
	awst::SourceLocation const& _loc
)
{
	return TypeCoercion::implicitNumericCast(std::move(_expr), _targetType, _loc);
}

static OverloadedNamesSet const s_emptyOverloads;

FreeFunctionIdMap const ExpressionBuilder::s_emptyFreeFunctionIds;

ExpressionBuilder::ExpressionBuilder(
	TypeMapper& _typeMapper,
	StorageMapper& _storageMapper,
	std::string const& _sourceFile,
	std::string const& _contractName,
	LibraryFunctionIdMap const& _libraryFunctionIds,
	OverloadedNamesSet const& _overloadedNames,
	FreeFunctionIdMap const& _freeFunctionById
)
	: m_ctx(
		_typeMapper,
		_storageMapper,
		_sourceFile,
		_contractName,
		_libraryFunctionIds,
		_overloadedNames.empty() ? s_emptyOverloads : _overloadedNames,
		_freeFunctionById.empty() ? s_emptyFreeFunctionIds : _freeFunctionById
	),
	  m_libraryFunctionIds(_libraryFunctionIds),
	  m_overloadedNames(_overloadedNames.empty() ? s_emptyOverloads : _overloadedNames),
	  m_freeFunctionById(_freeFunctionById.empty() ? s_emptyFreeFunctionIds : _freeFunctionById)
{
	// BuilderContext self-wires its callbacks (buildExpr, buildBinaryOp,
	// builderForInstance) and constructs its own type-builder registry in its
	// constructor — nothing more for ExpressionBuilder to do.
}

std::shared_ptr<awst::Expression> ExpressionBuilder::build(
	solidity::frontend::Expression const& _expr
)
{
	return sol_ast::buildExpression(m_ctx, _expr);
}

std::vector<std::shared_ptr<awst::Statement>> ExpressionBuilder::takePendingStatements()
{
	std::vector<std::shared_ptr<awst::Statement>> result;
	result.swap(m_ctx.pendingStatements);
	return result;
}

std::vector<std::shared_ptr<awst::Statement>> ExpressionBuilder::takePrePendingStatements()
{
	std::vector<std::shared_ptr<awst::Statement>> result;
	result.swap(m_ctx.prePendingStatements);
	return result;
}

} // namespace puyasol::builder
