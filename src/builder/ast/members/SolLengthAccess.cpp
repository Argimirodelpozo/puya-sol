/// @file SolLengthAccess.cpp
/// array.length, bytes.length, box-backed array length.

#include "builder/ast/members/SolLengthAccess.h"
#include "builder/solc/SolcFacts.h"
#include "builder/ast/members/SolAddressProperty.h"
#include "builder/storage/slot/EvmSlotLowering.h"
#include "builder/ast/exprs/SolIndexAccess.h"
#include "builder/target/EvmLayoutMode.h"
#include "builder/storage/StorageMapper.h"
#include "builder/types/TypeMapper.h"
#include "builder/types/TypeCoercion.h"

#include <libsolidity/ast/AST.h>

#include <sstream>
#include <cstdint>
#include <limits>
#include <optional>

namespace puyasol::builder::sol_ast
{

using namespace solidity::frontend;

namespace
{

// --evm-storage-layout: dynamic storage array length = its slot's word.
// Engaged result may hold nullptr (resolve error, already logged);
// nullopt = rung not applicable.
std::optional<std::shared_ptr<awst::Expression>> trySlotModeArrayLength(
	eb::ContractContext& ctx, Context& scope, Expression const& baseExpr,
	awst::SourceLocation const& loc)
{
	auto const* arrType = dynamic_cast<ArrayType const*>(baseExpr.annotation().type);
	if (!arrType || !arrType->dataStoredIn(DataLocation::Storage)
		|| !((ctx.typeMapper.profile().evmStorageLayout && EvmSlotLowering::isStorageStateRef(baseExpr))
			|| EvmSlotLowering::isSlotHandleRef(baseExpr, ctx, scope)))
		return std::nullopt;

	EvmSlotLowering low(ctx, scope, loc);
	auto addr = low.resolve(baseExpr);
	if (!addr) return std::shared_ptr<awst::Expression>{};
	// Resolve/evaluate the receiver even when solc provides a constant length.
	if (!arrType->isDynamicallySized())
	{
		ctx.emitSequencedOperand({}, addr->slot, true, loc);
		return awst::makeIntegerConstant(arrType->length().str(), loc,
			arrType->length() > std::numeric_limits<uint64_t>::max()
				? awst::WType::biguintType() : awst::WType::uint64Type());
	}
	if (arrType->isByteArrayOrString())
		return awst::makeLen(low.readBytesValue(*addr), loc);
	return EvmSlotLowering::readSlotWord(addr->slot, loc);
}

// Array behind a mapping-key/storage-ref param: fixed size folds to a
// constant, dynamic reads the box keyed by the runtime prefix.
std::shared_ptr<awst::Expression> tryKeyParamArrayLength(
	eb::ContractContext& ctx, Context& scope,
	VariableDeclaration const& varDecl, awst::SourceLocation const& loc)
{
	auto const* arrType = dynamic_cast<ArrayType const*>(varDecl.type());
	if (!arrType || arrType->isByteArrayOrString())
		return nullptr;
	auto const& keyParam = scope.bindings.mappingKeyParams.get(varDecl.id());
	if (keyParam.empty())
		return nullptr;
	if (!arrType->isDynamicallySized())
		return awst::makeIntegerConstant(
			arrType->length().str(), loc,
			arrType->length() > std::numeric_limits<uint64_t>::max()
				? awst::WType::biguintType()
				: awst::WType::uint64Type());
	auto key = awst::makeReinterpretCast(
		awst::makeVarExpression(
			keyParam, awst::WType::bytesType(), loc),
		awst::WType::boxKeyType(), loc);
	return SolLengthAccess::stateDynArrayLengthForKey(
		ctx, std::move(key), arrType, loc);
}

// Box-backed state array: fixed size folds to a constant (width per the
// annotation type), dynamic bytes/string reads the raw box length, other
// dynamic arrays divide the box payload by the encoded element size.
std::shared_ptr<awst::Expression> tryBoxStateArrayLength(
	eb::ContractContext& ctx, MemberAccess const& node,
	VariableDeclaration const& varDecl, awst::SourceLocation const& loc)
{
	if (!varDecl.isStateVariable()
		|| varDecl.isConstant()
		|| varDecl.immutable()
		|| !ctx.storageMapper.shouldUseBoxStorage(varDecl)
		|| !dynamic_cast<ArrayType const*>(varDecl.type()))
		return nullptr;

	auto const* arrType = dynamic_cast<ArrayType const*>(varDecl.type());

	// Statically-sized state arrays: `.length` is a compile-time
	// constant, not a box read. Avoid emitting (box_len - 2) /
	// elemSize, which underflows for empty boxes.
	if (!arrType->isDynamicallySized() && !arrType->isByteArrayOrString())
	{
		std::ostringstream oss;
		oss << arrType->length();
		// uint256 array sizes (e.g. from erc7201()) don't fit in
		// uint64 — emit as biguint in that case. The result's
		// Solidity type is uint256 which maps to biguint anyway.
		auto solLenType = node.annotation().type;
		awst::WType const* lenWtype = awst::WType::uint64Type();
		if (solLenType && solLenType->category()
				== solidity::frontend::Type::Category::Integer)
		{
			auto const* intType = dynamic_cast<
				solidity::frontend::IntegerType const*>(solLenType);
			if (intType && intType->numBits() > 64)
				lenWtype = awst::WType::biguintType();
		}
		else if (arrType->length() > std::numeric_limits<uint64_t>::max())
		{
			lenWtype = awst::WType::biguintType();
		}
		return awst::makeIntegerConstant(oss.str(), loc, lenWtype);
	}
	// Dynamic bytes / string state var: the raw box byte count is
	// the Solidity length. No 2-byte ARC4 prefix is applied on
	// write (see `bytes data; data = msg.data;` write path which
	// drops raw bytes into the box), so don't subtract one here.
	// Key by the physical binding, matching the write paths —
	// colliding inherited declarations diverge from the raw name.
	auto const boxName =
		ctx.storageMapper.physicalBindingFor(varDecl).key;
	if (arrType->isByteArrayOrString())
	{
		auto boxKey = awst::makeUtf8BytesConstant(boxName, loc);
		auto boxLen = builder::StorageMapper::makeBoxLenTuple(
			ctx.typeMapper, std::move(boxKey), loc);
		return awst::makeTupleItem(
			std::move(boxLen), 0, awst::WType::uint64Type(), loc);
	}

	return SolLengthAccess::stateDynArrayLength(ctx, boxName, arrType, loc);
}

} // anonymous namespace

std::shared_ptr<awst::Expression> SolLengthAccess::toAwst()
{
	auto const& baseExpr = baseExpression();

	if (auto const* codeAccess = SolcFacts::expressionAs<MemberAccess>(&baseExpr);
		codeAccess && codeAccess->memberName() == "code"
		&& dynamic_cast<AddressType const*>(codeAccess->expression().annotation().type))
		return SolAddressProperty::buildCodeMetadata(m_ctx, m_scope,
			codeAccess->expression(), SolAddressProperty::CodeProperty::Size, m_loc);

	if (auto slice = SolIndexRangeAccess::resolveSlice(m_ctx, baseExpr, m_loc))
		return slice->length;

	if (auto slotLen = trySlotModeArrayLength(m_ctx, m_scope, baseExpr, m_loc))
		return *slotLen;

	// Box-backed dynamic array: length = box_len(key) / elemSize
	if (auto const* ident = SolcFacts::expressionAs<Identifier>(&baseExpr))
		if (auto const* varDecl = dynamic_cast<VariableDeclaration const*>(
				ident->annotation().referencedDeclaration))
		{
			if (auto keyParamLen = tryKeyParamArrayLength(m_ctx, m_scope, *varDecl, m_loc))
				return keyParamLen;
			if (auto boxLen = tryBoxStateArrayLength(m_ctx, m_memberAccess, *varDecl, m_loc))
				return boxLen;
		}

	auto base = m_ctx.pinIfWriteBacks(m_ctx.lower(baseExpr, false), m_loc);
	if (auto const* fixedBytes = dynamic_cast<FixedBytesType const*>(baseExpr.annotation().type))
	{
		m_ctx.queuePreExpression(std::move(base), m_loc);
		return awst::makeIntegerConstant(fixedBytes->numBytes(), m_loc);
	}

	// bytes.length → len intrinsic
	if (base->wtype == awst::WType::bytesType())
		return awst::makeLen(std::move(base), m_loc);

	// array.length → ArrayLength node
	return awst::makeArrayLength(std::move(base), awst::WType::uint64Type(), m_loc);
}

std::shared_ptr<awst::Expression> SolLengthAccess::stateDynArrayLength(
	eb::ContractContext& _ctx,
	std::string const& _name,
	solidity::frontend::ArrayType const* _arrType,
	awst::SourceLocation const& _loc)
{
	return stateDynArrayLengthForKey(
		_ctx, awst::makeUtf8BytesConstant(
			_name, _loc, awst::WType::boxKeyType()), _arrType, _loc);
}

std::shared_ptr<awst::Expression> SolLengthAccess::stateDynArrayLengthForKey(
	eb::ContractContext& _ctx,
	std::shared_ptr<awst::Expression> _boxKey,
	solidity::frontend::ArrayType const* _arrType,
	awst::SourceLocation const& _loc)
{
	// Use the width-preserving Sol→ARC4 map (not map()+mapToARC4Type,
	// which erases sub-256 widths to biguint→32) so the divisor
	// matches the stride push/index store at (SolArrayMethod uses
	// mapSolTypeToARC4 too). Otherwise uint128[] divides by 32 not 16.
	auto* arc4ElemType = _ctx.typeMapper.mapSolTypeToARC4(_arrType->baseType());
	unsigned elemSize = builder::computeEncodedElementSize(arc4ElemType).fixedBytes<unsigned>().value_or(0);

	// Elements of unknown fixed size (nested dynamic arrays, mappings) can't
	// use the (box_len - 2) / elemSize trick. The ARC4 dynamic-array encoding
	// keeps a uint16 length prefix at box offset 0 — read that. box_get returns
	// (contents, exists); ternary on exists so an uninit box reads as length 0.
	if (elemSize == 0)
	{
		auto* getTupleType = _ctx.typeMapper.createType<awst::WTuple>(
			std::vector<awst::WType const*>{
				awst::WType::bytesType(), awst::WType::boolType()});
		auto boxGet = awst::makeIntrinsicCall("box_get", getTupleType, _loc);
		boxGet->stackArgs.push_back(_boxKey);

		auto contents = awst::makeTupleItem(boxGet, 0, awst::WType::bytesType(), _loc);

		auto exists = awst::makeTupleItem(boxGet, 1, awst::WType::boolType(), _loc);

		auto extractLen = awst::makeIntrinsicCall(
			"extract_uint16", awst::WType::uint64Type(), _loc);
		extractLen->stackArgs.push_back(std::move(contents));
		extractLen->stackArgs.push_back(awst::makeZero(_loc));

		return awst::makeConditional(
			std::move(exists), std::move(extractLen),
			awst::makeIntegerConstant("0", _loc),
			awst::WType::uint64Type(), _loc);
	}

	auto boxLen = builder::StorageMapper::makeBoxLenTuple(
		_ctx.typeMapper, std::move(_boxKey), _loc);
	auto lenVal = awst::makeTupleItem(std::move(boxLen), 0, awst::WType::uint64Type(), _loc);

	auto elemSizeConst = awst::makeIntegerConstant(elemSize, _loc);

	// Guard against box_len returning 0 (uninitialised box):
	// `(0 - 2) / elemSize` underflows. Use `max(len, 2)` so the
	// subtraction always stays non-negative, yielding 0 for
	// empty boxes.
	auto two = awst::makeIntegerConstant("2", _loc);

	auto lenGe2 = awst::makeNumericCompare(lenVal, awst::NumericComparison::Gte, two, _loc);

	auto safeLen = awst::makeConditional(
		std::move(lenGe2), std::move(lenVal), std::move(two),
		awst::WType::uint64Type(), _loc);

	// Subtract 2-byte ARC4 length header before dividing
	auto headerSize = awst::makeIntegerConstant("2", _loc);
	auto dataLen = awst::makeUInt64BinOp(std::move(safeLen), awst::UInt64BinaryOperator::Sub, std::move(headerSize), _loc);

	auto divExpr = awst::makeUInt64BinOp(std::move(dataLen), awst::UInt64BinaryOperator::FloorDiv, std::move(elemSizeConst), _loc);
	return divExpr;
}

} // namespace puyasol::builder::sol_ast
