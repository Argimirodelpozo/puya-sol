/// @file HelpersBuilder.cpp
/// Helper methods for inner transactions, tuple updates, state var resolution.

#include "builder/expressions/ExpressionBuilder.h"
#include "builder/storage/StorageMapper.h"
#include "Logger.h"

#include <libsolidity/ast/TypeProvider.h>

#include <algorithm>
#include <sstream>

namespace puyasol::builder
{

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
	auto e = std::make_shared<awst::IntegerConstant>();
	e->sourceLocation = _loc;
	e->wtype = awst::WType::uint64Type();
	e->value = std::move(_value);
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

	auto* wtype = m_typeMapper.createType<awst::WInnerTransactionFields>(_txnType);

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

	auto* submitWtype = m_typeMapper.createType<awst::WInnerTransaction>(txnType);

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
		auto stmt = std::make_shared<awst::ExpressionStatement>();
		stmt->sourceLocation = _loc;
		stmt->expr = std::move(submit);
		m_prePendingStatements.push_back(std::move(stmt));

		auto result = std::make_shared<awst::BoolConstant>();
		result->sourceLocation = _loc;
		result->wtype = awst::WType::boolType();
		result->value = true;
		return result;
	}

	// For application calls with return values: extract from LastLog
	if (txnType.has_value() && txnType.value() == 6) // Application call
	{
		// Submit as a pre-pending statement so it executes BEFORE reading the result
		auto submitStmt = std::make_shared<awst::ExpressionStatement>();
		submitStmt->sourceLocation = _loc;
		submitStmt->expr = std::move(submit);
		m_prePendingStatements.push_back(std::move(submitStmt));

		// Read LastLog from most recently submitted inner txn using itxn intrinsic
		auto readLog = std::make_shared<awst::IntrinsicCall>();
		readLog->sourceLocation = _loc;
		readLog->wtype = awst::WType::bytesType();
		readLog->opCode = "itxn";
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
			auto cast = std::make_shared<awst::ReinterpretCast>();
			cast->sourceLocation = _loc;
			cast->wtype = awst::WType::biguintType();
			cast->expr = std::move(stripPrefix);
			return cast;
		}
		else if (_solidityReturnType == awst::WType::uint64Type())
		{
			auto btoi = std::make_shared<awst::IntrinsicCall>();
			btoi->sourceLocation = _loc;
			btoi->opCode = "btoi";
			btoi->wtype = awst::WType::uint64Type();
			btoi->stackArgs.push_back(std::move(stripPrefix));
			return btoi;
		}
		else if (_solidityReturnType == awst::WType::boolType())
		{
			// ARC4 bool is 1 byte: 0x80 = true, 0x00 = false.
			// Extract bit 0 (MSB) using getbit to get 0 or 1, then compare != 0 for bool.
			auto getbit = std::make_shared<awst::IntrinsicCall>();
			getbit->sourceLocation = _loc;
			getbit->opCode = "getbit";
			getbit->wtype = awst::WType::uint64Type();
			getbit->stackArgs.push_back(std::move(stripPrefix));
			getbit->stackArgs.push_back(makeUint64("0", _loc));

			auto cmp = std::make_shared<awst::NumericComparisonExpression>();
			cmp->sourceLocation = _loc;
			cmp->wtype = awst::WType::boolType();
			cmp->lhs = std::move(getbit);
			cmp->rhs = makeUint64("0", _loc);
			cmp->op = awst::NumericComparison::Ne;
			return cmp;
		}
		else if (_solidityReturnType == awst::WType::accountType())
		{
			auto cast = std::make_shared<awst::ReinterpretCast>();
			cast->sourceLocation = _loc;
			cast->wtype = awst::WType::accountType();
			cast->expr = std::move(stripPrefix);
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
				auto extract = std::make_shared<awst::IntrinsicCall>();
				extract->sourceLocation = _loc;
				extract->opCode = "extract3";
				extract->wtype = awst::WType::bytesType();
				extract->stackArgs.push_back(singleBytes);
				extract->stackArgs.push_back(makeUint64(std::to_string(offset), _loc));
				extract->stackArgs.push_back(makeUint64(std::to_string(fieldSize), _loc));

				// Cast extracted bytes to the correct type
				std::shared_ptr<awst::Expression> fieldExpr;
				if (fieldType == awst::WType::biguintType())
				{
					auto cast = std::make_shared<awst::ReinterpretCast>();
					cast->sourceLocation = _loc;
					cast->wtype = awst::WType::biguintType();
					cast->expr = std::move(extract);
					fieldExpr = std::move(cast);
				}
				else if (fieldType == awst::WType::uint64Type())
				{
					auto btoi = std::make_shared<awst::IntrinsicCall>();
					btoi->sourceLocation = _loc;
					btoi->opCode = "btoi";
					btoi->wtype = awst::WType::uint64Type();
					btoi->stackArgs.push_back(std::move(extract));
					fieldExpr = std::move(btoi);
				}
				else if (fieldType == awst::WType::boolType())
				{
					auto getbit = std::make_shared<awst::IntrinsicCall>();
					getbit->sourceLocation = _loc;
					getbit->opCode = "getbit";
					getbit->wtype = awst::WType::uint64Type();
					getbit->stackArgs.push_back(std::move(extract));
					getbit->stackArgs.push_back(makeUint64("0", _loc));

					auto cmp = std::make_shared<awst::NumericComparisonExpression>();
					cmp->sourceLocation = _loc;
					cmp->wtype = awst::WType::boolType();
					cmp->lhs = std::move(getbit);
					cmp->rhs = makeUint64("0", _loc);
					cmp->op = awst::NumericComparison::Ne;
					fieldExpr = std::move(cmp);
				}
				else if (fieldType == awst::WType::accountType())
				{
					auto cast = std::make_shared<awst::ReinterpretCast>();
					cast->sourceLocation = _loc;
					cast->wtype = awst::WType::accountType();
					cast->expr = std::move(extract);
					fieldExpr = std::move(cast);
				}
				else
				{
					// bytes or fixed bytes — cast to target type
					auto cast = std::make_shared<awst::ReinterpretCast>();
					cast->sourceLocation = _loc;
					cast->wtype = fieldType;
					cast->expr = std::move(extract);
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
			auto cast = std::make_shared<awst::ReinterpretCast>();
			cast->sourceLocation = _loc;
			cast->wtype = _solidityReturnType;
			cast->expr = std::move(stripPrefix);
			return cast;
		}

		// For bytes/string: return stripped bytes directly
		return stripPrefix;
	}

	// For non-appl returns: emit submit as pending, return type-appropriate default
	auto stmt = std::make_shared<awst::ExpressionStatement>();
	stmt->sourceLocation = _loc;
	stmt->expr = std::move(submit);
	m_pendingStatements.push_back(std::move(stmt));

	return StorageMapper::makeDefaultValue(_solidityReturnType, _loc);
}

} // namespace puyasol::builder
