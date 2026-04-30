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



// ── Helpers (merged from HelpersBuilder.cpp) ──

std::shared_ptr<awst::Expression> ExpressionBuilder::buildTupleWithUpdatedField(
	std::shared_ptr<awst::Expression> _base,
	std::string const& _fieldName,
	std::shared_ptr<awst::Expression> _newValue,
	awst::SourceLocation const& _loc
)
{
	auto const* tupleType = dynamic_cast<awst::WTuple const*>(_base->wtype);
	auto const& names = *tupleType->names();
	auto const& types = tupleType->types();

	auto tuple = std::make_shared<awst::TupleExpression>();
	tuple->sourceLocation = _loc;
	tuple->wtype = _base->wtype;

	for (size_t i = 0; i < names.size(); ++i)
	{
		if (names[i] == _fieldName)
		{
			tuple->items.push_back(std::move(_newValue));
		}
		else
		{
			auto field = std::make_shared<awst::FieldExpression>();
			field->sourceLocation = _loc;
			field->base = _base;
			field->name = names[i];
			field->wtype = types[i];
			tuple->items.push_back(std::move(field));
		}
	}

	return tuple;
}

std::optional<ExpressionBuilder::StateVarInfo> ExpressionBuilder::resolveStateVar(
	std::string const& _name
)
{
	(void)_name;
	return std::nullopt;
}

std::shared_ptr<awst::IntegerConstant> ExpressionBuilder::makeUint64(
	std::string _value, awst::SourceLocation const& _loc
)
{
	auto e = awst::makeIntegerConstant(std::move(_value), _loc);
	return e;
}

std::shared_ptr<awst::Expression> ExpressionBuilder::buildCreateInnerTransaction(
	int _txnType,
	std::map<std::string, std::shared_ptr<awst::Expression>> _fields,
	awst::SourceLocation const& _loc
)
{
	// Set Fee to 0 (auto-pool from outer transaction)
	_fields["Fee"] = makeUint64("0", _loc);

	// Set TypeEnum to the transaction type
	_fields["TypeEnum"] = makeUint64(std::to_string(_txnType), _loc);

	auto* wtype = m_ctx.typeMapper.createType<awst::WInnerTransactionFields>(_txnType);

	auto create = std::make_shared<awst::CreateInnerTransaction>();
	create->sourceLocation = _loc;
	create->wtype = wtype;
	create->fields = std::move(_fields);
	return create;
}

std::shared_ptr<awst::Expression> ExpressionBuilder::buildSubmitAndReturn(
	std::shared_ptr<awst::Expression> _createExpr,
	awst::WType const* _solidityReturnType,
	awst::SourceLocation const& _loc
)
{
	// Extract transaction type from the create expression's wtype
	std::optional<int> txnType;
	if (auto const* itf = dynamic_cast<awst::WInnerTransactionFields const*>(_createExpr->wtype))
		txnType = itf->transactionType();

	auto* submitWtype = m_ctx.typeMapper.createType<awst::WInnerTransaction>(txnType);

	auto submit = std::make_shared<awst::SubmitInnerTransaction>();
	submit->sourceLocation = _loc;
	submit->wtype = submitWtype;
	submit->itxns.push_back(std::move(_createExpr));

	// For void returns: return submit directly
	if (!_solidityReturnType || _solidityReturnType == awst::WType::voidType())
		return submit;

	// For bool returns on non-app-call txns (transfer, send): return true
	if (_solidityReturnType == awst::WType::boolType()
		&& (!txnType.has_value() || txnType.value() != 6))
	{
		auto stmt = awst::makeExpressionStatement(std::move(submit), _loc);
		m_ctx.prePendingStatements.push_back(std::move(stmt));

		return awst::makeBoolConstant(true, _loc);
	}

	// For application calls with return values: extract from LastLog
	if (txnType.has_value() && txnType.value() == 6) // Application call
	{
		// Submit as a pre-pending statement so it executes BEFORE reading the result
		auto submitStmt = awst::makeExpressionStatement(std::move(submit), _loc);
		m_ctx.prePendingStatements.push_back(std::move(submitStmt));

		// Read LastLog from most recently submitted inner txn using itxn intrinsic
		auto readLog = awst::makeIntrinsicCall("itxn", awst::WType::bytesType(), _loc);
		readLog->immediates = {std::string("LastLog")};

		// Strip the 4-byte ARC4 return prefix (0x151f7c75)
		auto stripPrefix = std::make_shared<awst::IntrinsicCall>();
		stripPrefix->sourceLocation = _loc;
		stripPrefix->opCode = "extract";
		stripPrefix->immediates = {4, 0}; // offset 4, length 0 = rest of bytes
		stripPrefix->wtype = awst::WType::bytesType();
		stripPrefix->stackArgs.push_back(std::move(readLog));

		// Convert raw ABI bytes to the target Solidity type
		if (_solidityReturnType == awst::WType::biguintType())
		{
			auto cast = awst::makeReinterpretCast(std::move(stripPrefix), awst::WType::biguintType(), _loc);
			return cast;
		}
		else if (_solidityReturnType == awst::WType::uint64Type())
		{
			auto btoi = awst::makeIntrinsicCall("btoi", awst::WType::uint64Type(), _loc);
			btoi->stackArgs.push_back(std::move(stripPrefix));
			return btoi;
		}
		else if (_solidityReturnType == awst::WType::boolType())
		{
			// ARC4 bool is 1 byte: 0x80 = true, 0x00 = false.
			// Extract bit 0 (MSB) using getbit to get 0 or 1, then compare != 0 for bool.
			auto getbit = awst::makeIntrinsicCall("getbit", awst::WType::uint64Type(), _loc);
			getbit->stackArgs.push_back(std::move(stripPrefix));
			getbit->stackArgs.push_back(makeUint64("0", _loc));

			auto cmp = awst::makeNumericCompare(std::move(getbit), awst::NumericComparison::Ne, makeUint64("0", _loc), _loc);
			return cmp;
		}
		else if (_solidityReturnType == awst::WType::accountType())
		{
			auto cast = awst::makeReinterpretCast(std::move(stripPrefix), awst::WType::accountType(), _loc);
			return cast;
		}
		// For tuple/struct returns: decode each field from ARC4-encoded bytes
		if (auto const* tupleType = dynamic_cast<awst::WTuple const*>(_solidityReturnType))
		{
			// Wrap stripPrefix in SingleEvaluation so it's only read once
			auto singleBytes = std::make_shared<awst::SingleEvaluation>();
			singleBytes->sourceLocation = _loc;
			singleBytes->wtype = awst::WType::bytesType();
			singleBytes->source = std::move(stripPrefix);
			singleBytes->id = 0; // unique per method

			auto tuple = std::make_shared<awst::TupleExpression>();
			tuple->sourceLocation = _loc;
			tuple->wtype = _solidityReturnType;

			int offset = 0;
			for (size_t i = 0; i < tupleType->types().size(); ++i)
			{
				auto const* fieldType = tupleType->types()[i];
				int fieldSize = 0;

				if (fieldType == awst::WType::biguintType())
					fieldSize = 32; // ARC4 uint256
				else if (fieldType == awst::WType::uint64Type())
					fieldSize = 8;
				else if (fieldType == awst::WType::boolType())
					fieldSize = 1; // ARC4 bool
				else if (fieldType == awst::WType::accountType())
					fieldSize = 32;
				else if (auto const* bwt = dynamic_cast<awst::BytesWType const*>(fieldType))
				{
					if (bwt->length().has_value())
						fieldSize = static_cast<int>(bwt->length().value());
					else
						fieldSize = 0; // dynamic — not handled
				}

				if (fieldSize == 0)
				{
					// Can't decode dynamic fields, return bytes for the rest
					tuple->items.push_back(singleBytes);
					break;
				}

				// extract3(bytes, offset, fieldSize)
				auto extract = awst::makeIntrinsicCall("extract3", awst::WType::bytesType(), _loc);
				extract->stackArgs.push_back(singleBytes);
				extract->stackArgs.push_back(makeUint64(std::to_string(offset), _loc));
				extract->stackArgs.push_back(makeUint64(std::to_string(fieldSize), _loc));

				// Cast extracted bytes to the correct type
				std::shared_ptr<awst::Expression> fieldExpr;
				if (fieldType == awst::WType::biguintType())
				{
					auto cast = awst::makeReinterpretCast(std::move(extract), awst::WType::biguintType(), _loc);
					fieldExpr = std::move(cast);
				}
				else if (fieldType == awst::WType::uint64Type())
				{
					auto btoi = awst::makeIntrinsicCall("btoi", awst::WType::uint64Type(), _loc);
					btoi->stackArgs.push_back(std::move(extract));
					fieldExpr = std::move(btoi);
				}
				else if (fieldType == awst::WType::boolType())
				{
					auto getbit = awst::makeIntrinsicCall("getbit", awst::WType::uint64Type(), _loc);
					getbit->stackArgs.push_back(std::move(extract));
					getbit->stackArgs.push_back(makeUint64("0", _loc));

					auto cmp = awst::makeNumericCompare(std::move(getbit), awst::NumericComparison::Ne, makeUint64("0", _loc), _loc);
					fieldExpr = std::move(cmp);
				}
				else if (fieldType == awst::WType::accountType())
				{
					auto cast = awst::makeReinterpretCast(std::move(extract), awst::WType::accountType(), _loc);
					fieldExpr = std::move(cast);
				}
				else
				{
					// bytes or fixed bytes — cast to target type
					auto cast = awst::makeReinterpretCast(std::move(extract), fieldType, _loc);
					fieldExpr = std::move(cast);
				}

				tuple->items.push_back(std::move(fieldExpr));
				offset += fieldSize;
			}

			return tuple;
		}

		// For ARC4Struct returns: wrap bytes in ReinterpretCast to struct type
		if (dynamic_cast<awst::ARC4Struct const*>(_solidityReturnType))
		{
			auto cast = awst::makeReinterpretCast(std::move(stripPrefix), _solidityReturnType, _loc);
			return cast;
		}

		// For bytes/string: return stripped bytes directly
		return stripPrefix;
	}

	// For non-appl returns: emit submit as pending, return type-appropriate default
	auto stmt = awst::makeExpressionStatement(std::move(submit), _loc);
	m_ctx.pendingStatements.push_back(std::move(stmt));

	return StorageMapper::makeDefaultValue(_solidityReturnType, _loc);
}

} // namespace puyasol::builder
