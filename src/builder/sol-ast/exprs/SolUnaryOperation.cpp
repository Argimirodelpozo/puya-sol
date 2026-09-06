/// @file SolUnaryOperation.cpp — unary operation translation.

#include "Logger.h"
#include "builder/AwstShorthand.h"
#include "builder/sol-types/SolcConstFold.h"
#include "awst/NameGen.h"
#include "builder/sol-types/SolIntType.h"
#include "builder/sol-types/TypeCoercion.h"
#include "builder/sol-ast/exprs/SolUnaryOperation.h"
#include "builder/sol-eb/NodeBuilder.h"
#include "builder/sol-eb/BuilderOps.h"
#include "builder/sol-eb/AssignmentHelper.h"
#include "builder/sol-eb/BigUIntMathHelpers.h"
#include "builder/sol-ast/EvmSlotLowering.h"
#include "builder/storage/EvmLayoutMode.h"
#include "builder/storage/SlotHandleAccess.h"
#include "builder/sol-types/Arc4Defaults.h"
#include "builder/storage/StorageMapper.h"
#include "builder/storage/TransientStorage.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/itxn/CallResolver.h"

#include <libsolidity/ast/AST.h>
#include <libsolutil/Numeric.h>
#include <sstream>

namespace puyasol::builder::sol_ast
{

using namespace solidity::frontend;
using Token = solidity::frontend::Token;

namespace
{
// Promote a value to a signed 256-bit biguint: a uint64 value is widened via
// itob then reinterpreted as biguint; any other type is passed through.
// (Inc/dec's copy of this now lives in eb::promoteToBiguint via buildIncDec.)
std::shared_ptr<awst::Expression> promoteToSignedBiguint(
	std::shared_ptr<awst::Expression> value, awst::SourceLocation const& loc)
{
	if (value->wtype == awst::WType::uint64Type())
	{
		auto itob = awst::makeItob(std::move(value), loc);
		value = awst::makeAsBiguint(std::move(itob), loc);
	}
	return value;
}
} // namespace

SolUnaryOperation::SolUnaryOperation(
	eb::ContractContext& _ctx, UnaryOperation const& _node)
	: SolExpression(_ctx, _node), m_unaryOp(_node)
{
}

bool SolUnaryOperation::isBigUInt(awst::WType const* _type) const
{
	return _type == awst::WType::biguintType();
}

std::shared_ptr<awst::Expression> SolUnaryOperation::handleNot(
	std::shared_ptr<awst::Expression> _operand)
{
	auto e = awst::makeNot(std::move(_operand), m_loc);
	return e;
}

std::shared_ptr<awst::Expression> SolUnaryOperation::handleNegate(
	std::shared_ptr<awst::Expression> _operand)
{
	// For constant expressions like `-2`, operand type is RationalNumberType;
	// fall through to check result type for signed intN.
	auto intInfo = SolIntType::fromSol(m_unaryOp.subExpression().annotation().type);
	if (!intInfo || !intInfo->isSigned)
	{
		// constant `-2`: operand is RationalNumberType, not intN — use the result type.
		if (auto resultInfo = SolIntType::fromSol(m_unaryOp.annotation().type);
			resultInfo && resultInfo->isSigned)
			intInfo = resultInfo;
	}

	if (intInfo && intInfo->isSigned)
	{
		// -x = (2^N - x) mod 2^N; overflow: x == 2^(N-1) i.e. INT_MIN
		auto [pow2NStr, halfNStr] = intInfo->pow2NAndHalf();
		return eb::buildSignedNegate(
			m_ctx, m_scope.isUnchecked(), intInfo->bits, pow2NStr, halfNStr,
			m_unaryOp.id(),
			promoteToSignedBiguint(std::move(_operand), m_loc), m_loc);
	}

	// biguint constant negation: two's complement 2^256 - value
	if (isBigUInt(_operand->wtype))
	{
		if (auto const* intConst = dynamic_cast<awst::IntegerConstant const*>(_operand.get()))
		{
			solidity::u256 val(intConst->value);
			// Allow val == 2^255 (int256.min valid); strict < missed it, AVM b- crashes on 0-2^255.
			if (val > 0 && val <= (solidity::u256(1) << 255))
			{
				static const std::string pow256Str =
					kPow2_256;
				solidity::u256 pow256(pow256Str);
				solidity::u256 negVal = pow256 - val;
				std::ostringstream oss;
				oss << negVal;
				auto result = awst::makeIntegerConstant(oss.str(), m_loc, awst::WType::biguintType());
				return result;
			}
		}

		// Non-constant unsigned negation: 0 - x
		auto zero = awst::makeZero(m_loc, awst::WType::biguintType());
		auto e = awst::makeBigUIntBinOp(std::move(zero), awst::BigUIntBinaryOperator::Sub, std::move(_operand), m_loc);
		return e;
	}
	// uint64 constant negation: produce biguint two's-comp. Can't narrow: if the
	// context is int256 storage, uint64(2^64-1) would sign-extend wrong.
	if (_operand->wtype == awst::WType::uint64Type())
	{
		if (auto const* intConst = dynamic_cast<awst::IntegerConstant const*>(_operand.get()))
		{
			solidity::u256 val(intConst->value);
			if (val > 0)
			{
				static const std::string pow256Str =
					kPow2_256;
				solidity::u256 pow256(pow256Str);
				solidity::u256 negVal = pow256 - val;
				std::ostringstream oss;
				oss << negVal;
				auto result = awst::makeIntegerConstant(oss.str(), m_loc, awst::WType::biguintType());
				return result;
			}
		}
	}
	auto zero2 = awst::makeZero(m_loc, _operand->wtype);
	if (_operand->wtype == awst::WType::uint64Type())
	{
		auto e = awst::makeUInt64BinOp(std::move(zero2), awst::UInt64BinaryOperator::Sub, std::move(_operand), m_loc);
		return e;
	}
	auto e = awst::makeBigUIntBinOp(std::move(zero2), awst::BigUIntBinaryOperator::Sub, std::move(_operand), m_loc);
	return e;
}

std::shared_ptr<awst::Expression> SolUnaryOperation::handleBitNot(
	std::shared_ptr<awst::Expression> _operand)
{
	auto* resultType = _operand->wtype;
	if (_operand->wtype == awst::WType::uint64Type())
	{
		unsigned maskBits = 64;
		if (auto it = SolIntType::fromSol(m_unaryOp.subExpression().annotation().type))
			maskBits = it->bits;

		solidity::u256 mask = (maskBits >= 64)
			? solidity::u256("18446744073709551615")
			: (solidity::u256(1) << maskBits) - 1;
		std::ostringstream oss;
		oss << mask;

		auto maxVal = awst::makeIntegerConstant(oss.str(), m_loc);
		auto xorOp = awst::makeUInt64BinOp(std::move(_operand), awst::UInt64BinaryOperator::BitXor, std::move(maxVal), m_loc);
		return xorOp;
	}
	auto expr = std::move(_operand);
	// biguint-backed intN (64<N<256): ~x = x XOR (2^N-1) masked to bit width.
	// Raw BitInvert inverts wrong count: ~uint128(5) must be 2^128-6, not ~0x05=0xFA
	// (V4 LPFee removeOverrideFlagAndValidate was silently a no-op).
	if (expr->wtype == awst::WType::biguintType())
	{
		unsigned maskBits = 0;
		if (auto it = SolIntType::fromSol(m_unaryOp.subExpression().annotation().type))
			maskBits = it->bits;
		if (maskBits > 0 && maskBits < 256)
		{
			solidity::u256 mask = (solidity::u256(1) << maskBits) - 1;
			std::ostringstream oss; oss << mask;
			auto maskConst = awst::makeIntegerConstant(
				oss.str(), m_loc, awst::WType::biguintType());
			return awst::makeBigUIntBinOp(
				std::move(expr), awst::BigUIntBinaryOperator::BitXor,
				std::move(maskConst), m_loc);
		}
		auto cast = awst::makeAsBytes(std::move(expr), m_loc);
		expr = std::move(cast);
	}
	auto const* exprWtype = expr->wtype;
	auto e = awst::makeBitInvert(std::move(expr), exprWtype, m_loc);
	if (resultType == awst::WType::biguintType())
	{
		auto castBack = awst::makeReinterpretCast(std::move(e), resultType, m_loc);
		return castBack;
	}
	return e;
}

std::shared_ptr<awst::Expression> SolUnaryOperation::handleIncDec(
	std::shared_ptr<awst::Expression> _operand)
{
	bool isPrefix = m_unaryOp.isPrefixOperation();
	bool isInc = (m_unaryOp.getOperator() == Token::Inc);

	// Transient state var: no AWST lvalue (AssignmentStatement with a read
	// expr as target isn't one) — writes go through TransientStorage::buildWrite.
	// Detected here, HANDLED after makeNewValue below so it shares the same
	// width/sign/checked rules as every other target: the old early-out
	// computed a bare ±1 (uint8 t=255; t++ silently wrapped to 0 where EVM
	// panics) and pushed the postfix write to POST-effects (`t++ + t` read the
	// stale value; EVM writes immediately).
	VariableDeclaration const* transientVar = nullptr;
	if (auto const* ident = dynamic_cast<Identifier const*>(&m_unaryOp.subExpression()))
		if (auto const* varDecl = dynamic_cast<VariableDeclaration const*>(
				ident->annotation().referencedDeclaration))
			if (varDecl->isStateVariable()
				&& varDecl->referenceLocation() == VariableDeclaration::Location::Transient
				&& m_ctx.transientStorage
				&& m_ctx.transientStorage->isTransient(*varDecl))
				transientVar = varDecl;

	// NB: raw cast, NOT SolIntType::fromSol — this path historically does not unwrap
	// UDVTs here; a UDVT operand is treated as non-int (isSigned=false). Hoisted once
	// and reused for signedBits/unsignedBits below.
	auto const* opInt = dynamic_cast<IntegerType const*>(
		m_unaryOp.subExpression().annotation().type);
	bool isSigned = opInt && opInt->isSigned();

	if (dynamic_cast<awst::BoxValueExpression const*>(_operand.get()))
		_operand = builder::StorageMapper::makeStateGetWithDefault(_operand, _operand->wtype, m_loc);
	else if (auto const* idx = dynamic_cast<awst::IndexExpression const*>(_operand.get()))
	{
		if (dynamic_cast<awst::BoxValueExpression const*>(idx->base.get()))
		{
			auto* nativeType = m_ctx.typeMapper.map(m_unaryOp.subExpression().annotation().type);
			if (_operand->wtype != nativeType)
				_operand = awst::makeARC4Decode(_operand, nativeType, m_loc);
		}
	}

	unsigned signedBits = (opInt && isSigned) ? opInt->numBits() : 0;
	unsigned unsignedBits = (opInt && !isSigned) ? opInt->numBits() : 0;

	// All width/sign/checked/wrap rules for `base ± 1` live in eb::buildIncDec
	// (BigUIntMathHelpers), shared with the transient path below.
	auto makeNewValue = [&](std::shared_ptr<awst::Expression> base)
		-> std::shared_ptr<awst::Expression>
	{
		return eb::buildIncDec(m_ctx, m_scope.isUnchecked(), isInc,
			signedBits, unsignedBits, std::move(base), m_loc);
	};

	if (transientVar)
	{
		if (isPrefix)
		{
			// Pin the new value, write it, return the pin (mirrors the
			// struct-field prefix shape; cheaper and safer than re-reading).
			std::string pName = "__tpre_" + std::to_string(awst::NameGen::next("SolUnaryOperation.transientPreCounter"));
			auto nv = makeNewValue(_operand);
			auto* nvType = nv->wtype;
			m_ctx.preEffects().push_back(awst::makeAssignmentStatement(
				awst::makeVarExpression(pName, nvType, m_loc), std::move(nv), m_loc));
			if (auto writeStmt = m_ctx.transientStorage->buildWrite(*transientVar,
					awst::makeVarExpression(pName, nvType, m_loc), m_loc))
				m_ctx.preEffects().push_back(std::move(writeStmt));
			return awst::makeVarExpression(pName, nvType, m_loc);
		}
		// Postfix: save the old value, then write the increment as a
		// PRE-effect so a later read in the same statement observes it
		// (`t++ + t`) — matches the general postfix path below.
		std::string tempName = "__postinc_" + std::to_string(awst::NameGen::next("SolUnaryOperation.postIncCounter"));
		auto tempVar = awst::makeVarExpression(tempName, _operand->wtype, m_loc);
		m_ctx.preEffects().push_back(awst::makeAssignmentStatement(tempVar, _operand, m_loc));
		if (auto writeStmt = m_ctx.transientStorage->buildWrite(*transientVar,
				makeNewValue(tempVar), m_loc))
			m_ctx.preEffects().push_back(std::move(writeStmt));
		return tempVar;
	}


	// Derive write target from the ALREADY-BUILT operand — rebuilding re-runs
	// side-effecting indexes (verified: `arr[i++]++` gave i==2 on rebuild).
	// Unwrap StateGet (read wrapper) so assignment lands on BoxValue/AppState;
	// peel ARC4Decode so write hits encoded slot (paired with ARC4Encode below).
	auto makeWriteTarget = [&](std::shared_ptr<awst::Expression> target)
		-> std::shared_ptr<awst::Expression>
	{
		target = awst::unwrapStateGet(std::move(target));
		if (auto const* decodeExpr = dynamic_cast<awst::ARC4Decode const*>(target.get()))
			target = decodeExpr->value;
		return target;
	};

	// Boxed STRUCT FIELD inc/dec: a bare `field := v` assignment is rejected by puya once the struct is
	// boxed (any 2nd function keeps the struct in a box). Rebuild the struct copy-on-write
	// (box := struct-with-field-replaced) like SolAssignment's compound path.
	auto structFieldTypeOf = [&](std::shared_ptr<awst::Expression> const& writeTarget)
		-> awst::ARC4Struct const*
	{
		auto const* fieldExpr = dynamic_cast<awst::FieldExpression const*>(writeTarget.get());
		if (!fieldExpr) return nullptr;
		auto const* structType = dynamic_cast<awst::ARC4Struct const*>(fieldExpr->base->wtype);
		if (!structType)
			if (auto const* sg = dynamic_cast<awst::StateGet const*>(fieldExpr->base.get()))
				structType = dynamic_cast<awst::ARC4Struct const*>(sg->field->wtype);
		return structType;
	};
	// Shared struct-field COW store (AssignmentHelper) — including the
	// lazy-root-box ensure this path used to skip (`n[k][i].f++` on a fresh
	// mapping key died on "no such box" where `= v` worked).
	auto buildStructFieldCowWrite = [&](std::shared_ptr<awst::Expression> const& writeTarget,
		awst::ARC4Struct const* structType,
		std::shared_ptr<awst::Expression> nativeFieldValue) -> std::shared_ptr<awst::Statement>
	{
		auto const* fieldExpr = dynamic_cast<awst::FieldExpression const*>(writeTarget.get());
		auto store = eb::AssignmentHelper::buildStructFieldCowStore(
			m_ctx, fieldExpr, structType, std::move(nativeFieldValue), m_loc);
		return awst::makeAssignmentStatement(
			std::move(store.target), std::move(store.value), m_loc);
	};

	if (isPrefix)
	{
		auto writeTarget = makeWriteTarget(_operand);
		if (auto const* structType = structFieldTypeOf(writeTarget))
		{
			std::string pName = "__sfpre_" + std::to_string(awst::NameGen::next("SolUnaryOperation.sfPreCounter"));
			auto nv = makeNewValue(_operand);
			auto* nvType = nv->wtype;
			auto nvVar = awst::makeVarExpression(pName, nvType, m_loc);
			m_ctx.preEffects().push_back(awst::makeAssignmentStatement(nvVar, std::move(nv), m_loc));
			m_ctx.preEffects().push_back(buildStructFieldCowWrite(writeTarget, structType,
				awst::makeVarExpression(pName, nvType, m_loc)));
			return awst::makeVarExpression(pName, nvType, m_loc);
		}
		auto store = eb::AssignmentHelper::preparePlainStore(
			m_ctx, std::move(writeTarget), makeNewValue(_operand), m_loc);
		return awst::makeAssignmentExpression(
			std::move(store.target), std::move(store.value), m_loc, _operand->wtype);
	}
	else
	{
		// Post-inc: save old value, emit the write as a pre-effect so
		// `a++ + a` reads updated `a`; pending only fires at statement end.
		std::string tempName = "__postinc_" + std::to_string(awst::NameGen::next("SolUnaryOperation.postIncCounter"));

		auto tempVar = awst::makeVarExpression(tempName, _operand->wtype, m_loc);

		auto saveStmt = awst::makeAssignmentStatement(tempVar, _operand, m_loc);
		m_ctx.preEffects().push_back(std::move(saveStmt));

		auto writeTarget = makeWriteTarget(_operand);
		if (auto const* structType = structFieldTypeOf(writeTarget))
		{
			m_ctx.preEffects().push_back(buildStructFieldCowWrite(writeTarget, structType,
				makeNewValue(tempVar)));
			return tempVar;
		}
		auto store = eb::AssignmentHelper::preparePlainStore(
			m_ctx, std::move(writeTarget), makeNewValue(tempVar), m_loc);
		m_ctx.preEffects().push_back(awst::makeAssignmentStatement(
			std::move(store.target), std::move(store.value), m_loc));

		return tempVar;
	}
}

bool SolUnaryOperation::clearMultiBoxElement(
	VariableDeclaration const& _var,
	awst::WType const* _arrWtype,
	std::shared_ptr<awst::Expression> const& _index)
{
	using ::puyasol::builder::StorageMapper;

	unsigned const elemSize = StorageMapper::arc4StaticArrayElementSize(_arrWtype);
	unsigned const elemsPerBox = StorageMapper::elementsPerBox(_arrWtype);
	if (elemSize == 0 || elemsPerBox == 0)
		return false;
	auto const* sa = dynamic_cast<awst::ARC4StaticArray const*>(_arrWtype);
	if (!sa)
		return false;

	auto page = StorageMapper::arrayPageForIndex(
		m_ctx.storageMapper.physicalBindingFor(_var).name,
		_arrWtype, _index, m_ctx.preEffects(), m_loc);

	// The cleared element is its ARC-4 default, which for a fixed-width element
	// is elemSize zero bytes — not necessarily all-zero for shapes carrying head
	// offsets, so take the encoding rather than assuming.
	auto encoded = builder::arc4DefaultEncoding(sa->elementType());
	if (!encoded || encoded->size() != elemSize)
		throw SizeError("multi-box array element has no supported default encoding");

	auto replace = awst::makeIntrinsicCall("box_replace", awst::WType::voidType(), m_loc);
	replace->stackArgs.push_back(page.key);
	replace->stackArgs.push_back(page.offset);
	replace->stackArgs.push_back(awst::makeBytesConstant(std::move(*encoded), m_loc));
	auto exists = awst::makeTupleItem(StorageMapper::makeBoxLenTuple(
		m_ctx.typeMapper, page.key, m_loc), 1, awst::WType::boolType(), m_loc);
	auto body = awst::makeBlock(m_loc);
	body->body.push_back(awst::makeExpressionStatement(std::move(replace), m_loc));
	m_ctx.postEffects().push_back(awst::makeIfElse(std::move(exists), std::move(body), nullptr, m_loc));
	return true;
}

std::shared_ptr<awst::Expression> SolUnaryOperation::handleDelete(
	std::shared_ptr<awst::Expression> _operand)
{
	// Transient state var delete: write zero via TransientStorage (read expr not an lvalue).
	if (auto const* ident = dynamic_cast<Identifier const*>(&m_unaryOp.subExpression()))
	{
		if (auto const* varDecl = dynamic_cast<VariableDeclaration const*>(
				ident->annotation().referencedDeclaration))
		{
			if (varDecl->isStateVariable()
				&& varDecl->referenceLocation() == VariableDeclaration::Location::Transient
				&& m_ctx.transientStorage
				&& m_ctx.transientStorage->isTransient(*varDecl))
			{
				auto const* info = m_ctx.transientStorage->getVarInfoById(varDecl->id());
				auto* wt = info ? info->wtype : m_ctx.typeMapper.map(varDecl->type());
				auto zero = builder::StorageMapper::makeDefaultValue(wt, m_loc);
				if (auto stmt = m_ctx.transientStorage->buildWrite(
						*varDecl, std::move(zero), m_loc))
					m_ctx.postEffects().push_back(std::move(stmt));
				m_scope.eraseFuncPtrTarget(varDecl->id());
				return _operand;
			}
			if (varDecl->isStateVariable() && !varDecl->isConstant() && !varDecl->immutable()
				&& !m_ctx.typeMapper.profile().evmStorageLayout)
			{
				auto const* type = m_ctx.typeMapper.map(varDecl->type());
				if (StorageMapper::isMultiBoxArray(type))
				{
					auto count = StorageMapper::numBoxesForArray(type);
					if (count > 4096) throw SizeError("multi-box delete exceeds the 4096-page unroll capacity");
					auto name = m_ctx.storageMapper.physicalBindingFor(*varDecl).name;
					for (unsigned page = 0; page < count; ++page)
					{
						auto key = awst::makeConcat(awst::makeUtf8BytesConstant(name, m_loc),
							awst::makeItob(awst::makeIntegerConstant(page, m_loc), m_loc), m_loc);
						key->wtype = awst::WType::boxKeyType();
						m_ctx.queuePostExpression(awst::makeStateDelete(
							awst::makeBoxValueExpression(std::move(key), awst::WType::bytesType(), m_loc), m_loc), m_loc);
					}
					return _operand;
				}
			}
		}
	}

	// `delete m[i]` on a MULTI-BOX array: the element lives at a page/offset, so
	// there is no single lvalue to assign a default to and puya rejected the
	// plain IndexExpression with "unsupported assignment target". Zero the
	// element's slice in place instead. Reached once struct elements became
	// eligible for multi-box paging; before that such arrays fell back to a
	// single box, whose delete the generic path below already handles.
	//
	// The index comes off the ALREADY-BUILT operand, never from rebuilding the
	// solc node: `delete m[f()]` would otherwise call f() twice.
	if (auto const* index = dynamic_cast<IndexAccess const*>(&m_unaryOp.subExpression()))
		if (auto const* ident = dynamic_cast<Identifier const*>(&index->baseExpression()))
			if (auto const* varDecl = dynamic_cast<VariableDeclaration const*>(
					ident->annotation().referencedDeclaration);
				varDecl && varDecl->isStateVariable()
				&& !varDecl->isConstant() && !varDecl->immutable())
			{
				auto const* arrWtype = m_ctx.typeMapper.map(varDecl->type());
				auto indexed = _operand;
				if (auto const* decode = dynamic_cast<awst::ARC4Decode const*>(indexed.get()))
					indexed = decode->value;
				auto const* builtIndex = dynamic_cast<awst::IndexExpression const*>(indexed.get());
				if (builder::StorageMapper::isMultiBoxArray(arrWtype) && builtIndex
					&& clearMultiBoxElement(*varDecl, arrWtype, builtIndex->index))
					return _operand;
			}

	// Use already-built operand: rebuilding re-runs side effects (verified: `delete m[f()]` ran f() twice).
	auto target = _operand;

	// Clear function pointer tracking on delete.
	if (auto const* ident = dynamic_cast<Identifier const*>(&m_unaryOp.subExpression()))
	{
		if (auto const* varDecl = dynamic_cast<VariableDeclaration const*>(
				ident->annotation().referencedDeclaration))
			m_scope.eraseFuncPtrTarget(varDecl->id());
	}

	if (auto const* boxExpr = dynamic_cast<awst::BoxValueExpression const*>(target.get()))
	{
		// Top-level dynamic box (eager box_create in __postInit): emit `a = default`
		// not box_del — subsequent reads assert box exists. Mappings use box_del (lazy).
		if (builder::StorageMapper::isTopLevelDynamicBox(boxExpr))
		{
			auto def = builder::StorageMapper::makeDefaultValue(boxExpr->wtype, m_loc);
			auto put = awst::makeAssignmentExpression(target, std::move(def), m_loc, boxExpr->wtype);
			m_ctx.queuePostExpression(std::move(put), m_loc);
			return _operand;
		}
		auto stateDelete = awst::makeStateDelete(target, m_loc);
		m_ctx.queuePostExpression(std::move(stateDelete), m_loc);
		return _operand;
	}

	if (auto const* decodeExpr = dynamic_cast<awst::ARC4Decode const*>(target.get()))
		target = decodeExpr->value;

	// ARC4Struct field: COW with the field zeroed — the shared store
	// (AssignmentHelper). The old inline copy wrote to the single-stripped
	// base: no chain COW, no writable-target strip (puya rejected
	// `delete n[k][1].f` — StateGet kept inside the index target), no
	// lazy-root-box ensure.
	if (auto const* fieldExpr = dynamic_cast<awst::FieldExpression const*>(target.get()))
	{
		auto const* arc4StructType = dynamic_cast<awst::ARC4Struct const*>(fieldExpr->base->wtype);
		if (arc4StructType)
		{
			awst::WType const* arc4FieldType = awst::structFieldType(
				arc4StructType, fieldExpr->name);
			auto zeroVal = builder::StorageMapper::makeDefaultValue(
				arc4FieldType ? arc4FieldType : fieldExpr->wtype, m_loc);
			auto store = eb::AssignmentHelper::buildStructFieldCowStore(
				m_ctx, fieldExpr, arc4StructType, std::move(zeroVal), m_loc);
			m_ctx.queuePostEffect(awst::makeAssignmentStatement(
				std::move(store.target), std::move(store.value), m_loc));
			return _operand;
		}
	}

	auto defaultVal = builder::StorageMapper::makeDefaultValue(target->wtype, m_loc);
	target = awst::unwrapStateGet(std::move(target));

	// Slot-based storage delete: clear biguint-slot range.
	if (dynamic_cast<awst::BigUIntBinaryOperation const*>(target.get())
		&& target->wtype == awst::WType::biguintType())
	{
		auto const* subExprType = m_unaryOp.subExpression().annotation().type;
		auto const* arrType = subExprType ? dynamic_cast<ArrayType const*>(subExprType) : nullptr;
		unsigned slotCount = 1;
		if (arrType && !arrType->isDynamicallySized())
		{
			// EVM clears the array's SLOT footprint — packed arrays span fewer
			// slots than elements, multislot-element arrays span more.
			auto slots = arrType->storageSize();
			if (slots > 4096)
			{
				Logger::instance().error(
					"slot-handle delete of " + slots.str()
					+ " slots exceeds the unroll cap (4096)", m_loc);
				slots = 1;
			}
			slotCount = static_cast<unsigned>(slots);
		}

		for (unsigned j = 0; j < slotCount; ++j)
		{
			auto jConst = awst::makeIntegerConstant(j, m_loc, awst::WType::biguintType());

			auto slotJ = awst::makeBigUIntBinOp(target, awst::BigUIntBinaryOperator::Add, std::move(jConst), m_loc);

			auto btoi = builder::StorageMapper::biguintSlotToBtoi(slotJ, m_loc);

			auto zeroVal = awst::makeZero(m_loc, awst::WType::biguintType());

			auto call = awst::makeSubroutineCall(awst::SubroutineID{"__puyasol___storage_write"}, awst::WType::voidType(), m_loc);
			awst::pushCallArg(call->args, "__slot", std::move(btoi));
			awst::pushCallArg(call->args, "__value", std::move(zeroVal));

			m_ctx.queuePostExpression(std::move(call), m_loc);
		}
		return _operand;
	}

	{
		auto store = eb::AssignmentHelper::preparePlainStore(
			m_ctx, std::move(target), std::move(defaultVal), m_loc);
		m_ctx.queuePostEffect(awst::makeAssignmentStatement(
			std::move(store.target), std::move(store.value), m_loc));
	}
	return _operand;
}

std::shared_ptr<awst::Expression> SolUnaryOperation::handleEvmStorageIncDecDelete()
{
	auto const& sub = m_unaryOp.subExpression();
	auto const* solType = sub.annotation().type;
	if (m_unaryOp.getOperator() == Token::Delete
		&& solType && !solType->isValueType())
	{
		EvmSlotLowering low(m_ctx, m_scope, m_loc);
		auto addr = low.resolve(sub);
		if (!addr)
			return nullptr;
		addr->solType = solType;
		addr->wtype = m_ctx.typeMapper.map(solType);
		std::vector<std::shared_ptr<awst::Statement>> writes;
		if (low.clearAggregate(*addr, solType, writes))
		{
			for (auto& st: writes)
				m_ctx.queuePostEffect(std::move(st));
			return awst::makeZero(m_loc, awst::WType::biguintType());
		}
		return nullptr;
	}
	if (!solType || !solType->isValueType())
	{
		Logger::instance().error(
			"--evm-storage-layout: delete/++/-- on aggregate storage not yet "
			"supported", m_loc);
		return nullptr;
	}
	EvmSlotLowering low(m_ctx, m_scope, m_loc);
	auto addr = low.resolve(sub);
	if (!addr)
		return nullptr;

	// The slot (and any runtime byte offset) is referenced by the value read
	// AND the queued write — separate statements, so pin to temps (the
	// cross-statement idiom; SingleEvaluation would be unsound here).
	auto pinToTemp = [&](std::shared_ptr<awst::Expression>& _e, char const* _tag) {
		if (!_e || dynamic_cast<awst::VarExpression const*>(_e.get())
			|| dynamic_cast<awst::IntegerConstant const*>(_e.get()))
			return;
		std::string nm = std::string("__evm_") + _tag + "_"
			+ std::to_string(awst::NameGen::next("SolUnaryOperation.evmPin"));
		m_ctx.preEffects().push_back(awst::makeAssignmentStatement(
			awst::makeVarExpression(nm, _e->wtype, m_loc), _e, m_loc));
		_e = awst::makeVarExpression(nm, _e->wtype, m_loc);
	};
	pinToTemp(addr->slot, "slot");
	pinToTemp(addr->byteOffset, "off");

	Token op = m_unaryOp.getOperator();
	std::vector<std::shared_ptr<awst::Statement>> writes;
	if (op == Token::Delete)
	{
		std::shared_ptr<awst::Expression> zero;
		if (addr->wtype == awst::WType::accountType())
			zero = awst::makeAddressConstant(
				"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAY5HFKQ", m_loc);
		else if (addr->wtype == awst::WType::biguintType())
			zero = awst::makeZero(m_loc, awst::WType::biguintType());
		else if (addr->wtype == awst::WType::boolType())
			zero = awst::makeBoolConstant(false, m_loc, awst::WType::boolType());
		else if (addr->wtype == awst::WType::uint64Type())
			zero = awst::makeZero(m_loc);
		else if (auto const* bw = dynamic_cast<awst::BytesWType const*>(addr->wtype);
			bw && bw->length().has_value())
			zero = awst::makeBytesConstant(
				std::vector<uint8_t>(static_cast<size_t>(*bw->length()), 0), m_loc,
				awst::BytesEncoding::Base16, addr->wtype);
		else
			zero = builder::StorageMapper::makeDefaultValue(addr->wtype, m_loc);
		low.writeValue(*addr, std::move(zero), writes);
		for (auto& st: writes)
			m_ctx.queuePostEffect(std::move(st));
		return awst::makeZero(m_loc, awst::WType::biguintType());
	}

	bool isInc = (op == Token::Inc);
	bool isPrefix = m_unaryOp.isPrefixOperation();
	auto current = low.readValue(*addr);
	// Postfix returns the OLD value: pin it BEFORE the write. Returning a
	// fresh read with the write queued post-statement looked equivalent, but
	// `return st.a++` hoists pending statements ahead of the return, so the
	// "old" read observed the incremented value.
	std::shared_ptr<awst::Expression> oldPin;
	if (!isPrefix)
	{
		std::string onm = "__evm_old_"
			+ std::to_string(awst::NameGen::next("SolUnaryOperation.evmOld"));
		m_ctx.preEffects().push_back(awst::makeAssignmentStatement(
			awst::makeVarExpression(onm, current->wtype, m_loc), current, m_loc));
		oldPin = awst::makeVarExpression(onm, current->wtype, m_loc);
		current = awst::makeVarExpression(onm, current->wtype, m_loc);
	}
	auto one = awst::makeIntegerConstant("1", m_loc,
		addr->wtype == awst::WType::biguintType()
			? awst::WType::biguintType() : awst::WType::uint64Type());
	auto newValue = eb::AssignmentHelper::computeCompoundOrFallback(
		m_ctx, isInc ? Token::AssignAdd : Token::AssignSub,
		isInc ? Token::Add : Token::Sub,
		solType, current, std::move(one), current->wtype, m_loc);
	if (newValue && addr->wtype && newValue->wtype != addr->wtype
		&& (newValue->wtype == awst::WType::uint64Type()
			|| newValue->wtype == awst::WType::biguintType())
		&& (addr->wtype == awst::WType::uint64Type()
			|| addr->wtype == awst::WType::biguintType()))
		newValue = builder::TypeCoercion::implicitNumericCast(
			std::move(newValue), addr->wtype, m_loc);
	if (!newValue)
		return nullptr;
	low.writeValue(*addr, std::move(newValue), writes);
	// Both forms write via pre-effects (after the postfix old-pin above, so
	// ordering is pin -> write). Prefix returns a fresh
	// read (sees the new value); postfix returns the pinned old value.
	for (auto& st: writes)
		m_ctx.preEffects().push_back(std::move(st));
	if (isPrefix)
		return low.readValue(*addr);
	return oldPin;
}

std::shared_ptr<awst::Expression> SolUnaryOperation::toAwst()
{
	if (auto const* function = *m_unaryOp.annotation().userDefinedFunction)
		return eb::CallResolver::buildOperatorCall(m_ctx, *function,
			{&m_unaryOp.subExpression()}, m_loc);

	// The canonical constant path (fable-review item 1): solc folded the WHOLE
	// expression (non-fractional rational annotation, e.g. `-2`, `~5`) → emit
	// its value directly; never fold built AWST downstream. Runtime-typed
	// expressions (incl. AWST-constant operands like a lowered type(intN).min)
	// take the full checked paths below.
	if (auto folded = builder::SolcConstFold::foldAnnotated(m_unaryOp, m_ctx.typeMapper, m_loc))
		return folded;
	// intN-typed constant expression (e.g. `-M` over a constant variable) —
	// foldTyped's in-range guard keeps `-intN.min` on the checked path.
	if (auto folded = builder::SolcConstFold::foldTyped(m_unaryOp, m_loc))
		return folded;

	// --evm-storage-layout: ++/--/delete on storage state refs must not build
	// the operand (the sub-expression is the lvalue, not a value).
	{
		Token op = m_unaryOp.getOperator();
		if ((op == Token::Inc || op == Token::Dec || op == Token::Delete)
			&& m_ctx.typeMapper.profile().evmStorageLayout
			&& EvmSlotLowering::isStorageStateRef(m_unaryOp.subExpression()))
			return handleEvmStorageIncDecDelete();
	}

	auto operand = buildExpr(m_unaryOp.subExpression());

	// Try sol-eb builder dispatch for Not/Sub/BitNot
	{
		eb::BuilderUnaryOp builderOp;
		bool hasUnaryOp = true;
		switch (m_unaryOp.getOperator())
		{
		case Token::Not: builderOp = eb::BuilderUnaryOp::LogicalNot; break;
		case Token::Sub: builderOp = eb::BuilderUnaryOp::Negative; break;
		case Token::BitNot: builderOp = eb::BuilderUnaryOp::BitInvert; break;
		default: hasUnaryOp = false; break;
		}
		if (hasUnaryOp)
		{
			// Checked negate references the operand in the overflow assert AND
			// the negation (verified: -g() ran g 3×); biguint ~ references it
			// in len + extract. Pin once — the same wrapped node also feeds the
			// handleNot/handleNegate/handleBitNot fallbacks below. Inc/Dec/
			// Delete never enter this block (their operand must stay an
			// lvalue-shaped tree).
			operand = awst::makeEvalOnce(std::move(operand), m_loc);
			auto* solType = m_unaryOp.subExpression().annotation().type;
			auto builder = m_ctx.builderForInstance(solType, operand);
			if (builder)
			{
				auto result = builder->unary_op(builderOp, m_loc);
				if (result)
					return result->resolve();
			}
		}
	}

	switch (m_unaryOp.getOperator())
	{
	case Token::Not:    return handleNot(std::move(operand));
	case Token::Sub:    return handleNegate(std::move(operand));
	case Token::BitNot: return handleBitNot(std::move(operand));
	case Token::Inc:
	case Token::Dec:    return handleIncDec(std::move(operand));
	case Token::Delete: return handleDelete(std::move(operand));
	default:            return operand;
	}
}

} // namespace puyasol::builder::sol_ast
