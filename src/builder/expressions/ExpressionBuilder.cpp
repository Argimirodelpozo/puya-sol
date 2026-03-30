#include "builder/expressions/ExpressionBuilder.h"
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
	m_constantLocals[_declId] = _value;
}

unsigned long long ExpressionBuilder::getConstantLocal(solidity::frontend::Declaration const* _decl) const
{
	if (!_decl)
		return 0;
	auto it = m_constantLocals.find(_decl->id());
	return it != m_constantLocals.end() ? it->second : 0;
}

void ExpressionBuilder::trackFuncPtrTarget(int64_t _declId, solidity::frontend::FunctionDefinition const* _func)
{
	m_funcPtrTargets[_declId] = _func;
}

solidity::frontend::FunctionDefinition const* ExpressionBuilder::getFuncPtrTarget(int64_t _declId) const
{
	auto it = m_funcPtrTargets.find(_declId);
	return it != m_funcPtrTargets.end() ? it->second : nullptr;
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
	: m_typeMapper(_typeMapper),
	  m_storageMapper(_storageMapper),
	  m_sourceFile(_sourceFile),
	  m_contractName(_contractName),
	  m_libraryFunctionIds(_libraryFunctionIds),
	  m_overloadedNames(_overloadedNames.empty() ? s_emptyOverloads : _overloadedNames),
	  m_freeFunctionById(_freeFunctionById.empty() ? s_emptyFreeFunctionIds : _freeFunctionById)
{
	Logger::instance().debug("[TRACE] ExpressionBuilder m_freeFunctionById.size()=" + std::to_string(m_freeFunctionById.size()) + " paramSize=" + std::to_string(_freeFunctionById.size()) + " addr=" + std::to_string((uintptr_t)&m_freeFunctionById));
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
	m_stack.clear();
	_expr.accept(*this);
	if (m_stack.empty())
	{
		// Return a void constant as fallback
		auto vc = std::make_shared<awst::VoidConstant>();
		vc->sourceLocation = makeLoc(_expr.location());
		vc->wtype = awst::WType::voidType();
		return vc;
	}
	return pop();
}

void ExpressionBuilder::push(std::shared_ptr<awst::Expression> _expr)
{
	m_stack.push_back(std::move(_expr));
}

std::shared_ptr<awst::Expression> ExpressionBuilder::pop()
{
	if (m_stack.empty())
	{
		auto vc = std::make_shared<awst::VoidConstant>();
		vc->wtype = awst::WType::voidType();
		return vc;
	}
	auto expr = m_stack.back();
	m_stack.pop_back();
	return expr;
}

std::vector<std::shared_ptr<awst::Statement>> ExpressionBuilder::takePendingStatements()
{
	std::vector<std::shared_ptr<awst::Statement>> result;
	result.swap(m_pendingStatements);
	return result;
}

std::vector<std::shared_ptr<awst::Statement>> ExpressionBuilder::takePrePendingStatements()
{
	std::vector<std::shared_ptr<awst::Statement>> result;
	result.swap(m_prePendingStatements);
	return result;
}

void ExpressionBuilder::addParamRemap(int64_t _declId, std::string const& _uniqueName, awst::WType const* _type)
{
	m_paramRemaps[_declId] = {_uniqueName, _type};
}

void ExpressionBuilder::removeParamRemap(int64_t _declId)
{
	m_paramRemaps.erase(_declId);
}

void ExpressionBuilder::addSuperTarget(int64_t _funcId, std::string const& _name)
{
	m_superTargetNames[_funcId] = _name;
}

void ExpressionBuilder::addStorageAlias(int64_t _declId, std::shared_ptr<awst::Expression> _expr)
{
	m_storageAliases[_declId] = std::move(_expr);
}

void ExpressionBuilder::removeStorageAlias(int64_t _declId)
{
	m_storageAliases.erase(_declId);
}

awst::SourceLocation ExpressionBuilder::makeLoc(
	solidity::langutil::SourceLocation const& _solLoc
)
{
	awst::SourceLocation loc;
	loc.file = m_sourceFile;
	loc.line = _solLoc.start >= 0 ? _solLoc.start : 0;
	loc.endLine = _solLoc.end >= 0 ? _solLoc.end : 0;
	return loc;
}

bool ExpressionBuilder::isBigUInt(awst::WType const* _type)
{
	return _type == awst::WType::biguintType();
}


eb::BuilderContext ExpressionBuilder::makeBuilderContext()
{
	// Minimal context for builtin callable dispatch.
	// Only typeMapper, storageMapper, pendingStatements, prePendingStatements
	// are used by the builtin handlers.
	static std::map<int64_t, eb::ParamRemap> dummyParamRemaps;
	static std::unordered_map<int64_t, std::string> dummySuperTargets;
	static std::map<int64_t, std::shared_ptr<awst::Expression>> dummyStorageAliases;
	static std::map<int64_t, solidity::frontend::FunctionDefinition const*> dummyFuncPtrTargets;
	static std::unordered_map<int64_t, unsigned long long> dummyConstantLocals;

	return eb::BuilderContext{
		/*.typeMapper =*/ m_typeMapper,
		/*.storageMapper =*/ m_storageMapper,
		/*.sourceFile =*/ m_sourceFile,
		/*.contractName =*/ m_contractName,
		/*.libraryFunctionIds =*/ m_libraryFunctionIds,
		/*.overloadedNames =*/ m_overloadedNames,
		/*.freeFunctionById =*/ m_freeFunctionById,
		/*.pendingStatements =*/ m_pendingStatements,
		/*.prePendingStatements =*/ m_prePendingStatements,
		/*.paramRemaps =*/ dummyParamRemaps,
		/*.superTargetNames =*/ dummySuperTargets,
		/*.storageAliases =*/ dummyStorageAliases,
		/*.funcPtrTargets =*/ dummyFuncPtrTargets,
		/*.constantLocals =*/ dummyConstantLocals,
		/*.inConstructor =*/ m_inConstructor,
		/*.inUncheckedBlock =*/ m_inUncheckedBlock,
		/*.buildExpr =*/ [this](solidity::frontend::Expression const& _expr) {
			return this->build(_expr);
		},
		/*.builderForInstance =*/ [this](solidity::frontend::Type const* _solType, std::shared_ptr<awst::Expression> _expr) {
			auto ctx = makeBuilderContext();
			return m_registry.tryBuildInstance(ctx, _solType, std::move(_expr));
		},
	};
}

} // namespace puyasol::builder
