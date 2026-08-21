/// @file SolLengthAccess.cpp
/// array.length, bytes.length, box-backed array length.
/// Migrated from MemberAccessBuilder.cpp lines 476-555.

#include "builder/sol-ast/members/SolLengthAccess.h"
#include "Logger.h"
#include "builder/builtin/AppCodeSizeLowering.h"
#include "builder/sol-ast/EvmSlotLowering.h"
#include "builder/storage/EvmLayoutMode.h"
#include "builder/storage/StorageMapper.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/sol-types/TypeCoercion.h"

#include <libsolidity/ast/AST.h>

#include <sstream>
#include <cstdint>
#include <limits>

namespace puyasol::builder::sol_ast
{

using namespace solidity::frontend;

namespace {

// Peel a type-conversion FunctionCall wrapping an IndexRangeAccess:
// `uint256[](x[s:e])` → returns the inner IndexRangeAccess. Direct
// IndexRangeAccess passes through unchanged. Returns nullptr for anything
// else.
IndexRangeAccess const* peelToSlice(Expression const& expr)
{
	if (auto const* rg = dynamic_cast<IndexRangeAccess const*>(&expr))
		return rg;
	if (auto const* call = dynamic_cast<FunctionCall const*>(&expr))
	{
		if (call->annotation().kind.set()
			&& *call->annotation().kind == FunctionCallKind::TypeConversion
			&& !call->arguments().empty())
		{
			return peelToSlice(*call->arguments()[0]);
		}
	}
	return nullptr;
}

} // namespace

std::shared_ptr<awst::Expression> SolLengthAccess::toAwst()
{
	auto const& baseExpr = baseExpression();

	// `addressExpr.code.length` must not build the intermediate `.code` bytes.
	// Approval programs can be larger than AVM's maximum stack byte value, so
	// fetching AppApprovalProgram merely to apply `len` fails for exactly the
	// larger contracts that this predicate is commonly used to inspect.  Query
	// small application metadata instead; Yul extcodesize uses the same helper.
	if (auto const* codeAccess = dynamic_cast<MemberAccess const*>(&baseExpr);
		codeAccess && codeAccess->memberName() == "code")
	{
		// EVM stores runtime code only after initcode completes.
		if (m_scope.isInConstructor())
			return awst::makeZero(m_loc, awst::WType::uint64Type());

		auto const& addressExpr = codeAccess->expression();
		// Literal/precompile/EOA addresses are not applications under the
		// compiler's contract-value convention and therefore have no code.
		if (auto const* fc = dynamic_cast<FunctionCall const*>(&addressExpr);
			fc && fc->annotation().kind.set()
			&& *fc->annotation().kind == FunctionCallKind::TypeConversion
			&& fc->arguments().size() == 1)
		{
			if (auto const* lit = dynamic_cast<Literal const*>(fc->arguments()[0].get());
				lit && lit->token() == Token::Number)
			{
				return awst::makeZero(m_loc, awst::WType::uint64Type());
			}
		}

		auto address = buildExpr(addressExpr);
		std::shared_ptr<awst::Expression> application;
		if (auto const* intrinsic = dynamic_cast<awst::IntrinsicCall const*>(address.get());
			intrinsic && intrinsic->opCode == "global"
			&& !intrinsic->immediates.empty()
			&& std::holds_alternative<std::string>(intrinsic->immediates[0])
			&& std::get<std::string>(intrinsic->immediates[0])
				== "CurrentApplicationAddress")
		{
			application = awst::makeAsApplication(
				awst::makeGlobal("CurrentApplicationID",
					awst::WType::uint64Type(), m_loc), m_loc);
		}
		else
		{
			Logger::instance().warning(
				"`address(addr).code.length` resolves the application id from "
				"the address's last 8 bytes (this compiler's contract-value "
				"convention). It returns zero for a missing application and the "
				"allocated AVM program capacity for an existing one; AVM cannot "
				"observe an oversized program's exact byte length without "
				"materialising it.", m_loc);
			application = awst::makeAsApplication(
				awst::makeWord32ToUInt64(awst::makeAsBytes(address, m_loc), m_loc),
				m_loc);
		}

		return AppCodeSizeLowering::lower(
			m_ctx.typeMapper, std::move(application), m_loc, m_ctx.preEffects());
	}

	// Slice length: `x[s:e].length`, or the cast form `uint256[](x[s:e]).length`.
	// Walk the slice chain, emit bounds asserts, and compute
	//   final_length = end_outer - start_outer - ... (per-level clamped)
	// without materialising the intermediate substring3 bytes.
	if (auto const* rg = peelToSlice(baseExpr))
	{
		std::vector<IndexRangeAccess const*> slices;
		Expression const* cur = rg;
		while (auto const* r = dynamic_cast<IndexRangeAccess const*>(cur))
		{
			slices.push_back(r);
			cur = &r->baseExpression();
		}
		std::reverse(slices.begin(), slices.end());

		auto const* rootArrType = dynamic_cast<ArrayType const*>(cur->annotation().type);
		if (rootArrType && !rootArrType->isByteArrayOrString())
		{
			auto rootBase = buildExpr(*cur);
			std::string idSuffix = std::to_string(m_memberAccess.id());
			std::string rootVarName = "__slice_root_" + idSuffix;
			auto rootVar = awst::makeVarExpression(rootVarName, rootBase->wtype, m_loc);
			m_ctx.preEffects().push_back(
				awst::makeAssignmentStatement(rootVar, rootBase, m_loc));

			auto makeLen = [&](std::shared_ptr<awst::Expression> arr) -> std::shared_ptr<awst::Expression> {
				return awst::makeArrayLength(std::move(arr), awst::WType::uint64Type(), m_loc);
			};

			std::string lenVarName = "__slice_rootlen_" + idSuffix;
			auto lenSeed = makeLen(
				awst::makeVarExpression(rootVarName, rootBase->wtype, m_loc));
			auto lenVar = awst::makeVarExpression(lenVarName, awst::WType::uint64Type(), m_loc);
			m_ctx.preEffects().push_back(
				awst::makeAssignmentStatement(lenVar, lenSeed, m_loc));
			std::shared_ptr<awst::Expression> cumLength
				= awst::makeVarExpression(lenVarName, awst::WType::uint64Type(), m_loc);

			int sliceIx = 0;
			for (auto const* slice: slices)
			{
				std::string sIx = idSuffix + "_" + std::to_string(sliceIx++);
				std::string startName = "__slice_s_" + sIx;
				std::string endName = "__slice_e_" + sIx;

				std::shared_ptr<awst::Expression> startExpr;
				if (slice->startExpression())
					startExpr = buildExpr(*slice->startExpression());
				else
					startExpr = awst::makeZero(m_loc);
				startExpr = builder::TypeCoercion::implicitNumericCast(
					std::move(startExpr), awst::WType::uint64Type(), m_loc);

				std::shared_ptr<awst::Expression> endExpr;
				if (slice->endExpression())
					endExpr = buildExpr(*slice->endExpression());
				else
					endExpr = cumLength;
				endExpr = builder::TypeCoercion::implicitNumericCast(
					std::move(endExpr), awst::WType::uint64Type(), m_loc);

				auto startVar = awst::makeVarExpression(startName, awst::WType::uint64Type(), m_loc);
				m_ctx.preEffects().push_back(
					awst::makeAssignmentStatement(startVar, startExpr, m_loc));
				auto endVar = awst::makeVarExpression(endName, awst::WType::uint64Type(), m_loc);
				m_ctx.preEffects().push_back(
					awst::makeAssignmentStatement(endVar, endExpr, m_loc));

				{
					auto cmp = awst::makeNumericCompare(
						awst::makeVarExpression(startName, awst::WType::uint64Type(), m_loc),
						awst::NumericComparison::Lte,
						awst::makeVarExpression(endName, awst::WType::uint64Type(), m_loc),
						m_loc);
					m_ctx.preEffects().push_back(awst::makeExpressionStatement(
						awst::makeAssert(std::move(cmp), m_loc, "slice: start > end"), m_loc));
				}
				{
					auto cmp = awst::makeNumericCompare(
						awst::makeVarExpression(endName, awst::WType::uint64Type(), m_loc),
						awst::NumericComparison::Lte,
						cumLength,
						m_loc);
					m_ctx.preEffects().push_back(awst::makeExpressionStatement(
						awst::makeAssert(std::move(cmp), m_loc, "slice: end > length"), m_loc));
				}

				auto diff = awst::makeUInt64BinOp(
					awst::makeVarExpression(endName, awst::WType::uint64Type(), m_loc),
					awst::UInt64BinaryOperator::Sub,
					awst::makeVarExpression(startName, awst::WType::uint64Type(), m_loc),
					m_loc);

				std::string nextLenName = "__slice_l_" + sIx;
				auto nextLenVar = awst::makeVarExpression(nextLenName, awst::WType::uint64Type(), m_loc);
				m_ctx.preEffects().push_back(
					awst::makeAssignmentStatement(nextLenVar, diff, m_loc));
				cumLength = awst::makeVarExpression(nextLenName, awst::WType::uint64Type(), m_loc);
			}

			return cumLength;
		}
	}

	// --evm-storage-layout: dynamic storage array length = its slot's word.
	if (m_ctx.typeMapper.profile().evmStorageLayout)
		if (auto const* arrType = dynamic_cast<ArrayType const*>(
				baseExpr.annotation().type);
			arrType && arrType->dataStoredIn(solidity::frontend::DataLocation::Storage)
			&& EvmSlotLowering::isStorageStateRef(baseExpr))
		{
			if (!arrType->isDynamicallySized() && !arrType->isByteArrayOrString())
			{
				std::ostringstream oss;
				oss << arrType->length();
				return awst::makeIntegerConstant(oss.str(), m_loc,
					arrType->length() > std::numeric_limits<uint64_t>::max()
						? awst::WType::biguintType() : awst::WType::uint64Type());
			}
			if (arrType->isByteArrayOrString())
			{
				EvmSlotLowering low(m_ctx, m_scope, m_loc);
				auto addr = low.resolve(baseExpr);
				if (!addr)
					return nullptr;
				return awst::makeLen(low.readBytesValue(*addr), m_loc);
			}
			EvmSlotLowering low(m_ctx, m_scope, m_loc);
			auto addr = low.resolve(baseExpr);
			if (!addr)
				return nullptr;
			return EvmSlotLowering::readSlotWord(addr->slot, m_loc);
		}

	// Box-backed dynamic array: length = box_len(key) / elemSize
	if (auto const* ident = dynamic_cast<Identifier const*>(&baseExpr))
	{
		if (auto const* varDecl = dynamic_cast<VariableDeclaration const*>(
				ident->annotation().referencedDeclaration))
		{
			if (auto const* arrType = dynamic_cast<ArrayType const*>(varDecl->type());
				arrType && !arrType->isByteArrayOrString())
			{
				auto const& keyParam = m_scope.findMappingKeyParam(varDecl->id());
				if (!keyParam.empty())
				{
					if (!arrType->isDynamicallySized())
						return awst::makeIntegerConstant(
							arrType->length().str(), m_loc,
							arrType->length() > std::numeric_limits<uint64_t>::max()
								? awst::WType::biguintType()
								: awst::WType::uint64Type());
					auto key = awst::makeReinterpretCast(
						awst::makeVarExpression(
							keyParam, awst::WType::bytesType(), m_loc),
						awst::WType::boxKeyType(), m_loc);
					return stateDynArrayLengthForKey(
						m_ctx, std::move(key), arrType, m_loc);
				}
			}
			if (varDecl->isStateVariable()
				&& !varDecl->isConstant()
				&& !varDecl->immutable()
				&& m_ctx.storageMapper.shouldUseBoxStorage(*varDecl)
				&& dynamic_cast<ArrayType const*>(varDecl->type()))
			{
				auto const* arrType = dynamic_cast<ArrayType const*>(varDecl->type());

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
					auto solLenType = m_memberAccess.annotation().type;
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
					return awst::makeIntegerConstant(oss.str(), m_loc, lenWtype);
				}
				// Dynamic bytes / string state var: the raw box byte count is
				// the Solidity length. No 2-byte ARC4 prefix is applied on
				// write (see `bytes data; data = msg.data;` write path which
				// drops raw bytes into the box), so don't subtract one here.
				// Key by the physical binding, matching the write paths —
				// colliding inherited declarations diverge from the raw name.
				auto const boxName =
					m_ctx.storageMapper.physicalBindingFor(*varDecl).name;
				if (arrType->isByteArrayOrString())
				{
					auto boxKey = awst::makeUtf8BytesConstant(boxName, m_loc);
					auto boxLen = builder::StorageMapper::makeBoxLenTuple(
						m_ctx.typeMapper, std::move(boxKey), m_loc);
					return awst::makeTupleItem(
						std::move(boxLen), 0, awst::WType::uint64Type(), m_loc);
				}

				return stateDynArrayLength(m_ctx, boxName, arrType, m_loc);
			}
		}
	}

	auto base = buildExpr(baseExpr);

	// bytesN.length → compile-time constant N (fixed-size bytes)
	if (auto const* fixedBytes = dynamic_cast<awst::BytesWType const*>(base->wtype))
	{
		if (fixedBytes->length().has_value())
		{
			auto c = awst::makeIntegerConstant(*fixedBytes->length(), m_loc);
			return c;
		}
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
	unsigned elemSize = builder::StorageMapper::computeEncodedElementSize(arc4ElemType);

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
