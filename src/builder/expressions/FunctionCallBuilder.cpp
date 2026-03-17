/// @file FunctionCallBuilder.cpp
/// Handles function calls (require, abi.encode, type casts, struct construction, etc.).

#include "builder/expressions/ExpressionBuilder.h"
#include "builder/storage/StorageMapper.h"
#include "Logger.h"

#include <libsolidity/ast/TypeProvider.h>

#include <algorithm>
#include <functional>
#include <sstream>

namespace puyasol::builder
{

std::shared_ptr<awst::Expression> ExpressionBuilder::buildRequire(
	solidity::frontend::FunctionCall const& _call,
	awst::SourceLocation const& _loc
)
{
	auto const& args = _call.arguments();
	std::shared_ptr<awst::Expression> condition;
	std::optional<std::string> message;

	if (!args.empty())
		condition = build(*args[0]);

	if (args.size() > 1)
	{
		// Check for custom error constructor (e.g., require(cond, Errors.Foo()))
		// before attempting to translate, as error constructors may not be translatable
		bool isCustomError = false;
		if (auto const* errorCall = dynamic_cast<solidity::frontend::FunctionCall const*>(args[1].get()))
		{
			auto const& errExpr = errorCall->expression();
			if (auto const* ma = dynamic_cast<solidity::frontend::MemberAccess const*>(&errExpr))
			{
				message = ma->memberName();
				isCustomError = true;
			}
			else if (auto const* id = dynamic_cast<solidity::frontend::Identifier const*>(&errExpr))
			{
				message = id->name();
				isCustomError = true;
			}
		}
		if (!isCustomError)
		{
			auto msgExpr = build(*args[1]);
			if (auto const* sc = dynamic_cast<awst::StringConstant const*>(msgExpr.get()))
				message = sc->value;
			else
				message = "assertion failed";
		}
	}

	return IntrinsicMapper::createAssert(std::move(condition), std::move(message), _loc);
}

bool ExpressionBuilder::visit(solidity::frontend::FunctionCall const& _node)
{
	using namespace solidity::frontend;

	auto loc = makeLoc(_node.location());
	auto const& rawFuncExpr = _node.expression();

	// Unwrap FunctionCallOptions ({value: X, gas: Y}) to get the actual function expression.
	// Extract the "value" option for use in inner transaction construction.
	solidity::frontend::Expression const* funcExprPtr = &rawFuncExpr;
	std::shared_ptr<awst::Expression> callValueAmount;
	if (auto const* callOpts = dynamic_cast<FunctionCallOptions const*>(&rawFuncExpr))
	{
		funcExprPtr = &callOpts->expression();
		auto const& optNames = callOpts->names();
		auto optValues = callOpts->options();
		for (size_t i = 0; i < optNames.size(); ++i)
		{
			if (*optNames[i] == "value" && i < optValues.size())
			{
				callValueAmount = build(*optValues[i]);
				callValueAmount = implicitNumericCast(
					std::move(callValueAmount), awst::WType::uint64Type(), loc
				);
				break;
			}
		}
	}
	auto const& funcExpr = *funcExprPtr;

	// Handle type conversions
	if (*_node.annotation().kind == FunctionCallKind::TypeConversion)
	{
		if (!_node.arguments().empty())
		{
			auto* targetType = m_typeMapper.map(_node.annotation().type);

			// Special case: address(0) → zero-address constant
			if (targetType == awst::WType::accountType())
			{
				auto const& arg = *_node.arguments()[0];
				if (auto const* lit = dynamic_cast<solidity::frontend::Literal const*>(&arg))
				{
					if (lit->value() == "0")
					{
						// Create 32-byte zero address
						auto e = std::make_shared<awst::AddressConstant>();
						e->sourceLocation = loc;
						e->wtype = awst::WType::accountType();
						e->value = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAY5HFKQ";
						push(e);
						return false;
					}
				}
			}

			auto converted = build(*_node.arguments()[0]);
			// Apply type conversion — try implicit numeric promotion first
			converted = implicitNumericCast(std::move(converted), targetType, loc);

			// Narrowing uint64 cast: e.g., uint32 → uint16 (both map to uint64).
			// Insert AND mask when the Solidity target type has fewer bits.
			if (targetType == awst::WType::uint64Type()
				&& converted->wtype == awst::WType::uint64Type())
			{
				auto const* solTargetType = _node.annotation().type;
				auto const* solSourceType = _node.arguments()[0]->annotation().type;
				if (auto const* targetIntType = dynamic_cast<solidity::frontend::IntegerType const*>(solTargetType))
				{
					unsigned targetBits = targetIntType->numBits();
					unsigned sourceBits = 64;
					if (auto const* srcIntType = dynamic_cast<solidity::frontend::IntegerType const*>(solSourceType))
						sourceBits = srcIntType->numBits();
					else if (dynamic_cast<solidity::frontend::FixedBytesType const*>(solSourceType))
						sourceBits = 64; // bytes→uint already done
					if (targetBits < sourceBits && targetBits < 64)
					{
						auto mask = std::make_shared<awst::IntegerConstant>();
						mask->sourceLocation = loc;
						mask->wtype = awst::WType::uint64Type();
						mask->value = std::to_string((uint64_t(1) << targetBits) - 1);

						auto bitAnd = std::make_shared<awst::UInt64BinaryOperation>();
						bitAnd->sourceLocation = loc;
						bitAnd->wtype = awst::WType::uint64Type();
						bitAnd->left = std::move(converted);
						bitAnd->op = awst::UInt64BinaryOperator::BitAnd;
						bitAnd->right = std::move(mask);
						converted = std::move(bitAnd);
					}
				}
			}

			// Narrowing biguint cast: uint256 → uint160/uint128/etc.
			// Both map to biguint, but we must insert truncation (AND mask)
			// so SafeCast-style overflow checks work correctly on AVM.
			if (targetType == awst::WType::biguintType()
				&& converted->wtype == awst::WType::biguintType())
			{
				auto const* solTargetType = _node.annotation().type;
				auto const* solSourceType = _node.arguments()[0]->annotation().type;
				if (auto const* targetIntType = dynamic_cast<solidity::frontend::IntegerType const*>(solTargetType))
				{
					unsigned targetBits = targetIntType->numBits();
					unsigned sourceBits = 256; // default
					if (auto const* srcIntType = dynamic_cast<solidity::frontend::IntegerType const*>(solSourceType))
						sourceBits = srcIntType->numBits();
					if (targetBits < sourceBits && targetBits < 256)
					{
						// Insert: converted = converted & ((1 << targetBits) - 1)
						auto mask = std::make_shared<awst::IntegerConstant>();
						mask->sourceLocation = loc;
						mask->wtype = awst::WType::biguintType();
						// Compute mask: (2^targetBits) - 1
						solidity::u256 maskVal = (solidity::u256(1) << targetBits) - 1;
						mask->value = maskVal.str();

						auto bitAnd = std::make_shared<awst::BigUIntBinaryOperation>();
						bitAnd->sourceLocation = loc;
						bitAnd->wtype = awst::WType::biguintType();
						bitAnd->left = std::move(converted);
						bitAnd->op = awst::BigUIntBinaryOperator::BitAnd;
						bitAnd->right = std::move(mask);
						converted = std::move(bitAnd);
					}
				}
			}

			if (targetType != converted->wtype)
			{
				bool sourceIsBytes = converted->wtype
					&& converted->wtype->kind() == awst::WTypeKind::Bytes;
				bool targetIsUint = targetType == awst::WType::uint64Type();
				bool targetIsBiguint = targetType == awst::WType::biguintType();
				bool sourceIsUint = converted->wtype == awst::WType::uint64Type();
				bool targetIsBytes = targetType
					&& targetType->kind() == awst::WTypeKind::Bytes;

				if (sourceIsBytes && targetIsUint)
				{
					// bytes[N] → uint64: reinterpret to bytes then btoi
					auto expr = std::move(converted);
					if (expr->wtype != awst::WType::bytesType())
					{
						auto toBytes = std::make_shared<awst::ReinterpretCast>();
						toBytes->sourceLocation = loc;
						toBytes->wtype = awst::WType::bytesType();
						toBytes->expr = std::move(expr);
						expr = std::move(toBytes);
					}
					auto btoi = std::make_shared<awst::IntrinsicCall>();
					btoi->sourceLocation = loc;
					btoi->wtype = awst::WType::uint64Type();
					btoi->opCode = "btoi";
					btoi->stackArgs.push_back(std::move(expr));

					std::shared_ptr<awst::Expression> result = std::move(btoi);

					// Apply narrowing mask when target Solidity type is smaller
					// e.g., uint16(bytes4) → btoi gives uint32 value, need & 0xFFFF
					auto const* solTargetType = _node.annotation().type;
					if (auto const* intType = dynamic_cast<solidity::frontend::IntegerType const*>(solTargetType))
					{
						unsigned targetBits = intType->numBits();
						if (targetBits < 64)
						{
							auto mask = std::make_shared<awst::IntegerConstant>();
							mask->sourceLocation = loc;
							mask->wtype = awst::WType::uint64Type();
							mask->value = std::to_string((uint64_t(1) << targetBits) - 1);

							auto bitAnd = std::make_shared<awst::UInt64BinaryOperation>();
							bitAnd->sourceLocation = loc;
							bitAnd->wtype = awst::WType::uint64Type();
							bitAnd->left = std::move(result);
							bitAnd->op = awst::UInt64BinaryOperator::BitAnd;
							bitAnd->right = std::move(mask);
							result = std::move(bitAnd);
						}
					}

					push(std::move(result));
				}
				else if (sourceIsBytes && targetIsBiguint)
				{
					// bytes[N] → biguint: reinterpret to bytes then to biguint
					auto expr = std::move(converted);
					if (expr->wtype != awst::WType::bytesType())
					{
						auto toBytes = std::make_shared<awst::ReinterpretCast>();
						toBytes->sourceLocation = loc;
						toBytes->wtype = awst::WType::bytesType();
						toBytes->expr = std::move(expr);
						expr = std::move(toBytes);
					}
					auto cast = std::make_shared<awst::ReinterpretCast>();
					cast->sourceLocation = loc;
					cast->wtype = awst::WType::biguintType();
					cast->expr = std::move(expr);
					push(std::move(cast));
				}
				else if (sourceIsUint && targetIsBytes)
				{
					// uint64 → bytes[N]: itob produces 8 bytes, truncate to N if needed
					auto itob = std::make_shared<awst::IntrinsicCall>();
					itob->sourceLocation = loc;
					itob->wtype = awst::WType::bytesType();
					itob->opCode = "itob";
					itob->stackArgs.push_back(std::move(converted));

					// Determine target byte width from Solidity type
					int byteWidth = 8; // default (itob output size)
					auto const* solTargetType = _node.annotation().type;
					if (auto const* fbType = dynamic_cast<solidity::frontend::FixedBytesType const*>(solTargetType))
						byteWidth = static_cast<int>(fbType->numBytes());

					std::shared_ptr<awst::Expression> result = std::move(itob);

					if (byteWidth < 8)
					{
						// itob produces 8 bytes; extract last byteWidth bytes
						// extract3(itob_result, 8 - byteWidth, byteWidth)
						auto offsetConst = std::make_shared<awst::IntegerConstant>();
						offsetConst->sourceLocation = loc;
						offsetConst->wtype = awst::WType::uint64Type();
						offsetConst->value = std::to_string(8 - byteWidth);

						auto widthConst = std::make_shared<awst::IntegerConstant>();
						widthConst->sourceLocation = loc;
						widthConst->wtype = awst::WType::uint64Type();
						widthConst->value = std::to_string(byteWidth);

						auto extract = std::make_shared<awst::IntrinsicCall>();
						extract->sourceLocation = loc;
						extract->wtype = awst::WType::bytesType();
						extract->opCode = "extract3";
						extract->stackArgs.push_back(std::move(result));
						extract->stackArgs.push_back(std::move(offsetConst));
						extract->stackArgs.push_back(std::move(widthConst));
						result = std::move(extract);
					}
					else if (byteWidth > 8)
					{
						// itob produces 8 bytes; pad to byteWidth with leading zeros
						// concat(bzero(byteWidth), itob_result) → extract last byteWidth bytes
						auto widthConst = std::make_shared<awst::IntegerConstant>();
						widthConst->sourceLocation = loc;
						widthConst->wtype = awst::WType::uint64Type();
						widthConst->value = std::to_string(byteWidth);

						auto pad = std::make_shared<awst::IntrinsicCall>();
						pad->sourceLocation = loc;
						pad->wtype = awst::WType::bytesType();
						pad->opCode = "bzero";
						pad->stackArgs.push_back(std::move(widthConst));

						auto cat = std::make_shared<awst::IntrinsicCall>();
						cat->sourceLocation = loc;
						cat->wtype = awst::WType::bytesType();
						cat->opCode = "concat";
						cat->stackArgs.push_back(std::move(pad));
						cat->stackArgs.push_back(std::move(result));

						auto lenExpr = std::make_shared<awst::IntrinsicCall>();
						lenExpr->sourceLocation = loc;
						lenExpr->wtype = awst::WType::uint64Type();
						lenExpr->opCode = "len";
						lenExpr->stackArgs.push_back(cat);

						auto widthConst2 = std::make_shared<awst::IntegerConstant>();
						widthConst2->sourceLocation = loc;
						widthConst2->wtype = awst::WType::uint64Type();
						widthConst2->value = std::to_string(byteWidth);

						auto offsetExpr = std::make_shared<awst::UInt64BinaryOperation>();
						offsetExpr->sourceLocation = loc;
						offsetExpr->wtype = awst::WType::uint64Type();
						offsetExpr->left = std::move(lenExpr);
						offsetExpr->right = std::move(widthConst2);
						offsetExpr->op = awst::UInt64BinaryOperator::Sub;

						auto widthConst3 = std::make_shared<awst::IntegerConstant>();
						widthConst3->sourceLocation = loc;
						widthConst3->wtype = awst::WType::uint64Type();
						widthConst3->value = std::to_string(byteWidth);

						auto extract = std::make_shared<awst::IntrinsicCall>();
						extract->sourceLocation = loc;
						extract->wtype = awst::WType::bytesType();
						extract->opCode = "extract3";
						extract->stackArgs.push_back(std::move(cat));
						extract->stackArgs.push_back(std::move(offsetExpr));
						extract->stackArgs.push_back(std::move(widthConst3));
						result = std::move(extract);
					}

					if (targetType != awst::WType::bytesType())
					{
						auto cast = std::make_shared<awst::ReinterpretCast>();
						cast->sourceLocation = loc;
						cast->wtype = targetType;
						cast->expr = std::move(result);
						push(std::move(cast));
					}
					else
						push(std::move(result));
				}
				else if (isBigUInt(converted->wtype) && targetIsBytes)
				{
					// biguint → bytes[N]: pad/truncate to exact byte width
					// 1. ReinterpretCast biguint → bytes (variable-length)
					auto toBytes = std::make_shared<awst::ReinterpretCast>();
					toBytes->sourceLocation = loc;
					toBytes->wtype = awst::WType::bytesType();
					toBytes->expr = std::move(converted);

					// Determine target byte width from Solidity type
					int byteWidth = 32; // default for bytes32
					auto const* solTargetType = _node.annotation().type;
					if (auto const* fbType = dynamic_cast<solidity::frontend::FixedBytesType const*>(solTargetType))
						byteWidth = static_cast<int>(fbType->numBytes());

					// 2. concat(bzero(width), bytes) → ensure at least width bytes
					auto widthConst = std::make_shared<awst::IntegerConstant>();
					widthConst->sourceLocation = loc;
					widthConst->wtype = awst::WType::uint64Type();
					widthConst->value = std::to_string(byteWidth);

					auto pad = std::make_shared<awst::IntrinsicCall>();
					pad->sourceLocation = loc;
					pad->wtype = awst::WType::bytesType();
					pad->opCode = "bzero";
					pad->stackArgs.push_back(std::move(widthConst));

					auto cat = std::make_shared<awst::IntrinsicCall>();
					cat->sourceLocation = loc;
					cat->wtype = awst::WType::bytesType();
					cat->opCode = "concat";
					cat->stackArgs.push_back(std::move(pad));
					cat->stackArgs.push_back(std::move(toBytes));

					// 3. extract last byteWidth bytes
					auto lenCall = std::make_shared<awst::IntrinsicCall>();
					lenCall->sourceLocation = loc;
					lenCall->wtype = awst::WType::uint64Type();
					lenCall->opCode = "len";
					lenCall->stackArgs.push_back(cat);

					auto wc2 = std::make_shared<awst::IntegerConstant>();
					wc2->sourceLocation = loc;
					wc2->wtype = awst::WType::uint64Type();
					wc2->value = std::to_string(byteWidth);

					auto offset = std::make_shared<awst::IntrinsicCall>();
					offset->sourceLocation = loc;
					offset->wtype = awst::WType::uint64Type();
					offset->opCode = "-";
					offset->stackArgs.push_back(std::move(lenCall));
					offset->stackArgs.push_back(std::move(wc2));

					auto wc3 = std::make_shared<awst::IntegerConstant>();
					wc3->sourceLocation = loc;
					wc3->wtype = awst::WType::uint64Type();
					wc3->value = std::to_string(byteWidth);

					auto extract = std::make_shared<awst::IntrinsicCall>();
					extract->sourceLocation = loc;
					extract->wtype = awst::WType::bytesType();
					extract->opCode = "extract3";
					extract->stackArgs.push_back(std::move(cat));
					extract->stackArgs.push_back(std::move(offset));
					extract->stackArgs.push_back(std::move(wc3));

					// 4. ReinterpretCast to target bytes[N] type if needed
					if (targetType != awst::WType::bytesType())
					{
						auto finalCast = std::make_shared<awst::ReinterpretCast>();
						finalCast->sourceLocation = loc;
						finalCast->wtype = targetType;
						finalCast->expr = std::move(extract);
						push(std::move(finalCast));
					}
					else
						push(std::move(extract));
				}
				else if (auto const* refArr = dynamic_cast<awst::ReferenceArray const*>(targetType))
				{
					// bytes → fixed-size array: decompose into elements
					auto arrSize = refArr->arraySize();
					if (arrSize && *arrSize > 0)
					{
						auto* elemType = refArr->elementType();
						int elemSize = 32;
						if (elemType == awst::WType::uint64Type())
							elemSize = 8;

						auto bytesSource = std::move(converted);
						if (bytesSource->wtype != awst::WType::bytesType())
						{
							auto toBytes = std::make_shared<awst::ReinterpretCast>();
							toBytes->sourceLocation = loc;
							toBytes->wtype = awst::WType::bytesType();
							toBytes->expr = std::move(bytesSource);
							bytesSource = std::move(toBytes);
						}

						auto arr = std::make_shared<awst::NewArray>();
						arr->sourceLocation = loc;
						arr->wtype = targetType;
						for (int i = 0; i < *arrSize; ++i)
						{
							auto extract = std::make_shared<awst::IntrinsicCall>();
							extract->sourceLocation = loc;
							extract->wtype = awst::WType::bytesType();
							extract->opCode = "extract3";
							extract->stackArgs.push_back(bytesSource);
							auto off = std::make_shared<awst::IntegerConstant>();
							off->sourceLocation = loc;
							off->wtype = awst::WType::uint64Type();
							off->value = std::to_string(i * elemSize);
							extract->stackArgs.push_back(std::move(off));
							auto len = std::make_shared<awst::IntegerConstant>();
							len->sourceLocation = loc;
							len->wtype = awst::WType::uint64Type();
							len->value = std::to_string(elemSize);
							extract->stackArgs.push_back(std::move(len));

							if (elemType == awst::WType::biguintType())
							{
								auto cast = std::make_shared<awst::ReinterpretCast>();
								cast->sourceLocation = loc;
								cast->wtype = elemType;
								cast->expr = std::move(extract);
								arr->values.push_back(std::move(cast));
							}
							else if (elemType == awst::WType::uint64Type())
							{
								auto btoi = std::make_shared<awst::IntrinsicCall>();
								btoi->sourceLocation = loc;
								btoi->wtype = awst::WType::uint64Type();
								btoi->opCode = "btoi";
								btoi->stackArgs.push_back(std::move(extract));
								arr->values.push_back(std::move(btoi));
							}
							else
								arr->values.push_back(std::move(extract));
						}
						push(std::move(arr));
					}
					else
					{
						auto arr = std::make_shared<awst::NewArray>();
						arr->sourceLocation = loc;
						arr->wtype = targetType;
						push(std::move(arr));
					}
				}
				else if (sourceIsBytes && targetIsBytes)
				{
					// bytes[M] → bytes[N]: pad or truncate
					int sourceWidth = 0, targetWidth = 0;
					if (auto const* sw = dynamic_cast<awst::BytesWType const*>(converted->wtype))
						sourceWidth = sw->length() ? *sw->length() : 0;
					auto const* solTargetType = _node.annotation().type;
					if (auto const* fbType = dynamic_cast<solidity::frontend::FixedBytesType const*>(solTargetType))
						targetWidth = static_cast<int>(fbType->numBytes());
					if (!targetWidth)
						if (auto const* tw = dynamic_cast<awst::BytesWType const*>(targetType))
							targetWidth = tw->length() ? *tw->length() : 0;

					if (targetWidth > 0 && sourceWidth > 0 && targetWidth != sourceWidth)
					{
						// Reinterpret to raw bytes first
						auto expr = std::move(converted);
						if (expr->wtype != awst::WType::bytesType())
						{
							auto toBytes = std::make_shared<awst::ReinterpretCast>();
							toBytes->sourceLocation = loc;
							toBytes->wtype = awst::WType::bytesType();
							toBytes->expr = std::move(expr);
							expr = std::move(toBytes);
						}

						std::shared_ptr<awst::Expression> result;
						if (targetWidth > sourceWidth)
						{
							// Right-pad: concat(input, bzero(N-M))
							auto padSize = std::make_shared<awst::IntegerConstant>();
							padSize->sourceLocation = loc;
							padSize->wtype = awst::WType::uint64Type();
							padSize->value = std::to_string(targetWidth - sourceWidth);
							auto pad = std::make_shared<awst::IntrinsicCall>();
							pad->sourceLocation = loc;
							pad->wtype = awst::WType::bytesType();
							pad->opCode = "bzero";
							pad->stackArgs.push_back(std::move(padSize));
							auto cat = std::make_shared<awst::IntrinsicCall>();
							cat->sourceLocation = loc;
							cat->wtype = awst::WType::bytesType();
							cat->opCode = "concat";
							cat->stackArgs.push_back(std::move(expr));
							cat->stackArgs.push_back(std::move(pad));
							result = std::move(cat);
						}
						else
						{
							// Truncate: extract3(input, 0, N)
							auto zero = std::make_shared<awst::IntegerConstant>();
							zero->sourceLocation = loc;
							zero->wtype = awst::WType::uint64Type();
							zero->value = "0";
							auto width = std::make_shared<awst::IntegerConstant>();
							width->sourceLocation = loc;
							width->wtype = awst::WType::uint64Type();
							width->value = std::to_string(targetWidth);
							auto extract = std::make_shared<awst::IntrinsicCall>();
							extract->sourceLocation = loc;
							extract->wtype = awst::WType::bytesType();
							extract->opCode = "extract3";
							extract->stackArgs.push_back(std::move(expr));
							extract->stackArgs.push_back(std::move(zero));
							extract->stackArgs.push_back(std::move(width));
							result = std::move(extract);
						}

						auto finalCast = std::make_shared<awst::ReinterpretCast>();
						finalCast->sourceLocation = loc;
						finalCast->wtype = targetType;
						finalCast->expr = std::move(result);
						push(std::move(finalCast));
					}
					else
					{
						// Same size: simple reinterpret
						auto cast = std::make_shared<awst::ReinterpretCast>();
						cast->sourceLocation = loc;
						cast->wtype = targetType;
						cast->expr = std::move(converted);
						push(std::move(cast));
					}
				}
				else
				{
					// Same scalar type: safe to ReinterpretCast
					auto cast = std::make_shared<awst::ReinterpretCast>();
					cast->sourceLocation = loc;
					cast->wtype = targetType;
					cast->expr = std::move(converted);
					push(std::move(cast));
				}
			}
			else
				push(std::move(converted));
		}
		return false;
	}

	// Handle user-defined value type wrap/unwrap: Fr.wrap(x) and Fr.unwrap(y)
	// These are no-ops since UDVT and underlying type both map to the same WType
	if (auto const* funcType = dynamic_cast<solidity::frontend::FunctionType const*>(
		funcExpr.annotation().type))
	{
		if (funcType->kind() == solidity::frontend::FunctionType::Kind::Wrap
			|| funcType->kind() == solidity::frontend::FunctionType::Kind::Unwrap)
		{
			if (!_node.arguments().empty())
			{
				auto val = build(*_node.arguments()[0]);
				auto* targetType = m_typeMapper.map(_node.annotation().type);
				val = implicitNumericCast(std::move(val), targetType, loc);
				push(std::move(val));
			}
			return false;
		}
	}

	// Handle struct creation
	if (*_node.annotation().kind == FunctionCallKind::StructConstructorCall)
	{
		auto* solType = _node.annotation().type;
		auto* wtype = m_typeMapper.map(solType);

		auto const& names = _node.names();
		auto const& args = _node.arguments();

		// Collect field values into an ordered map
		std::map<std::string, std::shared_ptr<awst::Expression>> fieldValues;

		auto const* tupleType = dynamic_cast<awst::WTuple const*>(wtype);
		auto const* arc4StructType = dynamic_cast<awst::ARC4Struct const*>(wtype);
		if (!names.empty())
		{
			for (size_t i = 0; i < names.size(); ++i)
			{
				auto val = build(*args[i]);
				if (tupleType && i < tupleType->types().size())
					val = implicitNumericCast(std::move(val), tupleType->types()[i], loc);
				else if (arc4StructType)
				{
					for (auto const& [fname, ftype]: arc4StructType->fields())
						if (fname == *names[i])
						{
							val = implicitNumericCast(std::move(val), ftype, loc);
							break;
						}
				}
				fieldValues[*names[i]] = std::move(val);
			}
		}
		else
		{
			// Positional args
			if (auto const* structType = dynamic_cast<StructType const*>(solType))
			{
				auto const& members = structType->structDefinition().members();
				for (size_t i = 0; i < args.size() && i < members.size(); ++i)
				{
					auto val = build(*args[i]);
					if (tupleType && i < tupleType->types().size())
						val = implicitNumericCast(std::move(val), tupleType->types()[i], loc);
					else if (arc4StructType && i < arc4StructType->fields().size())
						val = implicitNumericCast(std::move(val), arc4StructType->fields()[i].second, loc);
					fieldValues[members[i]->name()] = std::move(val);
				}
			}
		}

		// Use NewStruct for ARC4Struct, NamedTupleExpression for WTuple
		if (arc4StructType)
		{
			// Wrap field values in ARC4Encode where the value's wtype doesn't match the field's ARC4 type
			for (auto const& [fname, ftype]: arc4StructType->fields())
			{
				auto it = fieldValues.find(fname);
				if (it != fieldValues.end() && it->second->wtype != ftype)
				{
					auto encode = std::make_shared<awst::ARC4Encode>();
					encode->sourceLocation = loc;
					encode->wtype = ftype;
					encode->value = std::move(it->second);
					it->second = std::move(encode);
				}
			}
			auto newStruct = std::make_shared<awst::NewStruct>();
			newStruct->sourceLocation = loc;
			newStruct->wtype = wtype;
			newStruct->values = std::move(fieldValues);
			push(newStruct);
		}
		else
		{
			auto structExpr = std::make_shared<awst::NamedTupleExpression>();
			structExpr->sourceLocation = loc;
			structExpr->wtype = wtype;
			structExpr->values = std::move(fieldValues);
			push(structExpr);
		}
		return false;
	}

	// Handle specific function calls
	if (auto const* memberAccess = dynamic_cast<MemberAccess const*>(&funcExpr))
	{
		std::string memberName = memberAccess->memberName();

		auto const& baseExpr = memberAccess->expression();

		// Handle .call(...) and .call{value: X}("") → inner transaction or stub
		// On EVM: low-level call to another contract.
		// On AVM: translated to inner transaction (payment or app call).
		if (memberName == "call")
		{
			auto receiver = build(baseExpr);
			bool handledAsInnerCall = false;

			if (callValueAmount)
			{
				// .call{value: X}("") → payment inner transaction
				std::map<std::string, std::shared_ptr<awst::Expression>> fields;
				fields["Receiver"] = std::move(receiver);
				fields["Amount"] = std::move(callValueAmount);

				auto create = buildCreateInnerTransaction(TxnTypePay, std::move(fields), loc);

				auto* submitWtype = m_typeMapper.createType<awst::WInnerTransaction>(TxnTypePay);
				auto submit = std::make_shared<awst::SubmitInnerTransaction>();
				submit->sourceLocation = loc;
				submit->wtype = submitWtype;
				submit->itxns.push_back(std::move(create));

				auto stmt = std::make_shared<awst::ExpressionStatement>();
				stmt->sourceLocation = loc;
				stmt->expr = std::move(submit);
				m_pendingStatements.push_back(std::move(stmt));
			}
			else
			{
				// .call(data) without value — detect abi.encodeCall for inner app call
				if (!_node.arguments().empty())
				{
					auto const& dataArg = *_node.arguments()[0];
					if (auto const* encodeCallExpr = dynamic_cast<solidity::frontend::FunctionCall const*>(&dataArg))
					{
						auto const* encodeMA = dynamic_cast<MemberAccess const*>(&encodeCallExpr->expression());
						if (encodeMA && encodeMA->memberName() == "encodeCall"
									&& encodeCallExpr->arguments().size() >= 2)
						{
									// Extract target function from first arg (e.g., IERC20.transfer)
									auto const& targetFnExpr = *encodeCallExpr->arguments()[0];
									solidity::frontend::FunctionDefinition const* targetFuncDef = nullptr;
									
									// The first arg's type annotation is a FunctionType
									if (auto const* fnType = dynamic_cast<solidity::frontend::FunctionType const*>(
												targetFnExpr.annotation().type))
									{
												if (fnType->hasDeclaration())
												{
															targetFuncDef = dynamic_cast<solidity::frontend::FunctionDefinition const*>(
																		&fnType->declaration()
															);
												}
									}
									
									if (targetFuncDef)
									{
												handledAsInnerCall = true;
												
												// Build ARC4 method selector (reuse solTypeToARC4Name logic)
												auto solTypeToARC4 = [this](solidity::frontend::Type const* _type) -> std::string {
															auto* wtype = m_typeMapper.map(_type);
															if (wtype == awst::WType::biguintType())
															{
																// Check signedness from Solidity type
																if (auto const* intT = dynamic_cast<solidity::frontend::IntegerType const*>(_type))
																	return intT->isSigned() ? "int256" : "uint256";
																return "uint256";
															}
															if (wtype == awst::WType::uint64Type())
															{
																if (auto const* intT = dynamic_cast<solidity::frontend::IntegerType const*>(_type))
																	return intT->isSigned()
																		? "int" + std::to_string(intT->numBits())
																		: "uint" + std::to_string(intT->numBits());
																return "uint64";
															}
															if (wtype == awst::WType::boolType()) return "bool";
															if (wtype == awst::WType::accountType()) return "address";
															if (wtype == awst::WType::bytesType()) return "byte[]";
															if (wtype == awst::WType::stringType()) return "string";
															if (wtype->kind() == awst::WTypeKind::Bytes)
															{
																		auto const* bw = static_cast<awst::BytesWType const*>(wtype);
																		if (bw->length().has_value())
																					return "byte[" + std::to_string(bw->length().value()) + "]";
																		return "byte[]";
															}
															if (auto const* structType = dynamic_cast<solidity::frontend::StructType const*>(_type))
																		return "struct " + structType->structDefinition().name();
															return _type->toString(true);
												};
												
												std::string methodSelector = targetFuncDef->name() + "(";
												bool first = true;
												for (auto const& param: targetFuncDef->parameters())
												{
															if (!first) methodSelector += ",";
															methodSelector += solTypeToARC4(param->type());
															first = false;
												}
												methodSelector += ")";
												if (targetFuncDef->returnParameters().size() > 1)
												{
															methodSelector += "(";
															bool firstRet = true;
															for (auto const& retParam: targetFuncDef->returnParameters())
															{
																		if (!firstRet) methodSelector += ",";
																		methodSelector += solTypeToARC4(retParam->type());
																		firstRet = false;
															}
															methodSelector += ")";
												}
												else if (targetFuncDef->returnParameters().size() == 1)
															methodSelector += solTypeToARC4(targetFuncDef->returnParameters()[0]->type());
												else
															methodSelector += "void";

												auto methodConst = std::make_shared<awst::MethodConstant>();
												methodConst->sourceLocation = loc;
												methodConst->wtype = awst::WType::bytesType();
												methodConst->value = methodSelector;
												
												// Build ApplicationArgs tuple
												auto argsTuple = std::make_shared<awst::TupleExpression>();
												argsTuple->sourceLocation = loc;
												argsTuple->items.push_back(std::move(methodConst));
												
												// Extract call arguments from second arg (tuple)
												auto const& argsExpr = *encodeCallExpr->arguments()[1];
												std::vector<solidity::frontend::ASTPointer<solidity::frontend::Expression const>> callArgs;
												if (auto const* tupleExpr = dynamic_cast<solidity::frontend::TupleExpression const*>(&argsExpr))
												{
															for (auto const& comp: tupleExpr->components())
																		if (comp) callArgs.push_back(comp);
												}
												else
															callArgs.push_back(encodeCallExpr->arguments()[1]);
												
												// Encode each argument with proper ARC4 encoding
												for (auto const& arg: callArgs)
												{
															auto argExpr = build(*arg);
															if (argExpr->wtype == awst::WType::bytesType()
																		|| argExpr->wtype->kind() == awst::WTypeKind::Bytes)
															{
																		argsTuple->items.push_back(std::move(argExpr));
															}
															else if (argExpr->wtype == awst::WType::uint64Type())
															{
																		auto itob = std::make_shared<awst::IntrinsicCall>();
																		itob->sourceLocation = loc;
																		itob->wtype = awst::WType::bytesType();
																		itob->opCode = "itob";
																		itob->stackArgs.push_back(std::move(argExpr));
																		argsTuple->items.push_back(std::move(itob));
															}
															else if (argExpr->wtype == awst::WType::biguintType())
															{
																		// biguint → ARC4 uint256 = 32 bytes, left-padded
																		auto cast = std::make_shared<awst::ReinterpretCast>();
																		cast->sourceLocation = loc;
																		cast->wtype = awst::WType::bytesType();
																		cast->expr = std::move(argExpr);

																		auto zeros = std::make_shared<awst::IntrinsicCall>();
																		zeros->sourceLocation = loc;
																		zeros->wtype = awst::WType::bytesType();
																		zeros->opCode = "bzero";
																		zeros->stackArgs.push_back(makeUint64("32", loc));

																		auto padded = std::make_shared<awst::IntrinsicCall>();
																		padded->sourceLocation = loc;
																		padded->wtype = awst::WType::bytesType();
																		padded->opCode = "concat";
																		padded->stackArgs.push_back(std::move(zeros));
																		padded->stackArgs.push_back(std::move(cast));

																		auto lenCall = std::make_shared<awst::IntrinsicCall>();
																		lenCall->sourceLocation = loc;
																		lenCall->wtype = awst::WType::uint64Type();
																		lenCall->opCode = "len";
																		lenCall->stackArgs.push_back(padded);

																		auto offset = std::make_shared<awst::IntrinsicCall>();
																		offset->sourceLocation = loc;
																		offset->wtype = awst::WType::uint64Type();
																		offset->opCode = "-";
																		offset->stackArgs.push_back(std::move(lenCall));
																		offset->stackArgs.push_back(makeUint64("32", loc));

																		auto extracted = std::make_shared<awst::IntrinsicCall>();
																		extracted->sourceLocation = loc;
																		extracted->wtype = awst::WType::bytesType();
																		extracted->opCode = "extract3";
																		extracted->stackArgs.push_back(std::move(padded));
																		extracted->stackArgs.push_back(std::move(offset));
																		extracted->stackArgs.push_back(makeUint64("32", loc));
																		
																		argsTuple->items.push_back(std::move(extracted));
															}
															else if (argExpr->wtype == awst::WType::boolType())
															{
																		// bool → ARC4 bool = 1 byte
																		auto zeroByte = std::make_shared<awst::BytesConstant>();
																		zeroByte->sourceLocation = loc;
																		zeroByte->wtype = awst::WType::bytesType();
																		zeroByte->encoding = awst::BytesEncoding::Base16;
																		zeroByte->value = {0x00};
																		
																		auto setbit = std::make_shared<awst::IntrinsicCall>();
																		setbit->sourceLocation = loc;
																		setbit->wtype = awst::WType::bytesType();
																		setbit->opCode = "setbit";
																		setbit->stackArgs.push_back(std::move(zeroByte));
																		setbit->stackArgs.push_back(makeUint64("0", loc));
																		setbit->stackArgs.push_back(std::move(argExpr));
																		
																		argsTuple->items.push_back(std::move(setbit));
															}
															else if (argExpr->wtype == awst::WType::accountType())
															{
																		// account/address → 32 bytes
																		auto cast = std::make_shared<awst::ReinterpretCast>();
																		cast->sourceLocation = loc;
																		cast->wtype = awst::WType::bytesType();
																		cast->expr = std::move(argExpr);
																		argsTuple->items.push_back(std::move(cast));
															}
															else
															{
																		// Fallback: reinterpret as bytes
																		auto cast = std::make_shared<awst::ReinterpretCast>();
																		cast->sourceLocation = loc;
																		cast->wtype = awst::WType::bytesType();
																		cast->expr = std::move(argExpr);
																		argsTuple->items.push_back(std::move(cast));
															}
												}
												
												// Build WTuple type for args
												std::vector<awst::WType const*> argTypes;
												for (auto const& item: argsTuple->items)
															argTypes.push_back(item->wtype);
												argsTuple->wtype = m_typeMapper.createType<awst::WTuple>(
															std::move(argTypes), std::nullopt
												);
												
												// Convert receiver address → app ID
												std::shared_ptr<awst::Expression> appId;
												if (receiver->wtype == awst::WType::applicationType())
												{
															appId = std::move(receiver);
												}
												else
												{
															std::shared_ptr<awst::Expression> bytesExpr = std::move(receiver);
															if (bytesExpr->wtype == awst::WType::accountType())
															{
																		auto toBytes = std::make_shared<awst::ReinterpretCast>();
																		toBytes->sourceLocation = loc;
																		toBytes->wtype = awst::WType::bytesType();
																		toBytes->expr = std::move(bytesExpr);
																		bytesExpr = std::move(toBytes);
															}
															auto extract = std::make_shared<awst::IntrinsicCall>();
															extract->sourceLocation = loc;
															extract->wtype = awst::WType::bytesType();
															extract->opCode = "extract";
															extract->immediates = {24, 8};
															extract->stackArgs.push_back(std::move(bytesExpr));
															
															auto btoi = std::make_shared<awst::IntrinsicCall>();
															btoi->sourceLocation = loc;
															btoi->wtype = awst::WType::uint64Type();
															btoi->opCode = "btoi";
															btoi->stackArgs.push_back(std::move(extract));
															
															auto cast = std::make_shared<awst::ReinterpretCast>();
															cast->sourceLocation = loc;
															cast->wtype = awst::WType::applicationType();
															cast->expr = std::move(btoi);
															appId = std::move(cast);
												}
												
												std::map<std::string, std::shared_ptr<awst::Expression>> fields;
												fields["ApplicationID"] = std::move(appId);
												fields["OnCompletion"] = makeUint64("0", loc);
												fields["ApplicationArgs"] = std::move(argsTuple);
												
												auto create = buildCreateInnerTransaction(TxnTypeAppl, std::move(fields), loc);
									
									// Submit inner transaction as a pending statement
									auto* submitWtype = m_typeMapper.createType<awst::WInnerTransaction>(TxnTypeAppl);
									auto submit = std::make_shared<awst::SubmitInnerTransaction>();
									submit->sourceLocation = loc;
									submit->wtype = submitWtype;
									submit->itxns.push_back(std::move(create));

									auto submitStmt = std::make_shared<awst::ExpressionStatement>();
									submitStmt->sourceLocation = loc;
									submitStmt->expr = std::move(submit);
									m_prePendingStatements.push_back(std::move(submitStmt));

									// Read LastLog from most recently submitted inner txn
									auto readLog = std::make_shared<awst::IntrinsicCall>();
									readLog->sourceLocation = loc;
									readLog->wtype = awst::WType::bytesType();
									readLog->opCode = "itxn";
									readLog->immediates = {std::string("LastLog")};

									// Strip the 4-byte ARC4 return prefix (0x151f7c75)
									auto stripPrefix = std::make_shared<awst::IntrinsicCall>();
									stripPrefix->sourceLocation = loc;
									stripPrefix->opCode = "extract";
									stripPrefix->immediates = {4, 0};
									stripPrefix->wtype = awst::WType::bytesType();
									stripPrefix->stackArgs.push_back(std::move(readLog));

									// Return (true, return_data)
									auto trueLit2 = std::make_shared<awst::BoolConstant>();
									trueLit2->sourceLocation = loc;
									trueLit2->wtype = awst::WType::boolType();
									trueLit2->value = true;

									auto tuple2 = std::make_shared<awst::TupleExpression>();
									tuple2->sourceLocation = loc;
									auto* tupleWtype2 = m_typeMapper.createType<awst::WTuple>(
										std::vector<awst::WType const*>{awst::WType::boolType(), awst::WType::bytesType()}
									);
									tuple2->wtype = tupleWtype2;
									tuple2->items.push_back(std::move(trueLit2));
									tuple2->items.push_back(std::move(stripPrefix));

									push(tuple2);
												return false;
									}
						}
					}
				}
				
				if (!handledAsInnerCall)
				{
					// Generic .call(data) stub — data is not abi.encodeCall
					Logger::instance().warning(
								"address.call(data) stubbed — returns (true, empty). "
								"Cross-contract calls need inner app call translation.",
								loc
					);
					for (auto const& arg: _node.arguments())
								build(*arg);
				}
			}
			
			if (!handledAsInnerCall)
			{
				// Return (true, empty_bytes) — EVM .call returns (bool, bytes)
				auto trueLit = std::make_shared<awst::BoolConstant>();
				trueLit->sourceLocation = loc;
				trueLit->wtype = awst::WType::boolType();
				trueLit->value = true;
				
				auto emptyBytes = std::make_shared<awst::BytesConstant>();
				emptyBytes->sourceLocation = loc;
				emptyBytes->wtype = awst::WType::bytesType();
				emptyBytes->encoding = awst::BytesEncoding::Base16;
				emptyBytes->value = {};
				
				auto tuple = std::make_shared<awst::TupleExpression>();
				tuple->sourceLocation = loc;
				auto* tupleWtype = m_typeMapper.createType<awst::WTuple>(
							std::vector<awst::WType const*>{awst::WType::boolType(), awst::WType::bytesType()}
				);
				tuple->wtype = tupleWtype;
				tuple->items.push_back(std::move(trueLit));
				tuple->items.push_back(std::move(emptyBytes));
				
				push(tuple);
			}
			return false;

		}

		// Handle .staticcall(...) — route to precompile if address is known
		if (memberName == "staticcall")
		{
			// Try to resolve precompile address from address(N) base expression
			std::optional<uint64_t> precompileAddr;
			if (auto const* baseCall = dynamic_cast<solidity::frontend::FunctionCall const*>(&baseExpr))
			{
				if (baseCall->annotation().kind.set()
					&& *baseCall->annotation().kind == solidity::frontend::FunctionCallKind::TypeConversion
					&& !baseCall->arguments().empty())
				{
					auto const* argType = baseCall->arguments()[0]->annotation().type;
					if (auto const* ratType = dynamic_cast<solidity::frontend::RationalNumberType const*>(argType))
					{
						auto val = ratType->literalValue(nullptr);
						if (val >= 1 && val <= 10)
							precompileAddr = static_cast<uint64_t>(val);
					}
				}
			}

			if (precompileAddr && !_node.arguments().empty())
			{
				// Translate the input data argument
				auto inputData = build(*_node.arguments()[0]);

				std::shared_ptr<awst::Expression> resultBytes;

				auto makeExtract = [&](std::shared_ptr<awst::Expression> source, int offset, int length) {
					auto call = std::make_shared<awst::IntrinsicCall>();
					call->sourceLocation = loc;
					call->wtype = awst::WType::bytesType();
					call->opCode = "extract3";
					call->stackArgs.push_back(std::move(source));
					auto offExpr = std::make_shared<awst::IntegerConstant>();
					offExpr->sourceLocation = loc;
					offExpr->wtype = awst::WType::uint64Type();
					offExpr->value = std::to_string(offset);
					call->stackArgs.push_back(std::move(offExpr));
					auto lenExpr = std::make_shared<awst::IntegerConstant>();
					lenExpr->sourceLocation = loc;
					lenExpr->wtype = awst::WType::uint64Type();
					lenExpr->value = std::to_string(length);
					call->stackArgs.push_back(std::move(lenExpr));
					return call;
				};

				switch (*precompileAddr)
				{
				case 6: // ecAdd: input = [x0:32|y0:32|x1:32|y1:32] → ec_add BN254g1
				{
					Logger::instance().debug("staticcall precompile 0x06: ecAdd → ec_add BN254g1", loc);
					auto pointA = makeExtract(inputData, 0, 64);
					auto pointB = makeExtract(inputData, 64, 64);
					auto ecCall = std::make_shared<awst::IntrinsicCall>();
					ecCall->sourceLocation = loc;
					ecCall->wtype = awst::WType::bytesType();
					ecCall->opCode = "ec_add";
					ecCall->immediates.push_back("BN254g1");
					ecCall->stackArgs.push_back(std::move(pointA));
					ecCall->stackArgs.push_back(std::move(pointB));
					resultBytes = std::move(ecCall);
					break;
				}
				case 7: // ecMul: input = [x:32|y:32|scalar:32] → ec_scalar_mul BN254g1
				{
					Logger::instance().debug("staticcall precompile 0x07: ecMul → ec_scalar_mul BN254g1", loc);
					auto point = makeExtract(inputData, 0, 64);
					auto scalar = makeExtract(inputData, 64, 32);
					auto ecCall = std::make_shared<awst::IntrinsicCall>();
					ecCall->sourceLocation = loc;
					ecCall->wtype = awst::WType::bytesType();
					ecCall->opCode = "ec_scalar_mul";
					ecCall->immediates.push_back("BN254g1");
					ecCall->stackArgs.push_back(std::move(point));
					ecCall->stackArgs.push_back(std::move(scalar));
					resultBytes = std::move(ecCall);
					break;
				}
				case 8: // ecPairing: input groups of 192 bytes
				{
					Logger::instance().debug("staticcall precompile 0x08: ecPairing → ec_pairing_check BN254g1", loc);
					// EVM ecPairing format per pair (192 bytes):
					//   [G1.x:32 | G1.y:32 | G2.x_im:32 | G2.x_re:32 | G2.y_im:32 | G2.y_re:32]
					// AVM ec_pairing_check BN254g1:
					//   stack[0] = G1 points: concat of (G1.x||G1.y) per pair = 64*N bytes
					//   stack[1] = G2 points: concat of (x_re||x_im||y_re||y_im) per pair = 128*N bytes
					// G2 coordinate swap: EVM=(x_im,x_re,y_im,y_re) → AVM=(x_re,x_im,y_re,y_im)

					auto makeConcat = [&](std::shared_ptr<awst::Expression> a, std::shared_ptr<awst::Expression> b) {
						auto c = std::make_shared<awst::IntrinsicCall>();
						c->sourceLocation = loc;
						c->wtype = awst::WType::bytesType();
						c->opCode = "concat";
						c->stackArgs.push_back(std::move(a));
						c->stackArgs.push_back(std::move(b));
						return c;
					};

					// Build G1s and G2s for 2 pairs (384 bytes input)
					// Pair 0: input[0:192], Pair 1: input[192:384]
					// G1s = input[0:64] || input[192:256]
					auto g1_0 = makeExtract(inputData, 0, 64);
					auto g1_1 = makeExtract(inputData, 192, 64);
					auto g1s = makeConcat(std::move(g1_0), std::move(g1_1));

					// G2 pair 0 (EVM): input[64:192] = [x_im:32|x_re:32|y_im:32|y_re:32]
					// G2 pair 0 (AVM): [x_re:32|x_im:32|y_re:32|y_im:32]
					auto g2_0_xre = makeExtract(inputData, 96, 32);
					auto g2_0_xim = makeExtract(inputData, 64, 32);
					auto g2_0_yre = makeExtract(inputData, 160, 32);
					auto g2_0_yim = makeExtract(inputData, 128, 32);
					auto g2_0 = makeConcat(
						makeConcat(std::move(g2_0_xre), std::move(g2_0_xim)),
						makeConcat(std::move(g2_0_yre), std::move(g2_0_yim))
					);

					// G2 pair 1 (EVM): input[256:384] = [x_im:32|x_re:32|y_im:32|y_re:32]
					auto g2_1_xre = makeExtract(inputData, 288, 32);
					auto g2_1_xim = makeExtract(inputData, 256, 32);
					auto g2_1_yre = makeExtract(inputData, 352, 32);
					auto g2_1_yim = makeExtract(inputData, 320, 32);
					auto g2_1 = makeConcat(
						makeConcat(std::move(g2_1_xre), std::move(g2_1_xim)),
						makeConcat(std::move(g2_1_yre), std::move(g2_1_yim))
					);

					auto g2s = makeConcat(std::move(g2_0), std::move(g2_1));

					auto ecCall = std::make_shared<awst::IntrinsicCall>();
					ecCall->sourceLocation = loc;
					ecCall->wtype = awst::WType::boolType();
					ecCall->opCode = "ec_pairing_check";
					ecCall->immediates.push_back("BN254g1");
					ecCall->stackArgs.push_back(std::move(g1s));
					ecCall->stackArgs.push_back(std::move(g2s));

					// ec_pairing_check returns bool directly (1 or 0)
					// The Solidity code expects (bool success, bytes result) where
					// result is ABI-encoded bool (32 bytes, last byte = 0/1)
					// Build: result = itob(ecResult ? 1 : 0) padded to 32 bytes

					// First, convert bool to uint64
					auto boolToInt = std::make_shared<awst::IntrinsicCall>();
					boolToInt->sourceLocation = loc;
					boolToInt->wtype = awst::WType::uint64Type();
					boolToInt->opCode = "select";
					auto zero64 = std::make_shared<awst::IntegerConstant>();
					zero64->sourceLocation = loc;
					zero64->wtype = awst::WType::uint64Type();
					zero64->value = "0";
					auto one64 = std::make_shared<awst::IntegerConstant>();
					one64->sourceLocation = loc;
					one64->wtype = awst::WType::uint64Type();
					one64->value = "1";
					boolToInt->stackArgs.push_back(std::move(zero64));
					boolToInt->stackArgs.push_back(std::move(one64));
					boolToInt->stackArgs.push_back(std::move(ecCall));

					// itob gives 8 bytes, pad to 32 with zeros: bzero(24) || itob(val)
					auto itob = std::make_shared<awst::IntrinsicCall>();
					itob->sourceLocation = loc;
					itob->wtype = awst::WType::bytesType();
					itob->opCode = "itob";
					itob->stackArgs.push_back(std::move(boolToInt));

					auto padding = std::make_shared<awst::IntrinsicCall>();
					padding->sourceLocation = loc;
					padding->wtype = awst::WType::bytesType();
					padding->opCode = "bzero";
					auto pad24 = std::make_shared<awst::IntegerConstant>();
					pad24->sourceLocation = loc;
					pad24->wtype = awst::WType::uint64Type();
					pad24->value = "24";
					padding->stackArgs.push_back(std::move(pad24));

					resultBytes = makeConcat(std::move(padding), std::move(itob));
					break;
				}
				default:
					Logger::instance().warning(
						"address.staticcall to precompile 0x" + std::to_string(*precompileAddr) +
						" not yet supported on AVM",
						loc
					);
					resultBytes = nullptr;
					break;
				}

				if (resultBytes)
				{
					// Return (true, resultBytes) tuple
					auto trueLit = std::make_shared<awst::BoolConstant>();
					trueLit->sourceLocation = loc;
					trueLit->wtype = awst::WType::boolType();
					trueLit->value = true;

					auto tuple = std::make_shared<awst::TupleExpression>();
					tuple->sourceLocation = loc;
					auto* tupleWtype = m_typeMapper.createType<awst::WTuple>(
						std::vector<awst::WType const*>{awst::WType::boolType(), awst::WType::bytesType()}
					);
					tuple->wtype = tupleWtype;
					tuple->items.push_back(std::move(trueLit));
					tuple->items.push_back(std::move(resultBytes));

					push(tuple);
					return false;
				}
			}

			// Fallback: stub for non-precompile or unsupported precompile addresses
			for (auto const& arg: _node.arguments())
				build(*arg);

			Logger::instance().warning(
				"address.staticcall(data) stubbed — returns (true, empty).",
				loc
			);

			auto trueLit = std::make_shared<awst::BoolConstant>();
			trueLit->sourceLocation = loc;
			trueLit->wtype = awst::WType::boolType();
			trueLit->value = true;

			auto emptyBytes = std::make_shared<awst::BytesConstant>();
			emptyBytes->sourceLocation = loc;
			emptyBytes->wtype = awst::WType::bytesType();
			emptyBytes->encoding = awst::BytesEncoding::Base16;
			emptyBytes->value = {};

			auto tuple = std::make_shared<awst::TupleExpression>();
			tuple->sourceLocation = loc;
			auto* tupleWtype = m_typeMapper.createType<awst::WTuple>(
				std::vector<awst::WType const*>{awst::WType::boolType(), awst::WType::bytesType()}
			);
			tuple->wtype = tupleWtype;
			tuple->items.push_back(std::move(trueLit));
			tuple->items.push_back(std::move(emptyBytes));

			push(tuple);
			return false;
		}

		// Handle .delegatecall(...) → stub as (true, empty_bytes)
		// AVM has no delegatecall equivalent; stub for compilation
		if (memberName == "delegatecall")
		{
			Logger::instance().warning(
				"address.delegatecall() stubbed — returns (true, empty). "
				"AVM has no delegatecall equivalent.",
				loc
			);

			auto trueLit = std::make_shared<awst::BoolConstant>();
			trueLit->sourceLocation = loc;
			trueLit->wtype = awst::WType::boolType();
			trueLit->value = true;

			auto emptyBytes = std::make_shared<awst::BytesConstant>();
			emptyBytes->sourceLocation = loc;
			emptyBytes->wtype = awst::WType::bytesType();
			emptyBytes->encoding = awst::BytesEncoding::Base16;
			emptyBytes->value = {};

			auto tuple = std::make_shared<awst::TupleExpression>();
			tuple->sourceLocation = loc;
			auto* tupleWtype = m_typeMapper.createType<awst::WTuple>(
				std::vector<awst::WType const*>{awst::WType::boolType(), awst::WType::bytesType()}
			);
			tuple->wtype = tupleWtype;
			tuple->items.push_back(std::move(trueLit));
			tuple->items.push_back(std::move(emptyBytes));

			push(tuple);
			return false;
		}

		// Handle address.transfer / address.send → payment inner transaction
		if (memberName == "transfer" || memberName == "send")
		{
			auto const* baseType = baseExpr.annotation().type;
			if (baseType
				&& baseType->category() == solidity::frontend::Type::Category::Address
				&& _node.arguments().size() == 1)
			{
				auto receiver = build(baseExpr);
				auto amount = build(*_node.arguments()[0]);
				amount = implicitNumericCast(
					std::move(amount), awst::WType::uint64Type(), loc
				);

				std::map<std::string, std::shared_ptr<awst::Expression>> fields;
				fields["Receiver"] = std::move(receiver);
				fields["Amount"] = std::move(amount);

				auto create = buildCreateInnerTransaction(
					TxnTypePay, std::move(fields), loc
				);
				// .transfer() returns void (reverts on failure)
				// .send() returns bool (on Algorand, always true if we reach here)
				auto* retType = (memberName == "send")
					? awst::WType::boolType()
					: awst::WType::voidType();
				push(buildSubmitAndReturn(
					std::move(create), retType, loc
				));
				return false;
			}
		}

		// push/pop/length on arrays (not struct types with library functions of the same name)
		auto const* baseAnnType = baseExpr.annotation().type;
		bool isArrayLike = baseAnnType
			&& (dynamic_cast<solidity::frontend::ArrayType const*>(baseAnnType)
				|| baseAnnType->category() == solidity::frontend::Type::Category::FixedBytes);
		if (isArrayLike && (memberName == "push" || memberName == "pop" || memberName == "length"))
		{
			// Check if this is a box-stored dynamic array state variable
			bool isBoxArray = false;
			std::string arrayVarName;
			if (auto const* ident = dynamic_cast<solidity::frontend::Identifier const*>(&baseExpr))
			{
				if (auto const* varDecl = dynamic_cast<solidity::frontend::VariableDeclaration const*>(
					ident->annotation().referencedDeclaration))
				{
					if (varDecl->isStateVariable() && StorageMapper::shouldUseBoxStorage(*varDecl)
						&& dynamic_cast<solidity::frontend::ArrayType const*>(varDecl->type()))
					{
						isBoxArray = true;
						arrayVarName = ident->name();
					}
				}
			}

			if (isBoxArray)
			{
				// Box-backed dynamic array: single box with packed elements.
				// Read the array from box storage, apply operation, write back.
				auto const* solArrType = dynamic_cast<solidity::frontend::ArrayType const*>(
					dynamic_cast<solidity::frontend::VariableDeclaration const*>(
						dynamic_cast<solidity::frontend::Identifier const*>(&baseExpr)
							->annotation().referencedDeclaration
					)->type()
				);
				auto* rawElemType = m_typeMapper.map(solArrType->baseType());
				auto* elemType = m_typeMapper.mapToARC4Type(rawElemType);
				auto* arrWType = m_typeMapper.createType<awst::ReferenceArray>(
					elemType, false, std::nullopt
				);

				// Build BoxValueExpression wrapped in StateGet for safe access
				// (returns empty array if box doesn't exist yet)
				auto boxKey = std::make_shared<awst::BytesConstant>();
				boxKey->sourceLocation = loc;
				boxKey->wtype = awst::WType::boxKeyType();
				boxKey->encoding = awst::BytesEncoding::Utf8;
				boxKey->value = std::vector<uint8_t>(arrayVarName.begin(), arrayVarName.end());

				auto boxExpr = std::make_shared<awst::BoxValueExpression>();
				boxExpr->sourceLocation = loc;
				boxExpr->wtype = arrWType;
				boxExpr->key = boxKey;
				boxExpr->existsAssertionMessage = std::nullopt;

				// Wrap in StateGet with empty array default
				auto emptyArr = std::make_shared<awst::NewArray>();
				emptyArr->sourceLocation = loc;
				emptyArr->wtype = arrWType;

				auto stateGet = std::make_shared<awst::StateGet>();
				stateGet->sourceLocation = loc;
				stateGet->wtype = arrWType;
				stateGet->field = boxExpr;
				stateGet->defaultValue = emptyArr;

				// Use boxExpr for writes, stateGet for reads
				std::shared_ptr<awst::Expression> readExpr = stateGet;
				std::shared_ptr<awst::Expression> writeExpr = boxExpr;

				if (memberName == "length")
				{
					auto e = std::make_shared<awst::ArrayLength>();
					e->sourceLocation = loc;
					e->wtype = awst::WType::uint64Type();
					e->array = readExpr;  // StateGet returns empty array if box missing
					push(e);
				}
				else if (memberName == "push" && !_node.arguments().empty())
				{
					auto val = build(*_node.arguments()[0]);

					// ARC4Encode the value for box storage
					auto encoded = std::make_shared<awst::ARC4Encode>();
					encoded->sourceLocation = loc;
					encoded->wtype = elemType;
					encoded->value = std::move(val);

					// Wrap in a single-element array for ArrayExtend
					auto singleArr = std::make_shared<awst::NewArray>();
					singleArr->sourceLocation = loc;
					singleArr->wtype = arrWType;
					singleArr->values.push_back(std::move(encoded));

					auto e = std::make_shared<awst::ArrayExtend>();
					e->sourceLocation = loc;
					e->wtype = awst::WType::voidType();
					e->base = writeExpr;  // write to box directly
					e->other = std::move(singleArr);
					push(e);
				}
				else if (memberName == "pop")
				{
					auto popExpr = std::make_shared<awst::ArrayPop>();
					popExpr->sourceLocation = loc;
					popExpr->wtype = elemType;
					popExpr->base = writeExpr;  // mutates box

					// ARC4Decode the popped value back to native type
					auto decode = std::make_shared<awst::ARC4Decode>();
					decode->sourceLocation = loc;
					decode->wtype = rawElemType;
					decode->value = std::move(popExpr);
					push(decode);
				}
			}
			else
			{
				auto base = build(baseExpr);
				if (memberName == "length")
				{
					auto e = std::make_shared<awst::ArrayLength>();
					e->sourceLocation = loc;
					e->wtype = awst::WType::uint64Type();
					e->array = std::move(base);
					push(e);
				}
				else if (memberName == "push" && !_node.arguments().empty())
				{
					// array.push(val) — use ArrayExtend (mutates in place, returns void)
					auto val = build(*_node.arguments()[0]);
					auto* baseWtype = base->wtype;

					// Wrap val in a single-element array
					auto singleArr = std::make_shared<awst::NewArray>();
					singleArr->sourceLocation = loc;
					singleArr->wtype = baseWtype;
					singleArr->values.push_back(std::move(val));

					auto e = std::make_shared<awst::ArrayExtend>();
					e->sourceLocation = loc;
					e->wtype = awst::WType::voidType();
					e->base = std::move(base);
					e->other = std::move(singleArr);

					push(e);
				}
				else if (memberName == "pop")
				{
					auto e = std::make_shared<awst::ArrayPop>();
					e->sourceLocation = loc;
					e->wtype = awst::WType::voidType();
					e->base = std::move(base);
					push(e);
				}
			}
			return false;
		}

		// ERC20 token calls (transfer, transferFrom, balanceOf, approve, allowance)
		// are now handled as regular external interface calls via inner app calls.
		// They fall through to the external call handler below (line ~3129).
	}

	// Check for require/assert
	if (auto const* identifier = dynamic_cast<Identifier const*>(&funcExpr))
	{
		std::string name = identifier->name();

		if (name == "require" || name == "assert")
		{
			push(buildRequire(_node, loc));
			return false;
		}

		if (name == "revert")
		{
			auto assertExpr = std::make_shared<awst::AssertExpression>();
			assertExpr->sourceLocation = loc;
			assertExpr->wtype = awst::WType::voidType();
			auto falseLit = std::make_shared<awst::BoolConstant>();
			falseLit->sourceLocation = loc;
			falseLit->wtype = awst::WType::boolType();
			falseLit->value = false;
			assertExpr->condition = std::move(falseLit);
			if (!_node.arguments().empty())
			{
				if (auto const* lit = dynamic_cast<Literal const*>(_node.arguments()[0].get()))
					assertExpr->errorMessage = lit->value();
				else
					assertExpr->errorMessage = "revert";
			}
			else
				assertExpr->errorMessage = "revert";
			push(assertExpr);
			return false;
		}

		if (name == "keccak256")
		{
			auto call = std::make_shared<awst::IntrinsicCall>();
			call->sourceLocation = loc;
			call->opCode = "keccak256";
			call->wtype = awst::WType::bytesType();
			for (auto const& arg: _node.arguments())
				call->stackArgs.push_back(build(*arg));
			push(call);
			return false;
		}

		if (name == "sha256")
		{
			auto call = std::make_shared<awst::IntrinsicCall>();
			call->sourceLocation = loc;
			call->opCode = "sha256";
			call->wtype = awst::WType::bytesType();
			for (auto const& arg: _node.arguments())
				call->stackArgs.push_back(build(*arg));
			push(call);
			return false;
		}

		// mulmod(x, y, z) → (x * y) % z using biguint full precision
		if (name == "mulmod" && _node.arguments().size() == 3)
		{
			auto x = build(*_node.arguments()[0]);
			auto y = build(*_node.arguments()[1]);
			auto z = build(*_node.arguments()[2]);

			// Promote all to biguint if needed
			auto promoteToBigUInt = [&](std::shared_ptr<awst::Expression>& operand)
			{
				if (operand->wtype == awst::WType::uint64Type())
				{
					auto itob = std::make_shared<awst::IntrinsicCall>();
					itob->sourceLocation = loc;
					itob->wtype = awst::WType::bytesType();
					itob->opCode = "itob";
					itob->stackArgs.push_back(std::move(operand));
					auto cast = std::make_shared<awst::ReinterpretCast>();
					cast->sourceLocation = loc;
					cast->wtype = awst::WType::biguintType();
					cast->expr = std::move(itob);
					operand = std::move(cast);
				}
			};
			promoteToBigUInt(x);
			promoteToBigUInt(y);
			promoteToBigUInt(z);

			// x * y
			auto mul = std::make_shared<awst::BigUIntBinaryOperation>();
			mul->sourceLocation = loc;
			mul->wtype = awst::WType::biguintType();
			mul->left = std::move(x);
			mul->right = std::move(y);
			mul->op = awst::BigUIntBinaryOperator::Mult;

			// (x * y) % z
			auto mod = std::make_shared<awst::BigUIntBinaryOperation>();
			mod->sourceLocation = loc;
			mod->wtype = awst::WType::biguintType();
			mod->left = std::move(mul);
			mod->right = std::move(z);
			mod->op = awst::BigUIntBinaryOperator::Mod;

			push(mod);
			return false;
		}

		// addmod(x, y, z) → (x + y) % z using biguint full precision
		if (name == "addmod" && _node.arguments().size() == 3)
		{
			auto x = build(*_node.arguments()[0]);
			auto y = build(*_node.arguments()[1]);
			auto z = build(*_node.arguments()[2]);

			auto promoteToBigUInt = [&](std::shared_ptr<awst::Expression>& operand)
			{
				if (operand->wtype == awst::WType::uint64Type())
				{
					auto itob = std::make_shared<awst::IntrinsicCall>();
					itob->sourceLocation = loc;
					itob->wtype = awst::WType::bytesType();
					itob->opCode = "itob";
					itob->stackArgs.push_back(std::move(operand));
					auto cast = std::make_shared<awst::ReinterpretCast>();
					cast->sourceLocation = loc;
					cast->wtype = awst::WType::biguintType();
					cast->expr = std::move(itob);
					operand = std::move(cast);
				}
			};
			promoteToBigUInt(x);
			promoteToBigUInt(y);
			promoteToBigUInt(z);

			auto add = std::make_shared<awst::BigUIntBinaryOperation>();
			add->sourceLocation = loc;
			add->wtype = awst::WType::biguintType();
			add->left = std::move(x);
			add->right = std::move(y);
			add->op = awst::BigUIntBinaryOperator::Add;

			auto mod = std::make_shared<awst::BigUIntBinaryOperation>();
			mod->sourceLocation = loc;
			mod->wtype = awst::WType::biguintType();
			mod->left = std::move(add);
			mod->right = std::move(z);
			mod->op = awst::BigUIntBinaryOperator::Mod;

			push(mod);
			return false;
		}

		// gasleft() → 0 (no equivalent on Algorand)
		if (name == "gasleft")
		{
			Logger::instance().warning("gasleft() has no Algorand equivalent, returning 0", loc);
			auto e = std::make_shared<awst::IntegerConstant>();
			e->sourceLocation = loc;
			e->wtype = awst::WType::biguintType();
			e->value = "0";
			push(e);
			return false;
		}

		// ecrecover(digest, v, r, s) → address
		// Pipeline: ecdsa_pk_recover → concat(X,Y) → keccak256 → extract last 20 bytes → pad to 32
		if (name == "ecrecover" && _node.arguments().size() == 4)
		{
			// Build the 4 arguments: digest (bytes32), v (uint8), r (bytes32), s (bytes32)
			auto digest = build(*_node.arguments()[0]);
			auto vArg = build(*_node.arguments()[1]);
			auto rArg = build(*_node.arguments()[2]);
			auto sArg = build(*_node.arguments()[3]);

			// Ensure digest, r, s are bytes (they may be biguint from keccak256)
			auto toBytes = [&](std::shared_ptr<awst::Expression> expr) -> std::shared_ptr<awst::Expression> {
				if (expr->wtype != awst::WType::bytesType())
				{
					auto cast = std::make_shared<awst::ReinterpretCast>();
					cast->sourceLocation = loc;
					cast->wtype = awst::WType::bytesType();
					cast->expr = std::move(expr);
					return cast;
				}
				return expr;
			};

			auto msgHash = toBytes(std::move(digest));
			auto r = toBytes(std::move(rArg));
			auto s = toBytes(std::move(sArg));

			// Compute recovery_id = v - 27 as uint64
			// v may be uint8/uint64 — need to get to uint64, then subtract 27
			std::shared_ptr<awst::Expression> vUint64;
			if (vArg->wtype == awst::WType::uint64Type())
				vUint64 = std::move(vArg);
			else if (vArg->wtype == awst::WType::biguintType())
			{
				// biguint → bytes → btoi
				auto vb = std::make_shared<awst::ReinterpretCast>();
				vb->sourceLocation = loc;
				vb->wtype = awst::WType::bytesType();
				vb->expr = std::move(vArg);
				auto bi = std::make_shared<awst::IntrinsicCall>();
				bi->sourceLocation = loc;
				bi->wtype = awst::WType::uint64Type();
				bi->opCode = "btoi";
				bi->stackArgs.push_back(std::move(vb));
				vUint64 = std::move(bi);
			}
			else
				vUint64 = std::move(vArg); // assume uint64-compatible

			// recoveryId = v - 27
			auto twentySeven = std::make_shared<awst::IntegerConstant>();
			twentySeven->sourceLocation = loc;
			twentySeven->wtype = awst::WType::uint64Type();
			twentySeven->value = "27";

			auto recoveryId = std::make_shared<awst::UInt64BinaryOperation>();
			recoveryId->sourceLocation = loc;
			recoveryId->wtype = awst::WType::uint64Type();
			recoveryId->op = awst::UInt64BinaryOperator::Sub;
			recoveryId->left = std::move(vUint64);
			recoveryId->right = std::move(twentySeven);

			// ecdsa_pk_recover Secp256k1 → (pubkey_x: bytes, pubkey_y: bytes)
			awst::WType const* tupleType = m_typeMapper.createType<awst::WTuple>(
				std::vector<awst::WType const*>{awst::WType::bytesType(), awst::WType::bytesType()}
			);

			auto ecdsaRecover = std::make_shared<awst::IntrinsicCall>();
			ecdsaRecover->sourceLocation = loc;
			ecdsaRecover->wtype = tupleType;
			ecdsaRecover->opCode = "ecdsa_pk_recover";
			ecdsaRecover->immediates.push_back("Secp256k1");
			ecdsaRecover->stackArgs.push_back(std::move(msgHash));
			ecdsaRecover->stackArgs.push_back(std::move(recoveryId));
			ecdsaRecover->stackArgs.push_back(std::move(r));
			ecdsaRecover->stackArgs.push_back(std::move(s));

			// Store tuple in a temp variable
			std::string tmpVar = "__ecrecover_result";

			auto tmpTarget = std::make_shared<awst::VarExpression>();
			tmpTarget->sourceLocation = loc;
			tmpTarget->name = tmpVar;
			tmpTarget->wtype = tupleType;

			auto assignTuple = std::make_shared<awst::AssignmentStatement>();
			assignTuple->sourceLocation = loc;
			assignTuple->target = tmpTarget;
			assignTuple->value = std::move(ecdsaRecover);
			m_pendingStatements.push_back(std::move(assignTuple));

			// Extract pubkey_x (index 0) and pubkey_y (index 1)
			auto tmpRead0 = std::make_shared<awst::VarExpression>();
			tmpRead0->sourceLocation = loc;
			tmpRead0->name = tmpVar;
			tmpRead0->wtype = tupleType;

			auto pubkeyX = std::make_shared<awst::TupleItemExpression>();
			pubkeyX->sourceLocation = loc;
			pubkeyX->wtype = awst::WType::bytesType();
			pubkeyX->base = std::move(tmpRead0);
			pubkeyX->index = 0;

			auto tmpRead1 = std::make_shared<awst::VarExpression>();
			tmpRead1->sourceLocation = loc;
			tmpRead1->name = tmpVar;
			tmpRead1->wtype = tupleType;

			auto pubkeyY = std::make_shared<awst::TupleItemExpression>();
			pubkeyY->sourceLocation = loc;
			pubkeyY->wtype = awst::WType::bytesType();
			pubkeyY->base = std::move(tmpRead1);
			pubkeyY->index = 1;

			// concat(pubkey_x, pubkey_y) → 64 bytes
			auto pubkeyConcat = std::make_shared<awst::IntrinsicCall>();
			pubkeyConcat->sourceLocation = loc;
			pubkeyConcat->wtype = awst::WType::bytesType();
			pubkeyConcat->opCode = "concat";
			pubkeyConcat->stackArgs.push_back(std::move(pubkeyX));
			pubkeyConcat->stackArgs.push_back(std::move(pubkeyY));

			// keccak256(concat) → 32 bytes
			auto hash = std::make_shared<awst::IntrinsicCall>();
			hash->sourceLocation = loc;
			hash->wtype = awst::WType::bytesType();
			hash->opCode = "keccak256";
			hash->stackArgs.push_back(std::move(pubkeyConcat));

			// extract3(hash, 12, 20) → last 20 bytes (Ethereum address)
			auto off12 = std::make_shared<awst::IntegerConstant>();
			off12->sourceLocation = loc;
			off12->wtype = awst::WType::uint64Type();
			off12->value = "12";
			auto len20 = std::make_shared<awst::IntegerConstant>();
			len20->sourceLocation = loc;
			len20->wtype = awst::WType::uint64Type();
			len20->value = "20";

			auto addrBytes = std::make_shared<awst::IntrinsicCall>();
			addrBytes->sourceLocation = loc;
			addrBytes->wtype = awst::WType::bytesType();
			addrBytes->opCode = "extract3";
			addrBytes->stackArgs.push_back(std::move(hash));
			addrBytes->stackArgs.push_back(std::move(off12));
			addrBytes->stackArgs.push_back(std::move(len20));

			// Left-pad to 32 bytes: concat(bzero(12), addr) for Solidity address format
			auto bzero12 = std::make_shared<awst::IntrinsicCall>();
			bzero12->sourceLocation = loc;
			bzero12->wtype = awst::WType::bytesType();
			bzero12->opCode = "bzero";
			auto twelve = std::make_shared<awst::IntegerConstant>();
			twelve->sourceLocation = loc;
			twelve->wtype = awst::WType::uint64Type();
			twelve->value = "12";
			bzero12->stackArgs.push_back(std::move(twelve));

			auto paddedAddr = std::make_shared<awst::IntrinsicCall>();
			paddedAddr->sourceLocation = loc;
			paddedAddr->wtype = awst::WType::bytesType();
			paddedAddr->opCode = "concat";
			paddedAddr->stackArgs.push_back(std::move(bzero12));
			paddedAddr->stackArgs.push_back(std::move(addrBytes));

			// Cast to account type (Algorand address = 32 bytes)
			auto result = std::make_shared<awst::ReinterpretCast>();
			result->sourceLocation = loc;
			result->wtype = awst::WType::accountType();
			result->expr = std::move(paddedAddr);

			push(result);
			return false;
		}
	}

	// Handle `new` expressions: new bytes(N), new T[](N)
	if (dynamic_cast<NewExpression const*>(&funcExpr))
	{
		auto* resultType = m_typeMapper.map(_node.annotation().type);
		if (resultType && resultType->kind() == awst::WTypeKind::Bytes)
		{
			// new bytes(N) → bzero(N) intrinsic
			auto sizeExpr = !_node.arguments().empty()
				? build(*_node.arguments()[0])
				: nullptr;
			if (sizeExpr)
				sizeExpr = implicitNumericCast(std::move(sizeExpr), awst::WType::uint64Type(), loc);

			auto e = std::make_shared<awst::IntrinsicCall>();
			e->sourceLocation = loc;
			e->wtype = resultType;
			e->opCode = "bzero";
			if (sizeExpr)
				e->stackArgs.push_back(std::move(sizeExpr));
			push(e);
			return false;
		}
		else if (resultType && resultType->kind() == awst::WTypeKind::ReferenceArray)
		{
			auto* refArr = dynamic_cast<awst::ReferenceArray const*>(resultType);
			auto e = std::make_shared<awst::NewArray>();
			e->sourceLocation = loc;
			e->wtype = resultType;

			// Try to resolve N at compile time so the array is properly sized
			if (!_node.arguments().empty() && refArr)
			{
				unsigned long long n = 0;
				auto const* argType = _node.arguments()[0]->annotation().type;
				if (auto const* ratType = dynamic_cast<RationalNumberType const*>(argType))
				{
					auto val = ratType->literalValue(nullptr);
					if (val > 0)
						n = static_cast<unsigned long long>(val);
				}
				// Also try resolving from tracked constant locals
				if (n == 0)
				{
					if (auto const* ident = dynamic_cast<Identifier const*>(&*_node.arguments()[0]))
					{
						auto constVal = getConstantLocal(ident->annotation().referencedDeclaration);
						if (constVal > 0)
							n = static_cast<unsigned long long>(constVal);
					}
				}

				if (n > 0)
				{
					// Compile-time known size: create N default values
					for (unsigned long long i = 0; i < n; ++i)
						e->values.push_back(
							StorageMapper::makeDefaultValue(refArr->elementType(), loc));
				}
				else if (!_node.arguments().empty())
				{
					// Runtime-sized: create empty array + loop to extend N times.
					// emit: __arr = NewArray(); __i = 0; while (__i < n) { __arr.extend(default); __i++; }
					static int rtArrayCounter = 0;
					int tc = rtArrayCounter++;
					std::string arrName = "__rt_arr_" + std::to_string(tc);
					std::string idxName = "__rt_idx_" + std::to_string(tc);

					auto sizeExpr = build(*_node.arguments()[0]);
					sizeExpr = implicitNumericCast(
						std::move(sizeExpr), awst::WType::uint64Type(), loc
					);

					// __arr = NewArray()
					auto arrVar = std::make_shared<awst::VarExpression>();
					arrVar->sourceLocation = loc;
					arrVar->wtype = resultType;
					arrVar->name = arrName;

					auto initArr = std::make_shared<awst::AssignmentStatement>();
					initArr->sourceLocation = loc;
					initArr->target = arrVar;
					initArr->value = e; // empty NewArray
					m_prePendingStatements.push_back(std::move(initArr));

					// __i = 0
					auto idxVar = std::make_shared<awst::VarExpression>();
					idxVar->sourceLocation = loc;
					idxVar->wtype = awst::WType::uint64Type();
					idxVar->name = idxName;

					auto initIdx = std::make_shared<awst::AssignmentStatement>();
					initIdx->sourceLocation = loc;
					initIdx->target = idxVar;
					auto zero = std::make_shared<awst::IntegerConstant>();
					zero->sourceLocation = loc;
					zero->wtype = awst::WType::uint64Type();
					zero->value = "0";
					initIdx->value = zero;
					m_prePendingStatements.push_back(std::move(initIdx));

					// while (__i < n)
					auto loop = std::make_shared<awst::WhileLoop>();
					loop->sourceLocation = loc;
					auto cond = std::make_shared<awst::NumericComparisonExpression>();
					cond->sourceLocation = loc;
					cond->wtype = awst::WType::boolType();
					cond->lhs = idxVar;
					cond->op = awst::NumericComparison::Lt;
					cond->rhs = sizeExpr;
					loop->condition = cond;

					// Body: __arr.extend(default); __i++
					auto loopBody = std::make_shared<awst::Block>();
					loopBody->sourceLocation = loc;

					// extend with single default element
					auto defaultElem = StorageMapper::makeDefaultValue(refArr->elementType(), loc);
					auto singleArr = std::make_shared<awst::NewArray>();
					singleArr->sourceLocation = loc;
					singleArr->wtype = resultType;
					singleArr->values.push_back(std::move(defaultElem));

					auto extend = std::make_shared<awst::ArrayExtend>();
					extend->sourceLocation = loc;
					extend->wtype = awst::WType::voidType();
					extend->base = arrVar;
					extend->other = std::move(singleArr);
					auto extendStmt = std::make_shared<awst::ExpressionStatement>();
					extendStmt->sourceLocation = loc;
					extendStmt->expr = extend;
					loopBody->body.push_back(std::move(extendStmt));

					// __i++
					auto one = std::make_shared<awst::IntegerConstant>();
					one->sourceLocation = loc;
					one->wtype = awst::WType::uint64Type();
					one->value = "1";
					auto incr = std::make_shared<awst::UInt64BinaryOperation>();
					incr->sourceLocation = loc;
					incr->wtype = awst::WType::uint64Type();
					incr->left = idxVar;
					incr->op = awst::UInt64BinaryOperator::Add;
					incr->right = one;
					auto incrAssign = std::make_shared<awst::AssignmentStatement>();
					incrAssign->sourceLocation = loc;
					incrAssign->target = idxVar;
					incrAssign->value = incr;
					loopBody->body.push_back(std::move(incrAssign));

					loop->loopBody = loopBody;
					m_prePendingStatements.push_back(std::move(loop));

					// Push the array variable as the expression result
					push(arrVar);
					return false;
				}
			}

			push(e);
			return false;
		}
	}

	// Handle abi.encodePacked(...) and abi.encodeCall(...)
	if (auto const* memberAccess = dynamic_cast<MemberAccess const*>(&funcExpr))
	{
		auto const* baseType = memberAccess->expression().annotation().type;
		if (auto const* magicType = dynamic_cast<MagicType const*>(baseType))
		{
			std::string memberName = memberAccess->memberName();

			// abi.encodePacked(...) → chain of concat intrinsics
			if (memberName == "encodePacked" || memberName == "encode")
			{
				auto const& args = _node.arguments();
				if (args.empty())
				{
					auto e = std::make_shared<awst::BytesConstant>();
					e->sourceLocation = loc;
					e->wtype = awst::WType::bytesType();
					e->encoding = awst::BytesEncoding::Base16;
					e->value = {};
					push(e);
					return false;
				}

				bool isPacked = (memberName == "encodePacked");

				// Helper: convert expression to bytes for abi.encode/encodePacked
				// For encodePacked, respects the Solidity type's packed byte width
				auto toPackedBytes = [&](std::shared_ptr<awst::Expression> expr, solidity::frontend::Type const* solType) -> std::shared_ptr<awst::Expression> {
					// Determine packed byte width from Solidity type
					int packedWidth = 0; // 0 means dynamic/no truncation needed
					if (isPacked && solType)
					{
						auto cat = solType->category();
						if (cat == Type::Category::Integer)
						{
							auto const* intType = dynamic_cast<IntegerType const*>(solType);
							if (intType)
								packedWidth = static_cast<int>(intType->numBits() / 8);
						}
						else if (cat == Type::Category::FixedBytes)
						{
							auto const* fbType = dynamic_cast<FixedBytesType const*>(solType);
							if (fbType)
								packedWidth = static_cast<int>(fbType->numBytes());
						}
						else if (cat == Type::Category::Bool)
						{
							packedWidth = 1;
						}
					}

					std::shared_ptr<awst::Expression> bytesExpr;
					if (expr->wtype == awst::WType::bytesType())
						bytesExpr = std::move(expr);
					else if (expr->wtype == awst::WType::stringType()
						|| (expr->wtype && expr->wtype->kind() == awst::WTypeKind::Bytes))
					{
						auto cast = std::make_shared<awst::ReinterpretCast>();
						cast->sourceLocation = loc;
						cast->wtype = awst::WType::bytesType();
						cast->expr = std::move(expr);
						bytesExpr = std::move(cast);
					}
					else if (expr->wtype == awst::WType::uint64Type())
					{
						auto itob = std::make_shared<awst::IntrinsicCall>();
						itob->sourceLocation = loc;
						itob->wtype = awst::WType::bytesType();
						itob->opCode = "itob";
						itob->stackArgs.push_back(std::move(expr));
						bytesExpr = std::move(itob);
					}
					else if (expr->wtype == awst::WType::biguintType())
					{
						auto cast = std::make_shared<awst::ReinterpretCast>();
						cast->sourceLocation = loc;
						cast->wtype = awst::WType::bytesType();
						cast->expr = std::move(expr);
						bytesExpr = std::move(cast);
					}
					else if (expr->wtype == awst::WType::accountType())
					{
						auto cast = std::make_shared<awst::ReinterpretCast>();
						cast->sourceLocation = loc;
						cast->wtype = awst::WType::bytesType();
						cast->expr = std::move(expr);
						bytesExpr = std::move(cast);
					}
					else if (expr->wtype == awst::WType::boolType())
					{
						// bool → bytes: convert to uint64 (0 or 1) then itob
						auto boolToInt = std::make_shared<awst::IntrinsicCall>();
						boolToInt->sourceLocation = loc;
						boolToInt->wtype = awst::WType::uint64Type();
						boolToInt->opCode = "select";
						boolToInt->stackArgs.push_back(makeUint64("0", loc));
						boolToInt->stackArgs.push_back(makeUint64("1", loc));
						boolToInt->stackArgs.push_back(std::move(expr));

						auto itob = std::make_shared<awst::IntrinsicCall>();
						itob->sourceLocation = loc;
						itob->wtype = awst::WType::bytesType();
						itob->opCode = "itob";
						itob->stackArgs.push_back(std::move(boolToInt));
						bytesExpr = std::move(itob);
					}
					else
					{
						// Fallback: reinterpret as bytes
						auto cast = std::make_shared<awst::ReinterpretCast>();
						cast->sourceLocation = loc;
						cast->wtype = awst::WType::bytesType();
						cast->expr = std::move(expr);
						bytesExpr = std::move(cast);
					}

					// For encodePacked with fixed-width types, extract exact byte width
					// e.g. uint8 → itob produces 8 bytes, extract last 1 byte
					// e.g. uint256 → biguint bytes are variable, pad/extract to 32 bytes
					if (packedWidth > 0 && packedWidth != 8)
					{
						if (packedWidth <= 8)
						{
							// Small uint (uint8..uint64): itob produces 8 bytes, extract suffix
							auto extract = std::make_shared<awst::IntrinsicCall>();
							extract->sourceLocation = loc;
							extract->wtype = awst::WType::bytesType();
							extract->opCode = "extract";
							extract->immediates.push_back(8 - packedWidth);
							extract->immediates.push_back(packedWidth);
							extract->stackArgs.push_back(std::move(bytesExpr));
							bytesExpr = std::move(extract);
						}
						else
						{
							// Large types (uint128, uint256, bytesN where N>8):
							// Ensure exactly packedWidth bytes:
							// 1. concat(bzero(packedWidth), bytes) → guaranteed >= packedWidth
							// 2. extract last packedWidth bytes via len/extract3
							auto widthConst = std::make_shared<awst::IntegerConstant>();
							widthConst->sourceLocation = loc;
							widthConst->wtype = awst::WType::uint64Type();
							widthConst->value = std::to_string(packedWidth);

							auto pad = std::make_shared<awst::IntrinsicCall>();
							pad->sourceLocation = loc;
							pad->wtype = awst::WType::bytesType();
							pad->opCode = "bzero";
							pad->stackArgs.push_back(std::move(widthConst));

							// concat(zeros, bytes) to ensure at least packedWidth bytes
							auto cat = std::make_shared<awst::IntrinsicCall>();
							cat->sourceLocation = loc;
							cat->wtype = awst::WType::bytesType();
							cat->opCode = "concat";
							cat->stackArgs.push_back(std::move(pad));
							cat->stackArgs.push_back(std::move(bytesExpr));

							// len(concat_result)
							auto lenCall = std::make_shared<awst::IntrinsicCall>();
							lenCall->sourceLocation = loc;
							lenCall->wtype = awst::WType::uint64Type();
							lenCall->opCode = "len";
							lenCall->stackArgs.push_back(cat);

							// offset = len - packedWidth
							auto widthConst2 = std::make_shared<awst::IntegerConstant>();
							widthConst2->sourceLocation = loc;
							widthConst2->wtype = awst::WType::uint64Type();
							widthConst2->value = std::to_string(packedWidth);

							auto offset = std::make_shared<awst::IntrinsicCall>();
							offset->sourceLocation = loc;
							offset->wtype = awst::WType::uint64Type();
							offset->opCode = "-";
							offset->stackArgs.push_back(std::move(lenCall));
							offset->stackArgs.push_back(std::move(widthConst2));

							// extract3(concat_result, offset, packedWidth)
							auto widthConst3 = std::make_shared<awst::IntegerConstant>();
							widthConst3->sourceLocation = loc;
							widthConst3->wtype = awst::WType::uint64Type();
							widthConst3->value = std::to_string(packedWidth);

							auto extract = std::make_shared<awst::IntrinsicCall>();
							extract->sourceLocation = loc;
							extract->wtype = awst::WType::bytesType();
							extract->opCode = "extract3";
							extract->stackArgs.push_back(std::move(cat));
							extract->stackArgs.push_back(std::move(offset));
							extract->stackArgs.push_back(std::move(widthConst3));

							bytesExpr = std::move(extract);
						}
					}

					return bytesExpr;
				};

				// Helper: pack a single argument, expanding arrays element-by-element
				auto packArg = [&](size_t argIdx) -> std::shared_ptr<awst::Expression> {
					auto const* solType = args[argIdx]->annotation().type;

					// Check if the argument is an array type
					auto const* arrType = dynamic_cast<ArrayType const*>(solType);
					// Also check for UDVT arrays: type checker resolves UDVT to underlying
					if (!arrType && solType && solType->category() == Type::Category::UserDefinedValueType)
					{
						auto const* udvt = dynamic_cast<UserDefinedValueType const*>(solType);
						if (udvt)
							arrType = dynamic_cast<ArrayType const*>(&udvt->underlyingType());
					}

					if (arrType && !arrType->isByteArrayOrString())
					{
						auto arrayExpr = build(*args[argIdx]);
						auto const* elemSolType = arrType->baseType();

						// Static array: unroll element access
						if (!arrType->isDynamicallySized())
						{
							int len = static_cast<int>(arrType->length());
							std::shared_ptr<awst::Expression> packed;
							for (int j = 0; j < len; ++j)
							{
								auto idx = std::make_shared<awst::IntegerConstant>();
								idx->sourceLocation = loc;
								idx->wtype = awst::WType::uint64Type();
								idx->value = std::to_string(j);

								auto indexExpr = std::make_shared<awst::IndexExpression>();
								indexExpr->sourceLocation = loc;
								indexExpr->base = arrayExpr;
								indexExpr->index = std::move(idx);
								indexExpr->wtype = m_typeMapper.map(elemSolType);

								auto elemBytes = toPackedBytes(std::move(indexExpr), elemSolType);
								if (!packed)
									packed = std::move(elemBytes);
								else
								{
									auto cat = std::make_shared<awst::IntrinsicCall>();
									cat->sourceLocation = loc;
									cat->wtype = awst::WType::bytesType();
									cat->opCode = "concat";
									cat->stackArgs.push_back(std::move(packed));
									cat->stackArgs.push_back(std::move(elemBytes));
									packed = std::move(cat);
								}
							}
							return packed ? packed : toPackedBytes(build(*args[argIdx]), solType);
						}
						else
						{
							// Dynamic array: ARC4Encode produces concatenated element bytes.
							// ReferenceArray encoding uses length_header=False, so the result
							// is the raw concatenation of elements — exactly what encodePacked needs.
							auto encode = std::make_shared<awst::ARC4Encode>();
							encode->sourceLocation = loc;
							encode->wtype = awst::WType::bytesType();
							encode->value = std::move(arrayExpr);
							return std::shared_ptr<awst::Expression>(std::move(encode));
						}
					}

					return toPackedBytes(build(*args[argIdx]), solType);
				};

				auto result = packArg(0);
				for (size_t i = 1; i < args.size(); ++i)
				{
					auto arg = packArg(i);
					auto concat = std::make_shared<awst::IntrinsicCall>();
					concat->sourceLocation = loc;
					concat->wtype = awst::WType::bytesType();
					concat->opCode = "concat";
					concat->stackArgs.push_back(std::move(result));
					concat->stackArgs.push_back(std::move(arg));
					result = std::move(concat);
				}
				push(std::move(result));
				return false;
			}

			// Helper: encode a single expression as ARC4 bytes for calldata encoding
			auto encodeArgAsARC4Bytes = [&](std::shared_ptr<awst::Expression> argExpr) -> std::shared_ptr<awst::Expression> {
				if (argExpr->wtype == awst::WType::bytesType()
					|| argExpr->wtype->kind() == awst::WTypeKind::Bytes)
				{
					return argExpr;
				}
				else if (argExpr->wtype == awst::WType::uint64Type())
				{
					auto itob = std::make_shared<awst::IntrinsicCall>();
					itob->sourceLocation = loc;
					itob->wtype = awst::WType::bytesType();
					itob->opCode = "itob";
					itob->stackArgs.push_back(std::move(argExpr));
					return itob;
				}
				else if (argExpr->wtype == awst::WType::biguintType())
				{
					// biguint → 32 bytes, left-padded
					auto cast = std::make_shared<awst::ReinterpretCast>();
					cast->sourceLocation = loc;
					cast->wtype = awst::WType::bytesType();
					cast->expr = std::move(argExpr);

					auto zeros = std::make_shared<awst::IntrinsicCall>();
					zeros->sourceLocation = loc;
					zeros->wtype = awst::WType::bytesType();
					zeros->opCode = "bzero";
					zeros->stackArgs.push_back(makeUint64("32", loc));

					auto padded = std::make_shared<awst::IntrinsicCall>();
					padded->sourceLocation = loc;
					padded->wtype = awst::WType::bytesType();
					padded->opCode = "concat";
					padded->stackArgs.push_back(std::move(zeros));
					padded->stackArgs.push_back(std::move(cast));

					auto lenCall = std::make_shared<awst::IntrinsicCall>();
					lenCall->sourceLocation = loc;
					lenCall->wtype = awst::WType::uint64Type();
					lenCall->opCode = "len";
					lenCall->stackArgs.push_back(padded);

					auto offset = std::make_shared<awst::IntrinsicCall>();
					offset->sourceLocation = loc;
					offset->wtype = awst::WType::uint64Type();
					offset->opCode = "-";
					offset->stackArgs.push_back(std::move(lenCall));
					offset->stackArgs.push_back(makeUint64("32", loc));

					auto extracted = std::make_shared<awst::IntrinsicCall>();
					extracted->sourceLocation = loc;
					extracted->wtype = awst::WType::bytesType();
					extracted->opCode = "extract3";
					extracted->stackArgs.push_back(std::move(padded));
					extracted->stackArgs.push_back(std::move(offset));
					extracted->stackArgs.push_back(makeUint64("32", loc));
					return extracted;
				}
				else if (argExpr->wtype == awst::WType::boolType())
				{
					auto zeroByte = std::make_shared<awst::BytesConstant>();
					zeroByte->sourceLocation = loc;
					zeroByte->wtype = awst::WType::bytesType();
					zeroByte->encoding = awst::BytesEncoding::Base16;
					zeroByte->value = {0x00};

					auto setbit = std::make_shared<awst::IntrinsicCall>();
					setbit->sourceLocation = loc;
					setbit->wtype = awst::WType::bytesType();
					setbit->opCode = "setbit";
					setbit->stackArgs.push_back(std::move(zeroByte));
					setbit->stackArgs.push_back(makeUint64("0", loc));
					setbit->stackArgs.push_back(std::move(argExpr));
					return setbit;
				}
				else if (argExpr->wtype == awst::WType::accountType())
				{
					auto cast = std::make_shared<awst::ReinterpretCast>();
					cast->sourceLocation = loc;
					cast->wtype = awst::WType::bytesType();
					cast->expr = std::move(argExpr);
					return cast;
				}
				else
				{
					auto cast = std::make_shared<awst::ReinterpretCast>();
					cast->sourceLocation = loc;
					cast->wtype = awst::WType::bytesType();
					cast->expr = std::move(argExpr);
					return cast;
				}
			};

			// Helper: build ARC4 method selector string from a FunctionDefinition
			auto buildARC4MethodSelector = [this](solidity::frontend::FunctionDefinition const* funcDef) -> std::string {
				auto solTypeToARC4 = [this](solidity::frontend::Type const* _type) -> std::string {
					auto* wtype = m_typeMapper.map(_type);
					if (wtype == awst::WType::biguintType()) return "uint256";
					if (wtype == awst::WType::uint64Type()) return "uint64";
					if (wtype == awst::WType::boolType()) return "bool";
					if (wtype == awst::WType::accountType()) return "address";
					if (wtype == awst::WType::bytesType()) return "byte[]";
					if (wtype == awst::WType::stringType()) return "string";
					if (wtype->kind() == awst::WTypeKind::Bytes)
					{
						auto const* bw = static_cast<awst::BytesWType const*>(wtype);
						if (bw->length().has_value())
							return "byte[" + std::to_string(bw->length().value()) + "]";
						return "byte[]";
					}
					if (auto const* structType = dynamic_cast<solidity::frontend::StructType const*>(_type))
						return "struct " + structType->structDefinition().name();
					return _type->toString(true);
				};

				std::string selector = funcDef->name() + "(";
				bool first = true;
				for (auto const& param: funcDef->parameters())
				{
					if (!first) selector += ",";
					selector += solTypeToARC4(param->type());
					first = false;
				}
				selector += ")";
				if (funcDef->returnParameters().size() > 1)
				{
					selector += "(";
					bool firstRet = true;
					for (auto const& retParam: funcDef->returnParameters())
					{
						if (!firstRet) selector += ",";
						selector += solTypeToARC4(retParam->type());
						firstRet = false;
					}
					selector += ")";
				}
				else if (funcDef->returnParameters().size() == 1)
					selector += solTypeToARC4(funcDef->returnParameters()[0]->type());
				else
					selector += "void";
				return selector;
			};

			// Helper: concatenate a list of byte expressions using concat intrinsics
			auto concatByteExprs = [&](std::vector<std::shared_ptr<awst::Expression>> parts) -> std::shared_ptr<awst::Expression> {
				if (parts.empty())
				{
					auto e = std::make_shared<awst::BytesConstant>();
					e->sourceLocation = loc;
					e->wtype = awst::WType::bytesType();
					e->encoding = awst::BytesEncoding::Base16;
					e->value = {};
					return e;
				}
				auto result = std::move(parts[0]);
				for (size_t i = 1; i < parts.size(); ++i)
				{
					auto concat = std::make_shared<awst::IntrinsicCall>();
					concat->sourceLocation = loc;
					concat->wtype = awst::WType::bytesType();
					concat->opCode = "concat";
					concat->stackArgs.push_back(std::move(result));
					concat->stackArgs.push_back(std::move(parts[i]));
					result = std::move(concat);
				}
				return result;
			};

			// abi.encodeCall(fn, (args)) → method_selector || ARC4_encoded_args
			if (memberName == "encodeCall")
			{
				if (_node.arguments().size() >= 2)
				{
					auto const& targetFnExpr = *_node.arguments()[0];
					solidity::frontend::FunctionDefinition const* targetFuncDef = nullptr;
					if (auto const* fnType = dynamic_cast<solidity::frontend::FunctionType const*>(
								targetFnExpr.annotation().type))
					{
						if (fnType->hasDeclaration())
							targetFuncDef = dynamic_cast<solidity::frontend::FunctionDefinition const*>(
								&fnType->declaration());
					}

					if (targetFuncDef)
					{
						std::string methodSig = buildARC4MethodSelector(targetFuncDef);
						auto methodConst = std::make_shared<awst::MethodConstant>();
						methodConst->sourceLocation = loc;
						methodConst->wtype = awst::WType::bytesType();
						methodConst->value = methodSig;

						// Extract arguments from second arg (tuple)
						std::vector<std::shared_ptr<awst::Expression>> parts;
						parts.push_back(std::move(methodConst));

						auto const& argsExpr = *_node.arguments()[1];
						std::vector<solidity::frontend::ASTPointer<solidity::frontend::Expression const>> callArgs;
						if (auto const* tupleExpr = dynamic_cast<solidity::frontend::TupleExpression const*>(&argsExpr))
						{
							for (auto const& comp: tupleExpr->components())
								if (comp) callArgs.push_back(comp);
						}
						else
							callArgs.push_back(_node.arguments()[1]);

						for (auto const& arg: callArgs)
							parts.push_back(encodeArgAsARC4Bytes(build(*arg)));

						push(concatByteExprs(std::move(parts)));
						return false;
					}
				}
				// Fallback: empty bytes
				auto e = std::make_shared<awst::BytesConstant>();
				e->sourceLocation = loc;
				e->wtype = awst::WType::bytesType();
				e->encoding = awst::BytesEncoding::Base16;
				e->value = {};
				push(e);
				return false;
			}

			// abi.encodeWithSelector(bytes4, args...) → selector || ARC4_encoded_args
			if (memberName == "encodeWithSelector")
			{
				auto const& args = _node.arguments();
				if (!args.empty())
				{
					std::vector<std::shared_ptr<awst::Expression>> parts;
					// First arg is the bytes4 selector
					parts.push_back(build(*args[0]));
					// Remaining args are encoded as ARC4
					for (size_t i = 1; i < args.size(); ++i)
						parts.push_back(encodeArgAsARC4Bytes(build(*args[i])));
					push(concatByteExprs(std::move(parts)));
					return false;
				}
				auto e = std::make_shared<awst::BytesConstant>();
				e->sourceLocation = loc;
				e->wtype = awst::WType::bytesType();
				e->encoding = awst::BytesEncoding::Base16;
				e->value = {};
				push(e);
				return false;
			}

			// abi.encodeWithSignature(string, args...) → sha256(sig)[0:4] || ARC4_encoded_args
			if (memberName == "encodeWithSignature")
			{
				auto const& args = _node.arguments();
				if (!args.empty())
				{
					std::vector<std::shared_ptr<awst::Expression>> parts;
					// First arg is the string signature — use MethodConstant to compute 4-byte selector
					// Extract the string literal value
					auto sigExpr = build(*args[0]);
					if (auto const* strConst = dynamic_cast<awst::BytesConstant const*>(sigExpr.get()))
					{
						auto methodConst = std::make_shared<awst::MethodConstant>();
						methodConst->sourceLocation = loc;
						methodConst->wtype = awst::WType::bytesType();
						methodConst->value = std::string(strConst->value.begin(), strConst->value.end());
						parts.push_back(std::move(methodConst));
					}
					else
					{
						// Dynamic signature: hash at runtime — sha256(sig), extract first 4 bytes
						auto hash = std::make_shared<awst::IntrinsicCall>();
						hash->sourceLocation = loc;
						hash->wtype = awst::WType::bytesType();
						hash->opCode = "sha256";
						hash->stackArgs.push_back(std::move(sigExpr));

						auto extract4 = std::make_shared<awst::IntrinsicCall>();
						extract4->sourceLocation = loc;
						extract4->wtype = awst::WType::bytesType();
						extract4->opCode = "extract";
						extract4->immediates = {0, 4};
						extract4->stackArgs.push_back(std::move(hash));
						parts.push_back(std::move(extract4));
					}

					for (size_t i = 1; i < args.size(); ++i)
						parts.push_back(encodeArgAsARC4Bytes(build(*args[i])));
					push(concatByteExprs(std::move(parts)));
					return false;
				}
				auto e = std::make_shared<awst::BytesConstant>();
				e->sourceLocation = loc;
				e->wtype = awst::WType::bytesType();
				e->encoding = awst::BytesEncoding::Base16;
				e->value = {};
				push(e);
				return false;
			}

			// abi.decode — pass through first argument, cast to target type
			if (memberName == "decode")
			{
				auto* targetType = m_typeMapper.map(_node.annotation().type);
				if (!_node.arguments().empty())
				{
					auto decoded = build(*_node.arguments()[0]);
					if (targetType && decoded->wtype != targetType)
					{
						// For scalar types, use appropriate cast
						if (targetType == awst::WType::boolType())
						{
							// bytes → bool: check if any byte is non-zero
							// ABI-encoded bool is 32 bytes, last byte is 0 or 1
							// Use btoi on the last byte, then compare != 0
							auto bytesExpr = std::move(decoded);
							if (bytesExpr->wtype != awst::WType::bytesType())
							{
								auto toBytes = std::make_shared<awst::ReinterpretCast>();
								toBytes->sourceLocation = loc;
								toBytes->wtype = awst::WType::bytesType();
								toBytes->expr = std::move(bytesExpr);
								bytesExpr = std::move(toBytes);
							}
							// btoi(bytes) — will interpret as big-endian integer
							auto btoi = std::make_shared<awst::IntrinsicCall>();
							btoi->sourceLocation = loc;
							btoi->wtype = awst::WType::uint64Type();
							btoi->opCode = "btoi";
							btoi->stackArgs.push_back(std::move(bytesExpr));

							auto zero = std::make_shared<awst::IntegerConstant>();
							zero->sourceLocation = loc;
							zero->wtype = awst::WType::uint64Type();
							zero->value = "0";

							auto cmp = std::make_shared<awst::NumericComparisonExpression>();
							cmp->sourceLocation = loc;
							cmp->wtype = awst::WType::boolType();
							cmp->lhs = std::move(btoi);
							cmp->rhs = std::move(zero);
							cmp->op = awst::NumericComparison::Ne;
							push(cmp);
						}
						else if (targetType == awst::WType::uint64Type())
						{
							// bytes → uint64: use btoi
							auto bytesExpr = std::move(decoded);
							if (bytesExpr->wtype != awst::WType::bytesType())
							{
								auto toBytes = std::make_shared<awst::ReinterpretCast>();
								toBytes->sourceLocation = loc;
								toBytes->wtype = awst::WType::bytesType();
								toBytes->expr = std::move(bytesExpr);
								bytesExpr = std::move(toBytes);
							}
							auto btoi = std::make_shared<awst::IntrinsicCall>();
							btoi->sourceLocation = loc;
							btoi->wtype = awst::WType::uint64Type();
							btoi->opCode = "btoi";
							btoi->stackArgs.push_back(std::move(bytesExpr));
							push(btoi);
						}
						else if (auto const* refArr = dynamic_cast<awst::ReferenceArray const*>(targetType))
						{
							// abi.decode bytes into fixed-size array by extracting at element boundaries
							auto arrSize = refArr->arraySize();
							if (!arrSize)
							{
								Logger::instance().warning(
									"abi.decode to dynamic array not supported; using empty array", loc
								);
								auto emptyArr = std::make_shared<awst::NewArray>();
								emptyArr->sourceLocation = loc;
								emptyArr->wtype = targetType;
								push(std::move(emptyArr));
							}
							else
							{
								auto* elemType = refArr->elementType();
								int elemSize = 32; // ABI default
								if (elemType == awst::WType::uint64Type())
									elemSize = 8;

								// Ensure source is bytes
								auto bytesSource = std::move(decoded);
								if (bytesSource->wtype != awst::WType::bytesType())
								{
									auto toBytes = std::make_shared<awst::ReinterpretCast>();
									toBytes->sourceLocation = loc;
									toBytes->wtype = awst::WType::bytesType();
									toBytes->expr = std::move(bytesSource);
									bytesSource = std::move(toBytes);
								}

								auto arr = std::make_shared<awst::NewArray>();
								arr->sourceLocation = loc;
								arr->wtype = targetType;

								for (int i = 0; i < *arrSize; ++i)
								{
									// extract3(source, offset, elemSize)
									auto extract = std::make_shared<awst::IntrinsicCall>();
									extract->sourceLocation = loc;
									extract->wtype = awst::WType::bytesType();
									extract->opCode = "extract3";
									extract->stackArgs.push_back(bytesSource);
									auto offExpr = std::make_shared<awst::IntegerConstant>();
									offExpr->sourceLocation = loc;
									offExpr->wtype = awst::WType::uint64Type();
									offExpr->value = std::to_string(i * elemSize);
									extract->stackArgs.push_back(std::move(offExpr));
									auto lenExpr = std::make_shared<awst::IntegerConstant>();
									lenExpr->sourceLocation = loc;
									lenExpr->wtype = awst::WType::uint64Type();
									lenExpr->value = std::to_string(elemSize);
									extract->stackArgs.push_back(std::move(lenExpr));

									// Cast to element type
									if (elemType == awst::WType::biguintType())
									{
										auto cast = std::make_shared<awst::ReinterpretCast>();
										cast->sourceLocation = loc;
										cast->wtype = elemType;
										cast->expr = std::move(extract);
										arr->values.push_back(std::move(cast));
									}
									else if (elemType == awst::WType::uint64Type())
									{
										auto btoi = std::make_shared<awst::IntrinsicCall>();
										btoi->sourceLocation = loc;
										btoi->wtype = awst::WType::uint64Type();
										btoi->opCode = "btoi";
										btoi->stackArgs.push_back(std::move(extract));
										arr->values.push_back(std::move(btoi));
									}
									else
									{
										// bytes or other types: use as-is or reinterpret
										if (elemType != awst::WType::bytesType())
										{
											auto cast = std::make_shared<awst::ReinterpretCast>();
											cast->sourceLocation = loc;
											cast->wtype = elemType;
											cast->expr = std::move(extract);
											arr->values.push_back(std::move(cast));
										}
										else
											arr->values.push_back(std::move(extract));
									}
								}
								push(std::move(arr));
							}
						}
						else if (targetType->kind() != awst::WTypeKind::WTuple)
						{
							auto cast = std::make_shared<awst::ReinterpretCast>();
							cast->sourceLocation = loc;
							cast->wtype = targetType;
							cast->expr = std::move(decoded);
							push(cast);
						}
						else if (auto const* tupleType = dynamic_cast<awst::WTuple const*>(targetType))
						{
							// ABI decode bytes into tuple fields by extracting at 32-byte boundaries
							// Each ABI-encoded field is 32 bytes (uint256, address, bytes32, etc.)
							auto tuple = std::make_shared<awst::TupleExpression>();
							tuple->sourceLocation = loc;
							tuple->wtype = targetType;

							// Ensure source is bytes for extraction
							auto bytesSource = std::move(decoded);
							if (bytesSource->wtype != awst::WType::bytesType())
							{
								auto toBytes = std::make_shared<awst::ReinterpretCast>();
								toBytes->sourceLocation = loc;
								toBytes->wtype = awst::WType::bytesType();
								toBytes->expr = std::move(bytesSource);
								bytesSource = std::move(toBytes);
							}

							int offset = 0;
							for (auto const* fieldType: tupleType->types())
							{
								int fieldSize = 32; // ABI default: 32 bytes per field
								if (fieldType == awst::WType::boolType())
									fieldSize = 32;
								else if (fieldType == awst::WType::biguintType())
									fieldSize = 32;
								else if (fieldType && fieldType->kind() == awst::WTypeKind::Bytes)
								{
									auto const* bytesT = dynamic_cast<awst::BytesWType const*>(fieldType);
									if (bytesT && bytesT->length())
										fieldSize = static_cast<int>(*bytesT->length());
								}

								// extract3(source, offset, fieldSize) → bytes
								auto extract = std::make_shared<awst::IntrinsicCall>();
								extract->sourceLocation = loc;
								extract->wtype = awst::WType::bytesType();
								extract->opCode = "extract3";
								extract->stackArgs.push_back(bytesSource); // shared ptr copied
								auto offExpr = std::make_shared<awst::IntegerConstant>();
								offExpr->sourceLocation = loc;
								offExpr->wtype = awst::WType::uint64Type();
								offExpr->value = std::to_string(offset);
								extract->stackArgs.push_back(std::move(offExpr));
								auto lenExpr = std::make_shared<awst::IntegerConstant>();
								lenExpr->sourceLocation = loc;
								lenExpr->wtype = awst::WType::uint64Type();
								lenExpr->value = std::to_string(fieldSize);
								extract->stackArgs.push_back(std::move(lenExpr));

								// Cast extracted bytes to the target field type
								if (fieldType == awst::WType::biguintType())
								{
									auto cast = std::make_shared<awst::ReinterpretCast>();
									cast->sourceLocation = loc;
									cast->wtype = fieldType;
									cast->expr = std::move(extract);
									tuple->items.push_back(std::move(cast));
								}
								else if (fieldType == awst::WType::boolType())
								{
									// btoi(extracted) != 0
									auto btoi = std::make_shared<awst::IntrinsicCall>();
									btoi->sourceLocation = loc;
									btoi->wtype = awst::WType::uint64Type();
									btoi->opCode = "btoi";
									btoi->stackArgs.push_back(std::move(extract));
									auto zero = std::make_shared<awst::IntegerConstant>();
									zero->sourceLocation = loc;
									zero->wtype = awst::WType::uint64Type();
									zero->value = "0";
									auto cmp = std::make_shared<awst::NumericComparisonExpression>();
									cmp->sourceLocation = loc;
									cmp->wtype = awst::WType::boolType();
									cmp->lhs = std::move(btoi);
									cmp->rhs = std::move(zero);
									cmp->op = awst::NumericComparison::Ne;
									tuple->items.push_back(std::move(cmp));
								}
								else
								{
									auto cast = std::make_shared<awst::ReinterpretCast>();
									cast->sourceLocation = loc;
									cast->wtype = fieldType;
									cast->expr = std::move(extract);
									tuple->items.push_back(std::move(cast));
								}
								offset += fieldSize;
							}
							push(tuple);
						}
					}
					else
						push(decoded);
				}
				else
				{
					auto e = std::make_shared<awst::BytesConstant>();
					e->sourceLocation = loc;
					e->wtype = awst::WType::bytesType();
					e->encoding = awst::BytesEncoding::Base16;
					e->value = {};
					push(e);
				}
				return false;
			}
		}

		// bytes.concat(a, b, ...) → chain of concat intrinsics
		// TypeType wrapping BytesType with concat member
		if (auto const* typeType = dynamic_cast<solidity::frontend::TypeType const*>(baseType))
		{
			auto const* actualType = typeType->actualType();
			if (actualType && actualType->category() == solidity::frontend::Type::Category::Array)
			{
				auto const* arrType = dynamic_cast<solidity::frontend::ArrayType const*>(actualType);
				if (arrType && arrType->isByteArrayOrString()
					&& memberAccess->memberName() == "concat")
				{
					auto const& args = _node.arguments();
					if (args.empty())
					{
						auto e = std::make_shared<awst::BytesConstant>();
						e->sourceLocation = loc;
						e->wtype = awst::WType::bytesType();
						e->encoding = awst::BytesEncoding::Base16;
						e->value = {};
						push(e);
						return false;
					}

					// Helper: convert expression to bytes
					auto toBytes = [&](std::shared_ptr<awst::Expression> expr) -> std::shared_ptr<awst::Expression> {
						if (expr->wtype == awst::WType::bytesType()
							|| (expr->wtype && expr->wtype->kind() == awst::WTypeKind::Bytes))
							return expr;
						auto cast = std::make_shared<awst::ReinterpretCast>();
						cast->sourceLocation = loc;
						cast->wtype = awst::WType::bytesType();
						cast->expr = std::move(expr);
						return cast;
					};

					auto result = toBytes(build(*args[0]));
					for (size_t i = 1; i < args.size(); ++i)
					{
						auto arg = toBytes(build(*args[i]));
						auto concat = std::make_shared<awst::IntrinsicCall>();
						concat->sourceLocation = loc;
						concat->wtype = awst::WType::bytesType();
						concat->opCode = "concat";
						concat->stackArgs.push_back(std::move(result));
						concat->stackArgs.push_back(std::move(arg));
						result = std::move(concat);
					}
					push(std::move(result));
					return false;
				}
			}
		}
	}

	// Generic function call → SubroutineCallExpression
	auto call = std::make_shared<awst::SubroutineCallExpression>();
	call->sourceLocation = loc;
	bool isUsingDirectiveCall = false;
	FunctionDefinition const* resolvedFuncDef = nullptr;

	if (auto const* identifier = dynamic_cast<Identifier const*>(&funcExpr))
	{
		std::string name = identifier->name();
		auto const* decl = identifier->annotation().referencedDeclaration;
		// Check if this is a function pointer variable call: `ptr()` where ptr was assigned a function
		if (auto const* varDecl = dynamic_cast<VariableDeclaration const*>(decl))
		{
			if (auto const* targetFunc = getFuncPtrTarget(varDecl->id()))
			{
				// Resolve the function pointer to the actual function
				decl = targetFunc;
				Logger::instance().debug(
					"resolved function pointer '" + name + "' to '" + targetFunc->name() + "'"
				);
			}
			else if (dynamic_cast<solidity::frontend::FunctionType const*>(varDecl->type()))
			{
				// Unresolvable function pointer call (e.g., uninitialized or state-stored).
				// Emit assert(false) — calling an uninitialized function pointer is undefined
				// behavior in Solidity and always reverts on EVM.
				Logger::instance().warning(
					"call to unresolvable function pointer '" + name + "', emitting assert(false)", loc
				);
				auto assertExpr = std::make_shared<awst::AssertExpression>();
				assertExpr->sourceLocation = loc;
				assertExpr->wtype = awst::WType::voidType();
				auto falseLit = std::make_shared<awst::BoolConstant>();
				falseLit->sourceLocation = loc;
				falseLit->wtype = awst::WType::boolType();
				falseLit->value = false;
				assertExpr->condition = falseLit;
				assertExpr->errorMessage = "uninitialized function pointer";
				m_pendingStatements.push_back([&](){
					auto stmt = std::make_shared<awst::ExpressionStatement>();
					stmt->sourceLocation = loc;
					stmt->expr = assertExpr;
					return stmt;
				}());
				// Push a void result
				auto vc = std::make_shared<awst::VoidConstant>();
				vc->sourceLocation = loc;
				vc->wtype = awst::WType::voidType();
				push(std::move(vc));
				return false;
			}
		}
		if (auto const* funcDef = dynamic_cast<FunctionDefinition const*>(decl))
		{
			resolvedFuncDef = funcDef;
			if (funcDef->returnParameters().empty())
				call->wtype = awst::WType::voidType();
			else if (funcDef->returnParameters().size() == 1)
				call->wtype = m_typeMapper.map(funcDef->returnParameters()[0]->type());
			else
			{
				// Multi-value return → WTuple
				std::vector<awst::WType const*> retTypes;
				for (auto const& param: funcDef->returnParameters())
					retTypes.push_back(m_typeMapper.map(param->type()));
				call->wtype = m_typeMapper.createType<awst::WTuple>(
					std::move(retTypes), std::nullopt
				);
			}

			// Check if this function belongs to a library (e.g. calling within a library)
			bool resolvedAsLibrary = false;
			if (auto const* scope = funcDef->scope())
			{
				if (auto const* contractDef = dynamic_cast<ContractDefinition const*>(scope))
				{
					if (contractDef->isLibrary())
					{
						// Prefer AST ID lookup for precise overload resolution
						auto byId = m_freeFunctionById.find(funcDef->id());
						if (byId != m_freeFunctionById.end())
						{
							call->target = awst::SubroutineID{byId->second};
							resolvedAsLibrary = true;
						}
						else
						{
							std::string key = contractDef->name() + "." + funcDef->name();
							auto it = m_libraryFunctionIds.find(key);
							if (it == m_libraryFunctionIds.end())
							{
								key += "(" + std::to_string(funcDef->parameters().size()) + ")";
								it = m_libraryFunctionIds.find(key);
							}
							if (it != m_libraryFunctionIds.end())
							{
								call->target = awst::SubroutineID{it->second};
								resolvedAsLibrary = true;
							}
						}
					}
				}
			}
			if (!resolvedAsLibrary && funcDef->isFree())
			{
				auto it = m_freeFunctionById.find(funcDef->id());
				if (it != m_freeFunctionById.end())
				{
					call->target = awst::SubroutineID{it->second};
					resolvedAsLibrary = true;
				}
			}
			if (!resolvedAsLibrary)
			{
				Logger::instance().debug("library resolution failed for '" + name + "', falling back to InstanceMethodTarget");
				call->target = awst::InstanceMethodTarget{resolveMethodName(*funcDef)};
			}
		}
		else
		{
			call->wtype = m_typeMapper.map(_node.annotation().type);
			call->target = awst::InstanceMethodTarget{name};
		}
	}
	else if (auto const* memberAccess = dynamic_cast<MemberAccess const*>(&funcExpr))
	{
		call->wtype = m_typeMapper.map(_node.annotation().type);

		// Check if the base expression references a library contract
		bool resolvedAsLibrary = false;
		auto const& baseExpr = memberAccess->expression();
		if (auto const* baseId = dynamic_cast<Identifier const*>(&baseExpr))
		{
			auto const* decl = baseId->annotation().referencedDeclaration;
			if (auto const* contractDef = dynamic_cast<ContractDefinition const*>(decl))
			{
				if (contractDef->isLibrary())
				{
					// Set resolvedFuncDef for argument type coercion
					auto const* refDecl = memberAccess->annotation().referencedDeclaration;
					if (auto const* fd = dynamic_cast<FunctionDefinition const*>(refDecl))
						resolvedFuncDef = fd;

					// Prefer AST ID lookup for precise overload resolution
					if (resolvedFuncDef)
					{
						auto byId = m_freeFunctionById.find(resolvedFuncDef->id());
						if (byId != m_freeFunctionById.end())
						{
							call->target = awst::SubroutineID{byId->second};
							resolvedAsLibrary = true;
						}
					}
					if (!resolvedAsLibrary)
					{
						std::string key = contractDef->name() + "." + memberAccess->memberName();
						auto it = m_libraryFunctionIds.find(key);
						if (it == m_libraryFunctionIds.end())
						{
							size_t paramCount = _node.arguments().size();
							if (resolvedFuncDef)
								paramCount = resolvedFuncDef->parameters().size();
							key += "(" + std::to_string(paramCount) + ")";
							it = m_libraryFunctionIds.find(key);
						}
						if (it != m_libraryFunctionIds.end())
						{
							call->target = awst::SubroutineID{it->second};
							resolvedAsLibrary = true;
						}
					}
				}
			}
		}
		// Also check if the member's referenced declaration is a library function
		// (handles `using Library for Type` and `using {func} for Type` patterns)
		if (!resolvedAsLibrary)
		{
			auto const* refDecl = memberAccess->annotation().referencedDeclaration;
			if (auto const* funcDef = dynamic_cast<FunctionDefinition const*>(refDecl))
			{
				resolvedFuncDef = funcDef;
				std::string dbgScope = "(no scope)";
				if (auto const* scope = funcDef->scope())
				{
					if (auto const* contractDef = dynamic_cast<ContractDefinition const*>(scope))
					{
						dbgScope = contractDef->name() + (contractDef->isLibrary() ? " [lib]" : "");
						if (contractDef->isLibrary())
						{
							// Prefer AST ID lookup for precise overload resolution
							auto byId = m_freeFunctionById.find(funcDef->id());
							Logger::instance().debug("[RES] lib using-for: " + contractDef->name() + "." + funcDef->name() + " id=" + std::to_string(funcDef->id()) + " mapSize=" + std::to_string(m_freeFunctionById.size()) + " found=" + (byId != m_freeFunctionById.end() ? "YES" : "NO"));
							if (byId != m_freeFunctionById.end())
							{
								call->target = awst::SubroutineID{byId->second};
								resolvedAsLibrary = true;
								isUsingDirectiveCall = true;
							}
							else
							{
								std::string key = contractDef->name() + "." + funcDef->name();
								auto it = m_libraryFunctionIds.find(key);
								if (it == m_libraryFunctionIds.end())
								{
									key += "(" + std::to_string(funcDef->parameters().size()) + ")";
									it = m_libraryFunctionIds.find(key);
								}
								Logger::instance().debug("[RES] lib name fallback key='" + key + "' found=" + (it != m_libraryFunctionIds.end() ? "YES" : "NO"));
								if (it != m_libraryFunctionIds.end())
								{
									call->target = awst::SubroutineID{it->second};
									resolvedAsLibrary = true;
									isUsingDirectiveCall = true;
								}
							}
						}
					}
				}
				// Check if it's a free function bound via `using {func} for Type`
				if (!resolvedAsLibrary && funcDef->isFree())
				{
					auto it = m_freeFunctionById.find(funcDef->id());
					Logger::instance().debug("[RES] free using-for: " + funcDef->name() + " id=" + std::to_string(funcDef->id()) + " found=" + (it != m_freeFunctionById.end() ? "YES" : "NO"));
					if (it != m_freeFunctionById.end())
					{
						call->target = awst::SubroutineID{it->second};
						resolvedAsLibrary = true;
						// Only prepend receiver for actual using-for calls (x.method()),
						// not for module-qualified calls (Module.func()).
						// Module imports have a Module type on the base expression.
						auto const* baseType = memberAccess->expression().annotation().type;
						bool isModuleCall = baseType
							&& baseType->category() == solidity::frontend::Type::Category::Module;
						if (!isModuleCall)
							isUsingDirectiveCall = true;
					}
				}
				if (!resolvedAsLibrary)
				{
					Logger::instance().debug("[RES] FAILED for " + funcDef->name() + " id=" + std::to_string(funcDef->id()) + " scope=" + dbgScope + " isFree=" + (funcDef->isFree() ? "Y" : "N"));
				}
			}
		}
		if (!resolvedAsLibrary)
		{
			// Check if this is a super.method() call or a base-contract internal call
			auto const* baseType = memberAccess->expression().annotation().type;
			// Track whether the original expression was a TypeType (contract used as type, not instance).
			// e.g., `BaseBase.g()` where BaseBase is the contract type, not a variable.
			bool wasTypeType = false;
			// Unwrap TypeType if needed (super has type TypeType(ContractType(isSuper=true)))
			if (baseType && baseType->category() == solidity::frontend::Type::Category::TypeType)
			{
				wasTypeType = true;
				auto const* typeType = dynamic_cast<solidity::frontend::TypeType const*>(baseType);
				if (typeType)
					baseType = typeType->actualType();
			}
			bool isSuperCall = false;
			bool isBaseInternalCall = false;
			if (baseType && baseType->category() == solidity::frontend::Type::Category::Contract)
			{
				auto const* contractType = dynamic_cast<solidity::frontend::ContractType const*>(baseType);
				if (contractType && contractType->isSuper())
				{
					isSuperCall = true;
					auto const* refDecl = memberAccess->annotation().referencedDeclaration;
					if (auto const* funcDef = dynamic_cast<solidity::frontend::FunctionDefinition const*>(refDecl))
					{
						resolvedFuncDef = funcDef;
						// Look up the super target name by AST ID
						auto it = m_superTargetNames.find(funcDef->id());
						if (it != m_superTargetNames.end())
						{
							call->target = awst::InstanceMethodTarget{it->second};
						}
						else
						{
							// Fallback: use resolved name (will be same as override — bug)
							Logger::instance().warning("super call target not registered for " + funcDef->name(), loc);
							call->target = awst::InstanceMethodTarget{resolveMethodName(*funcDef)};
						}
					}
				}
				// Check for explicit base contract internal call: `BaseContract.method()`
				// When a contract type is used as a type reference (not an instance variable),
				// and it's a base of the current contract, this is an internal call that
				// targets a specific base version of the method.
				else if (wasTypeType && contractType)
				{
					auto const* refDecl = memberAccess->annotation().referencedDeclaration;
					if (auto const* funcDef = dynamic_cast<solidity::frontend::FunctionDefinition const*>(refDecl))
					{
						resolvedFuncDef = funcDef;
						isBaseInternalCall = true;
						// Set return type from the resolved function
						if (funcDef->returnParameters().empty())
							call->wtype = awst::WType::voidType();
						else if (funcDef->returnParameters().size() == 1)
							call->wtype = m_typeMapper.map(funcDef->returnParameters()[0]->type());
						else
						{
							std::vector<awst::WType const*> retTypes;
							for (auto const& param: funcDef->returnParameters())
								retTypes.push_back(m_typeMapper.map(param->type()));
							call->wtype = m_typeMapper.createType<awst::WTuple>(
								std::move(retTypes), std::nullopt);
						}
						// Use the method name as resolved for the current contract.
						// The method may have been overridden, so use resolveMethodName
						// which produces the name as it appears in the derived contract.
						call->target = awst::InstanceMethodTarget{resolveMethodName(*funcDef)};
					}
				}
			}
			// Check if base is an interface/contract type (external call).
			// Both interfaces and calls to other concrete contracts should route
			// through inner app calls. Only calls to the current contract (self/this),
			// super calls, or base internal calls remain as InstanceMethodTarget.
			bool isExternalCall = false;
			if (!isSuperCall && !isBaseInternalCall && baseType && baseType->category() == solidity::frontend::Type::Category::Contract)
			{
				auto const* contractType = dynamic_cast<solidity::frontend::ContractType const*>(baseType);
				if (contractType)
				{
					if (contractType->contractDefinition().isInterface())
						isExternalCall = true;
					else
					{
						// Concrete contract type — external call if it's a different contract.
						// Check by name since we don't have the AST ContractDefinition here.
						auto const& calledName = contractType->contractDefinition().name();
						if (calledName != m_contractName)
							isExternalCall = true;
					}
				}
			}
			if (isExternalCall)
			{
				// External interface call → application call inner transaction
				auto baseTranslated = build(memberAccess->expression());

				// Build ARC4 method selector string.
				// Must use ARC4 type names (matching puya output) not Solidity names.
				// Use the WType→ARC4 mapping to generate names identical to the
				// callee's routing selector (which goes through the puya backend).
				std::function<std::string(awst::WType const*)> wtypeToABIName;
				wtypeToABIName = [&wtypeToABIName](awst::WType const* _wtype) -> std::string {
					if (_wtype == awst::WType::arc4BoolType())
						return "bool";
					switch (_wtype->kind())
					{
					case awst::WTypeKind::ARC4UIntN:
					{
						auto const* uintN = static_cast<awst::ARC4UIntN const*>(_wtype);
						return "uint" + std::to_string(uintN->n());
					}
					case awst::WTypeKind::ARC4StaticArray:
					{
						auto const* sa = static_cast<awst::ARC4StaticArray const*>(_wtype);
						return wtypeToABIName(sa->elementType()) + "[" + std::to_string(sa->arraySize()) + "]";
					}
					case awst::WTypeKind::ARC4DynamicArray:
					{
						auto const* da = static_cast<awst::ARC4DynamicArray const*>(_wtype);
						if (da->arc4Alias() == "string")
							return "string";
						return wtypeToABIName(da->elementType()) + "[]";
					}
					case awst::WTypeKind::ARC4Struct:
					{
						auto const* st = static_cast<awst::ARC4Struct const*>(_wtype);
						std::string result = "(";
						bool first = true;
						for (auto const& [name, fieldType]: st->fields())
						{
							if (!first) result += ",";
							result += wtypeToABIName(fieldType);
							first = false;
						}
						result += ")";
						return result;
					}
					case awst::WTypeKind::ARC4Tuple:
					{
						auto const* tp = static_cast<awst::ARC4Tuple const*>(_wtype);
						std::string result = "(";
						bool first = true;
						for (auto const* elemType: tp->types())
						{
							if (!first) result += ",";
							result += wtypeToABIName(elemType);
							first = false;
						}
						result += ")";
						return result;
					}
					default:
						return _wtype->name();
					}
				};
				auto solTypeToARC4Name = [this, &wtypeToABIName](solidity::frontend::Type const* _type) -> std::string {
					auto* rawType = m_typeMapper.map(_type);
					// Solidity address → ARC4 "address" (not "uint8[32]")
					if (rawType == awst::WType::accountType())
						return "address";
					// Signed integers → "intN" instead of "uintN"
					if (auto const* intT = dynamic_cast<solidity::frontend::IntegerType const*>(_type))
					{
						if (intT->isSigned())
							return "int" + std::to_string(intT->numBits());
					}
					auto* arc4Type = m_typeMapper.mapToARC4Type(rawType);
					return wtypeToABIName(arc4Type);
				};

				std::string methodSelector = memberAccess->memberName() + "(";
				auto const* extRefDecl = memberAccess->annotation().referencedDeclaration;
				if (auto const* funcDef = dynamic_cast<FunctionDefinition const*>(extRefDecl))
				{
					bool first = true;
					for (auto const& param: funcDef->parameters())
					{
						if (!first) methodSelector += ",";
						methodSelector += solTypeToARC4Name(param->type());
						first = false;
					}
					methodSelector += ")";
					if (funcDef->returnParameters().size() > 1)
					{
						// Multiple return values → tuple: (type1,type2,...)
						methodSelector += "(";
						bool firstRet = true;
						for (auto const& retParam: funcDef->returnParameters())
						{
							if (!firstRet) methodSelector += ",";
							methodSelector += solTypeToARC4Name(retParam->type());
							firstRet = false;
						}
						methodSelector += ")";
					}
					else if (funcDef->returnParameters().size() == 1)
						methodSelector += solTypeToARC4Name(funcDef->returnParameters()[0]->type());
					else
						methodSelector += "void";
				}
				else if (auto const* varDecl = dynamic_cast<VariableDeclaration const*>(extRefDecl))
				{
					// Public state variable getter: no params, returns the variable's type
					methodSelector += ")";
					methodSelector += solTypeToARC4Name(varDecl->type());
				}
				else
					methodSelector += ")void";

				auto methodConst = std::make_shared<awst::MethodConstant>();
				methodConst->sourceLocation = loc;
				methodConst->wtype = awst::WType::bytesType();
				methodConst->value = methodSelector;

				// ApplicationArgs must be a WTuple of bytes values
				auto argsTuple = std::make_shared<awst::TupleExpression>();
				argsTuple->sourceLocation = loc;
				argsTuple->items.push_back(std::move(methodConst));
				// Get parameter types for ARC4 re-encoding
				std::vector<solidity::frontend::Type const*> paramSolTypes;
				if (auto const* fd = dynamic_cast<FunctionDefinition const*>(extRefDecl))
					for (auto const& param: fd->parameters())
						paramSolTypes.push_back(param->type());

				// Add actual call arguments (converted to bytes for ApplicationArgs)
				size_t argIdx = 0;
				for (auto const& arg: _node.arguments())
				{
					bool isDynamicBytes = false;
					if (argIdx < paramSolTypes.size())
					{
						auto cat = paramSolTypes[argIdx]->category();
						isDynamicBytes = (cat == solidity::frontend::Type::Category::Array
							&& dynamic_cast<solidity::frontend::ArrayType const*>(paramSolTypes[argIdx])
								&& dynamic_cast<solidity::frontend::ArrayType const*>(paramSolTypes[argIdx])->isByteArrayOrString());
					}
					++argIdx;
					// Inline array literals [a, b, c]: serialize elements directly
					// to avoid building a ReferenceArray that can't be cast to bytes.
					if (auto const* tupleExpr =
							dynamic_cast<solidity::frontend::TupleExpression const*>(arg.get());
						tupleExpr && tupleExpr->isInlineArray())
					{
						// Build each element, ARC4-encode as 32-byte biguint, concat all
						std::shared_ptr<awst::Expression> acc;
						for (auto const& comp: tupleExpr->components())
						{
							if (!comp) continue;
							auto elem = build(*comp);
							elem = implicitNumericCast(std::move(elem), awst::WType::biguintType(), loc);
							// biguint → 32-byte big-endian: concat(bzero(32), bytes(val)), extract last 32
							auto cast = std::make_shared<awst::ReinterpretCast>();
							cast->sourceLocation = loc;
							cast->wtype = awst::WType::bytesType();
							cast->expr = std::move(elem);

							auto zeros = std::make_shared<awst::IntrinsicCall>();
							zeros->sourceLocation = loc;
							zeros->wtype = awst::WType::bytesType();
							zeros->opCode = "bzero";
							zeros->stackArgs.push_back(makeUint64("32", loc));

							auto padded = std::make_shared<awst::IntrinsicCall>();
							padded->sourceLocation = loc;
							padded->wtype = awst::WType::bytesType();
							padded->opCode = "concat";
							padded->stackArgs.push_back(std::move(zeros));
							padded->stackArgs.push_back(std::move(cast));

							auto lenCall = std::make_shared<awst::IntrinsicCall>();
							lenCall->sourceLocation = loc;
							lenCall->wtype = awst::WType::uint64Type();
							lenCall->opCode = "len";
							lenCall->stackArgs.push_back(padded);

							auto offset = std::make_shared<awst::IntrinsicCall>();
							offset->sourceLocation = loc;
							offset->wtype = awst::WType::uint64Type();
							offset->opCode = "-";
							offset->stackArgs.push_back(std::move(lenCall));
							offset->stackArgs.push_back(makeUint64("32", loc));

							auto extracted = std::make_shared<awst::IntrinsicCall>();
							extracted->sourceLocation = loc;
							extracted->wtype = awst::WType::bytesType();
							extracted->opCode = "extract3";
							extracted->stackArgs.push_back(std::move(padded));
							extracted->stackArgs.push_back(std::move(offset));
							extracted->stackArgs.push_back(makeUint64("32", loc));

							if (!acc)
								acc = std::move(extracted);
							else
							{
								auto cat = std::make_shared<awst::IntrinsicCall>();
								cat->sourceLocation = loc;
								cat->wtype = awst::WType::bytesType();
								cat->opCode = "concat";
								cat->stackArgs.push_back(std::move(acc));
								cat->stackArgs.push_back(std::move(extracted));
								acc = std::move(cat);
							}
						}
						if (acc)
							argsTuple->items.push_back(std::move(acc));
						continue;
					}
					auto argExpr = build(*arg);
					// Inner transaction ApplicationArgs must all be bytes.
					if (argExpr->wtype == awst::WType::bytesType()
						|| argExpr->wtype->kind() == awst::WTypeKind::Bytes)
					{
						if (isDynamicBytes)
						{
							// ARC4 byte[] encoding: uint16(length) ++ raw_bytes
							auto lenExpr = std::make_shared<awst::IntrinsicCall>();
							lenExpr->sourceLocation = loc;
							lenExpr->wtype = awst::WType::uint64Type();
							lenExpr->opCode = "len";
							lenExpr->stackArgs.push_back(argExpr);

							auto itobLen = std::make_shared<awst::IntrinsicCall>();
							itobLen->sourceLocation = loc;
							itobLen->wtype = awst::WType::bytesType();
							itobLen->opCode = "itob";
							itobLen->stackArgs.push_back(std::move(lenExpr));

							// extract bytes 6..7 of itob (8 bytes) = big-endian uint16
							auto header = std::make_shared<awst::IntrinsicCall>();
							header->sourceLocation = loc;
							header->wtype = awst::WType::bytesType();
							header->opCode = "extract";
							header->immediates = {6, 2};
							header->stackArgs.push_back(std::move(itobLen));

							auto encoded = std::make_shared<awst::IntrinsicCall>();
							encoded->sourceLocation = loc;
							encoded->wtype = awst::WType::bytesType();
							encoded->opCode = "concat";
							encoded->stackArgs.push_back(std::move(header));
							encoded->stackArgs.push_back(std::move(argExpr));

							argsTuple->items.push_back(std::move(encoded));
						}
						else
						{
							argsTuple->items.push_back(std::move(argExpr));
						}
					}
					else if (argExpr->wtype == awst::WType::uint64Type())
					{
						// uint64 → itob → bytes
						auto itob = std::make_shared<awst::IntrinsicCall>();
						itob->sourceLocation = loc;
						itob->wtype = awst::WType::bytesType();
						itob->opCode = "itob";
						itob->stackArgs.push_back(std::move(argExpr));
						argsTuple->items.push_back(std::move(itob));
					}
					else if (argExpr->wtype == awst::WType::biguintType())
					{
						// biguint → ARC4 uint256 = 32 bytes, left-padded with zeros
						// concat(bzero(32), reinterpret_as_bytes(value)) → extract last 32 bytes
						auto cast = std::make_shared<awst::ReinterpretCast>();
						cast->sourceLocation = loc;
						cast->wtype = awst::WType::bytesType();
						cast->expr = std::move(argExpr);

						auto zeros = std::make_shared<awst::IntrinsicCall>();
						zeros->sourceLocation = loc;
						zeros->wtype = awst::WType::bytesType();
						zeros->opCode = "bzero";
						zeros->stackArgs.push_back(makeUint64("32", loc));

						auto padded = std::make_shared<awst::IntrinsicCall>();
						padded->sourceLocation = loc;
						padded->wtype = awst::WType::bytesType();
						padded->opCode = "concat";
						padded->stackArgs.push_back(std::move(zeros));
						padded->stackArgs.push_back(std::move(cast));

						auto lenCall = std::make_shared<awst::IntrinsicCall>();
						lenCall->sourceLocation = loc;
						lenCall->wtype = awst::WType::uint64Type();
						lenCall->opCode = "len";
						lenCall->stackArgs.push_back(padded);

						auto offset = std::make_shared<awst::IntrinsicCall>();
						offset->sourceLocation = loc;
						offset->wtype = awst::WType::uint64Type();
						offset->opCode = "-";
						offset->stackArgs.push_back(std::move(lenCall));
						offset->stackArgs.push_back(makeUint64("32", loc));

						auto extracted = std::make_shared<awst::IntrinsicCall>();
						extracted->sourceLocation = loc;
						extracted->wtype = awst::WType::bytesType();
						extracted->opCode = "extract3";
						extracted->stackArgs.push_back(std::move(padded));
						extracted->stackArgs.push_back(std::move(offset));
						extracted->stackArgs.push_back(makeUint64("32", loc));
						
						argsTuple->items.push_back(std::move(extracted));
					}
					else if (argExpr->wtype == awst::WType::boolType())
					{
						// bool → ARC4 bool = 1 byte: 0x80 for true, 0x00 for false
						// setbit(byte 0x00, 0, boolValue)
						auto zeroByte = std::make_shared<awst::BytesConstant>();
						zeroByte->sourceLocation = loc;
						zeroByte->wtype = awst::WType::bytesType();
						zeroByte->encoding = awst::BytesEncoding::Base16;
						zeroByte->value = {0x00};
						
						auto setbit = std::make_shared<awst::IntrinsicCall>();
						setbit->sourceLocation = loc;
						setbit->wtype = awst::WType::bytesType();
						setbit->opCode = "setbit";
						setbit->stackArgs.push_back(std::move(zeroByte));
						setbit->stackArgs.push_back(makeUint64("0", loc));
						setbit->stackArgs.push_back(std::move(argExpr));
						
						argsTuple->items.push_back(std::move(setbit));
					}
					else if (argExpr->wtype->kind() == awst::WTypeKind::ReferenceArray
						|| argExpr->wtype->kind() == awst::WTypeKind::ARC4StaticArray
						|| argExpr->wtype->kind() == awst::WTypeKind::ARC4DynamicArray
						|| argExpr->wtype->kind() == awst::WTypeKind::ARC4Struct
						|| argExpr->wtype->kind() == awst::WTypeKind::ARC4Tuple)
					{
						// Complex types: use abi.encodePacked equivalent.
						// For inner txn args, serialize via concatenation of
						// element bytes. Use ReinterpretCast to biguint then
						// to bytes as a simple serialization.
						// For ARC4 types, they're already bytes-backed.
						if (argExpr->wtype->kind() == awst::WTypeKind::ReferenceArray)
						{
							// ReferenceArray stores elements as consecutive bytes.
							// For uint256[N], backing bytes are N x 32-byte big-endian
							// values, which is exactly the ARC4 static array encoding.
							auto cast = std::make_shared<awst::ReinterpretCast>();
							cast->sourceLocation = loc;
							cast->wtype = awst::WType::bytesType();
							cast->expr = std::move(argExpr);
							argsTuple->items.push_back(std::move(cast));
						}
						else
						{
							// ARC4 types are bytes-backed, reinterpret is fine
							auto cast = std::make_shared<awst::ReinterpretCast>();
							cast->sourceLocation = loc;
							cast->wtype = awst::WType::bytesType();
							cast->expr = std::move(argExpr);
							argsTuple->items.push_back(std::move(cast));
						}
					}
					else
					{
						// Fallback: try reinterpret as bytes
						auto cast = std::make_shared<awst::ReinterpretCast>();
						cast->sourceLocation = loc;
						cast->wtype = awst::WType::bytesType();
						cast->expr = std::move(argExpr);
						argsTuple->items.push_back(std::move(cast));
					}
				}
				// Build WTuple type for the args (all bytes)
				std::vector<awst::WType const*> argTypes;
				for (auto const& item: argsTuple->items)
					argTypes.push_back(item->wtype);
				argsTuple->wtype = m_typeMapper.createType<awst::WTuple>(
					std::move(argTypes), std::nullopt
				);

				// Convert address/account to application ID for inner transaction.
				// On Algorand, interface-typed variables should hold app IDs (uint64).
				// Use an intrinsic call (btoi) to convert the bytes-backed account
				// to a uint64, then reinterpret as application type.
				std::shared_ptr<awst::Expression> appId;
				if (baseTranslated->wtype == awst::WType::applicationType())
				{
					appId = std::move(baseTranslated);
				}
				else
				{
					// Convert bytes-backed type (account/address) to app ID (uint64).
					// Addresses are 32 bytes but app IDs are stored as big-endian,
					// so extract the last 8 bytes then btoi.
					std::shared_ptr<awst::Expression> bytesExpr = std::move(baseTranslated);
					// If account type, reinterpret as bytes first
					if (bytesExpr->wtype == awst::WType::accountType())
					{
						auto toBytes = std::make_shared<awst::ReinterpretCast>();
						toBytes->sourceLocation = loc;
						toBytes->wtype = awst::WType::bytesType();
						toBytes->expr = std::move(bytesExpr);
						bytesExpr = std::move(toBytes);
					}
					// extract last 8 bytes (offset 24, length 8) from 32-byte address
					auto extract = std::make_shared<awst::IntrinsicCall>();
					extract->sourceLocation = loc;
					extract->wtype = awst::WType::bytesType();
					extract->opCode = "extract";
					extract->immediates = {24, 8};
					extract->stackArgs.push_back(std::move(bytesExpr));

					auto btoi = std::make_shared<awst::IntrinsicCall>();
					btoi->sourceLocation = loc;
					btoi->wtype = awst::WType::uint64Type();
					btoi->opCode = "btoi";
					btoi->stackArgs.push_back(std::move(extract));

					// Reinterpret uint64 as application (both are uint64-backed)
					auto cast = std::make_shared<awst::ReinterpretCast>();
					cast->sourceLocation = loc;
					cast->wtype = awst::WType::applicationType();
					cast->expr = std::move(btoi);
					appId = std::move(cast);
				}

				std::map<std::string, std::shared_ptr<awst::Expression>> fields;
				fields["ApplicationID"] = std::move(appId);
				fields["OnCompletion"] = makeUint64("0", loc); // NoOp
				fields["ApplicationArgs"] = std::move(argsTuple);

				auto create = buildCreateInnerTransaction(
					TxnTypeAppl, std::move(fields), loc
				);
				auto* retType = m_typeMapper.map(_node.annotation().type);
				push(buildSubmitAndReturn(std::move(create), retType, loc));
				return false;
			}
			if (!isSuperCall)
			{
				// Last resort: try library/free function resolution by AST ID
				auto const* refDecl = memberAccess->annotation().referencedDeclaration;
				bool resolvedHere = false;
				if (auto const* funcDef = dynamic_cast<FunctionDefinition const*>(refDecl))
				{
					resolvedFuncDef = funcDef;
					// Try AST ID lookup in free functions
					auto byId = m_freeFunctionById.find(funcDef->id());
					if (byId != m_freeFunctionById.end())
					{
						call->target = awst::SubroutineID{byId->second};
						resolvedHere = true;
						// Only prepend receiver for using-for, not module-qualified calls
						auto const* baseType2 = memberAccess->expression().annotation().type;
						bool isModCall = baseType2
							&& baseType2->category() == solidity::frontend::Type::Category::Module;
						if (!isModCall)
							isUsingDirectiveCall = true;
					}
					else
					{
						// Try library function map with scope
						if (auto const* scope = funcDef->scope())
						{
							if (auto const* contractDef = dynamic_cast<ContractDefinition const*>(scope))
							{
								if (contractDef->isLibrary())
								{
									std::string key = contractDef->name() + "." + funcDef->name();
									auto it = m_libraryFunctionIds.find(key);
									if (it == m_libraryFunctionIds.end())
									{
										key += "(" + std::to_string(funcDef->parameters().size()) + ")";
										it = m_libraryFunctionIds.find(key);
									}
									if (it != m_libraryFunctionIds.end())
									{
										call->target = awst::SubroutineID{it->second};
										resolvedHere = true;
										isUsingDirectiveCall = true;
									}
								}
							}
						}
					}
				}
				if (!resolvedHere)
				{
					std::string methodName = memberAccess->memberName();
					if (resolvedFuncDef)
						methodName = resolveMethodName(*resolvedFuncDef);
					Logger::instance().debug("library resolution failed for member '" + methodName + "', falling back to InstanceMethodTarget");
					call->target = awst::InstanceMethodTarget{methodName};
				}
			}
		}
	}
	else if (auto const* innerCall = dynamic_cast<FunctionCall const*>(&funcExpr))
	{
		// Pattern: _castToView(fn)(args...) — function pointer cast + immediate call.
		// The inner call returns a function-type; its argument is the actual target function.
		// On AVM we have no view/pure distinction, so call the target directly.
		bool resolvedFunctionPointer = false;
		if (innerCall->arguments().size() == 1)
		{
			if (auto const* argId = dynamic_cast<Identifier const*>(innerCall->arguments()[0].get()))
			{
				auto const* decl = argId->annotation().referencedDeclaration;
				if (auto const* targetFunc = dynamic_cast<FunctionDefinition const*>(decl))
				{
					resolvedFuncDef = targetFunc;
					call->wtype = m_typeMapper.map(_node.annotation().type);
					// Try to resolve as instance method
					call->target = awst::InstanceMethodTarget{resolveMethodName(*targetFunc)};
					Logger::instance().debug(
						"resolved function pointer cast pattern: calling '"
						+ targetFunc->name() + "' directly"
					);
					resolvedFunctionPointer = true;
				}
			}
		}
		if (!resolvedFunctionPointer)
		{
			Logger::instance().warning("could not resolve function call target", loc);
			call->wtype = m_typeMapper.map(_node.annotation().type);
			call->target = awst::InstanceMethodTarget{"unknown"};
		}
	}
	else
	{
		Logger::instance().warning("could not resolve function call target", loc);
		call->wtype = m_typeMapper.map(_node.annotation().type);
		call->target = awst::InstanceMethodTarget{"unknown"};
	}

	// If the resolved function has function-type parameters (e.g. comparators),
	// it was skipped during AWST registration — emit a no-op instead of an invalid call.
	// Solidity function-type params are statically resolvable but the called function
	// (e.g. _quickSort) uses raw EVM memory ops that can't translate to AVM anyway.
	if (resolvedFuncDef)
	{
		bool hasFunctionParam = false;
		for (auto const& p: resolvedFuncDef->parameters())
		{
			if (dynamic_cast<solidity::frontend::FunctionType const*>(p->type()))
			{
				hasFunctionParam = true;
				break;
			}
		}
		if (hasFunctionParam)
		{
			Logger::instance().warning(
				"skipping call to '" + resolvedFuncDef->name()
				+ "' (has function-type parameter, not supported on AVM)", loc
			);
			// Push a void intrinsic (no-op) so the expression stack is consistent
			auto noop = std::make_shared<awst::IntrinsicCall>();
			noop->sourceLocation = loc;
			noop->wtype = awst::WType::voidType();
			noop->opCode = "log";
			auto msg = std::make_shared<awst::BytesConstant>();
			msg->sourceLocation = loc;
			msg->wtype = awst::WType::bytesType();
			msg->value = {};
			noop->stackArgs.push_back(std::move(msg));
			push(noop);
			return false;
		}
	}

	// Collect parameter types from resolved function definition for type coercion
	std::vector<awst::WType const*> paramTypes;
	if (resolvedFuncDef)
	{
		for (auto const& param: resolvedFuncDef->parameters())
			paramTypes.push_back(m_typeMapper.map(param->type()));
	}

	// For `using Library for Type` calls, prepend the receiver as the first arg
	if (isUsingDirectiveCall)
	{
		if (auto const* memberAccess = dynamic_cast<MemberAccess const*>(&funcExpr))
		{
			awst::CallArg ca;
			ca.value = build(memberAccess->expression());
			if (!paramTypes.empty())
				ca.value = implicitNumericCast(std::move(ca.value), paramTypes[0], loc);
			call->args.push_back(std::move(ca));
		}
	}

	// Use sortedArguments() when named args are present to reorder to parameter order.
	// For positional calls, sortedArguments() returns args in original order.
	auto const sortedArgs = _node.sortedArguments();
	for (size_t i = 0; i < sortedArgs.size(); ++i)
	{
		awst::CallArg ca;
		ca.value = build(*sortedArgs[i]);
		// Coerce argument type to match callee parameter type if needed
		// For `using` directive calls, param index 0 is the receiver (already prepended above),
		// so call-site arg[i] maps to paramTypes[i+1]
		size_t paramIdx = isUsingDirectiveCall ? (i + 1) : i;
		if (paramIdx < paramTypes.size())
		{
			ca.value = implicitNumericCast(std::move(ca.value), paramTypes[paramIdx], loc);
		}
		call->args.push_back(std::move(ca));
	}

	// Storage write-back: when a using-for call mutates a storage reference
	// that's backed by box storage (e.g., _roles[role].members.add(account)),
	// the puya backend threads the modified arg as an extra return value.
	// We need to capture it and write it back to the box.
	//
	// Detect: isUsingDirectiveCall + first param is storage + receiver is a
	// FieldExpression on a box-stored struct.
	bool emittedWriteBack = false;
	if (isUsingDirectiveCall && resolvedFuncDef && !call->args.empty()
		&& resolvedFuncDef->stateMutability() != solidity::frontend::StateMutability::View
		&& resolvedFuncDef->stateMutability() != solidity::frontend::StateMutability::Pure
		&& !resolvedFuncDef->parameters().empty()
		&& resolvedFuncDef->parameters()[0]->referenceLocation()
			== solidity::frontend::VariableDeclaration::Location::Storage)
	{
		// Check if receiver (first arg) traces back to a BoxValueExpression
		auto const* receiverExpr = call->args[0].value.get();
		std::shared_ptr<awst::BoxValueExpression> rootBox;
		std::vector<std::string> fieldPath;

		// Walk through FieldExpression → StateGet → BoxValueExpression
		std::function<void(awst::Expression const*)> traceToBox;
		traceToBox = [&](awst::Expression const* e) {
			if (auto const* field = dynamic_cast<awst::FieldExpression const*>(e)) {
				fieldPath.push_back(field->name);
				traceToBox(field->base.get());
			} else if (auto const* sg = dynamic_cast<awst::StateGet const*>(e)) {
				traceToBox(sg->field.get());
			} else if (auto const* box = dynamic_cast<awst::BoxValueExpression const*>(e)) {
				// Clone the box expression for write target
				auto b = std::make_shared<awst::BoxValueExpression>();
				b->sourceLocation = box->sourceLocation;
				b->wtype = box->wtype;
				b->key = box->key;
				b->existsAssertionMessage = std::nullopt;
				rootBox = b;
			}
		};
		traceToBox(receiverExpr);

		Logger::instance().debug("[WRITEBACK] traceToBox: rootBox=" + std::string(rootBox ? "YES" : "NO")
			+ " fieldPath.size=" + std::to_string(fieldPath.size())
			+ " isUsingDirectiveCall=" + std::to_string(isUsingDirectiveCall));
		if (rootBox && !fieldPath.empty())
		{
			auto* origRetType = call->wtype;
			auto* storageArgType = call->args[0].value->wtype;

			auto* tupleType = m_typeMapper.createType<awst::WTuple>(
				std::vector<awst::WType const*>{origRetType, storageArgType}
			);
			call->wtype = tupleType;

			static int storageWriteBackCounter = 0;
			std::string tempName = "__storage_wb_" + std::to_string(storageWriteBackCounter++);

			auto tempVar = std::make_shared<awst::VarExpression>();
			tempVar->sourceLocation = loc;
			tempVar->wtype = tupleType;
			tempVar->name = tempName;

			// Pre-pending: tempVar = call()
			auto assignTemp = std::make_shared<awst::AssignmentStatement>();
			assignTemp->sourceLocation = loc;
			assignTemp->target = tempVar;
			assignTemp->value = std::shared_ptr<awst::Expression>(call);
			m_prePendingStatements.push_back(std::move(assignTemp));

			// Extract original return (element 0)
			auto origRet = std::make_shared<awst::TupleItemExpression>();
			origRet->sourceLocation = loc;
			origRet->wtype = origRetType;
			origRet->base = tempVar;
			origRet->index = 0;
			push(origRet);

			// Extract modified storage arg (element 1)
			auto modifiedArg = std::make_shared<awst::TupleItemExpression>();
			modifiedArg->sourceLocation = loc;
			modifiedArg->wtype = storageArgType;
			modifiedArg->base = tempVar;
			modifiedArg->index = 1;

			// Build copy-on-write: read full struct, replace field, write back
			std::reverse(fieldPath.begin(), fieldPath.end());

			auto readStruct = std::make_shared<awst::StateGet>();
			readStruct->sourceLocation = loc;
			readStruct->wtype = rootBox->wtype;
			readStruct->field = rootBox;
			readStruct->defaultValue = StorageMapper::makeDefaultValue(rootBox->wtype, loc);

			auto const* structType = dynamic_cast<awst::ARC4Struct const*>(rootBox->wtype);
			if (structType && fieldPath.size() == 1)
			{
				auto newStruct = std::make_shared<awst::NewStruct>();
				newStruct->sourceLocation = loc;
				newStruct->wtype = structType;
				for (auto const& [fn, ft]: structType->fields())
				{
					if (fn == fieldPath[0])
						newStruct->values[fn] = modifiedArg;
					else
					{
						auto fieldRead = std::make_shared<awst::FieldExpression>();
						fieldRead->sourceLocation = loc;
						fieldRead->wtype = ft;
						fieldRead->base = readStruct;
						fieldRead->name = fn;
						newStruct->values[fn] = std::move(fieldRead);
					}
				}

				auto writeBack = std::make_shared<awst::AssignmentExpression>();
				writeBack->sourceLocation = loc;
				writeBack->wtype = rootBox->wtype;
				writeBack->target = rootBox;
				writeBack->value = std::move(newStruct);

				auto stmt = std::make_shared<awst::ExpressionStatement>();
				stmt->sourceLocation = loc;
				stmt->expr = std::move(writeBack);
				m_pendingStatements.push_back(std::move(stmt));
				emittedWriteBack = true;
			}
		}
	}

	if (!emittedWriteBack)
		push(call);

	return false;
}


} // namespace puyasol::builder
