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

void ExpressionBuilder::trackConstantLocal(int64_t _declId, unsigned long long _value)
{
	m_ctx.constantLocals[_declId] = _value;
}

unsigned long long ExpressionBuilder::getConstantLocal(solidity::frontend::Declaration const* _decl) const
{
	if (!_decl)
		return 0;
	auto it = m_ctx.constantLocals.find(_decl->id());
	return it != m_ctx.constantLocals.end() ? it->second : 0;
}

void ExpressionBuilder::trackFuncPtrTarget(int64_t _declId, solidity::frontend::FunctionDefinition const* _func)
{
	m_ctx.funcPtrTargets[_declId] = _func;
}

solidity::frontend::FunctionDefinition const* ExpressionBuilder::getFuncPtrTarget(int64_t _declId) const
{
	auto it = m_ctx.funcPtrTargets.find(_declId);
	return it != m_ctx.funcPtrTargets.end() ? it->second : nullptr;
}

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
	Logger::instance().debug("[TRACE] ExpressionBuilder m_freeFunctionById.size()=" + std::to_string(m_freeFunctionById.size()) + " paramSize=" + std::to_string(_freeFunctionById.size()) + " addr=" + std::to_string((uintptr_t)&m_freeFunctionById));

	// Wire the BuilderContext callbacks back into ExpressionBuilder.
	m_ctx.buildExpr = [this](solidity::frontend::Expression const& _expr) {
		return this->build(_expr);
	};
	m_ctx.buildBinaryOp = [this](solidity::frontend::Token _op,
		std::shared_ptr<awst::Expression> _left,
		std::shared_ptr<awst::Expression> _right,
		awst::WType const* _resultType,
		awst::SourceLocation const& _loc) {
		return eb::buildBinaryOp(m_ctx, _op, std::move(_left), std::move(_right), _resultType, _loc);
	};
	m_ctx.builderForInstance = [this](solidity::frontend::Type const* _solType, std::shared_ptr<awst::Expression> _expr) {
		return m_registry.tryBuildInstance(m_ctx, _solType, std::move(_expr));
	};
}

std::string ExpressionBuilder::resolveMethodName(
	solidity::frontend::FunctionDefinition const& _func
)
{
	std::string name = _func.name();
	if (m_overloadedNames.count(name))
	{
		name += "(";
		bool first = true;
		for (auto const& p: _func.parameters())
		{
			if (!first) name += ",";
			auto const* solType = p->type();
			if (dynamic_cast<solidity::frontend::BoolType const*>(solType))
				name += "b";
			else if (auto const* intType = dynamic_cast<solidity::frontend::IntegerType const*>(solType))
				name += (intType->isSigned() ? "i" : "u") + std::to_string(intType->numBits());
			else if (dynamic_cast<solidity::frontend::AddressType const*>(solType))
				name += "addr";
			else if (auto const* fixedBytes = dynamic_cast<solidity::frontend::FixedBytesType const*>(solType))
				name += "b" + std::to_string(fixedBytes->numBytes());
			else
				name += std::to_string(p->id());
			first = false;
		}
		name += ")";
	}
	return name;
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

void ExpressionBuilder::addParamRemap(int64_t _declId, std::string const& _uniqueName, awst::WType const* _type)
{
	m_ctx.paramRemaps[_declId] = {_uniqueName, _type};
}

void ExpressionBuilder::removeParamRemap(int64_t _declId)
{
	m_ctx.paramRemaps.erase(_declId);
}

void ExpressionBuilder::addSuperTarget(int64_t _funcId, std::string const& _name)
{
	m_ctx.superTargetNames[_funcId] = _name;
}

void ExpressionBuilder::clearSuperTargets()
{
	m_ctx.superTargetNames.clear();
}

void ExpressionBuilder::addStorageAlias(int64_t _declId, std::shared_ptr<awst::Expression> _expr)
{
	m_ctx.storageAliases[_declId] = std::move(_expr);
}

void ExpressionBuilder::removeStorageAlias(int64_t _declId)
{
	m_ctx.storageAliases.erase(_declId);
}

void ExpressionBuilder::addMappingKeyParam(int64_t _declId, std::string const& _paramName)
{
	m_ctx.mappingKeyParams[_declId] = _paramName;
}

std::string ExpressionBuilder::getMappingKeyParam(int64_t _declId) const
{
	auto it = m_ctx.mappingKeyParams.find(_declId);
	return it != m_ctx.mappingKeyParams.end() ? it->second : std::string{};
}

awst::SourceLocation ExpressionBuilder::makeLoc(
	solidity::langutil::SourceLocation const& _solLoc
)
{
	awst::SourceLocation loc;
	loc.file = m_ctx.sourceFile;
	loc.line = _solLoc.start >= 0 ? _solLoc.start : 0;
	loc.endLine = _solLoc.end >= 0 ? _solLoc.end : 0;
	return loc;
}

} // namespace puyasol::builder
