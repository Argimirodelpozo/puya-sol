/// @file SolFieldAccess.cpp
/// Struct field access (ARC4Struct, WTuple).
/// Migrated from MemberAccessBuilder.cpp lines 712-754.

#include "builder/sol-ast/members/SolFieldAccess.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/sol-types/TypeCoercion.h"
#include "builder/sol-types/SolIntType.h"
#include "builder/storage/SlotHandleAccess.h"
#include "builder/sol-ast/EvmSlotLowering.h"
#include "builder/storage/EvmLayoutMode.h"
#include "Logger.h"

namespace puyasol::builder::sol_ast
{

std::shared_ptr<awst::Expression> SolFieldAccess::toAwst()
{
	std::string member = memberName();

	// --evm-storage-layout: struct-field reads rooted at a persistent state
	// var resolve to their EVM word address (writes intercept in SolAssignment).
	if (m_ctx.typeMapper.profile().evmStorageLayout
		&& EvmSlotLowering::isStorageStateRef(m_memberAccess))
	{
		EvmSlotLowering low(m_ctx, m_scope, m_loc);
		auto addr = low.resolve(m_memberAccess);
		if (!addr)
			return nullptr;
		auto const* resType = m_memberAccess.annotation().type;
		return low.readAny(*addr, resType);
	}

	// Explicit `.slot` handles use the same recursive address/type dispatch.
	// Peeling the complete member/index chain here avoids duplicating packed,
	// scalar, struct, and nested-array cases in this expression builder.
	auto const* slotResultType = m_memberAccess.annotation().type;
	if (!m_memberAccess.annotation().willBeWrittenTo
		&& EvmSlotLowering::isSlotHandleRef(m_memberAccess, m_ctx, m_scope))
	{
		EvmSlotLowering low(m_ctx, m_scope, m_loc);
		auto addr = low.resolve(m_memberAccess);
		return addr ? low.readAny(*addr, slotResultType) : nullptr;
	}

	// Field read through a LIVE static calldata pointer: `assembly { s := s2 }
	// r = s.x;` must read the word the (repointed) pointer designates inside
	// __cd_blob — solc's calldataOffsetOfMember gives the field's byte offset
	// within the struct's calldata encoding. Only for rvalue reads of int-mapped
	// fields; anything else falls through to the decoded-value path.
	if (!m_memberAccess.annotation().willBeWrittenTo)
		if (auto const* baseId = dynamic_cast<solidity::frontend::Identifier const*>(&baseExpression()))
			if (auto const* vd = dynamic_cast<solidity::frontend::VariableDeclaration const*>(
					baseId->annotation().referencedDeclaration))
				if (vd->referenceLocation()
						== solidity::frontend::VariableDeclaration::Location::CallData)
					if (auto* live = m_ctx.currentScope
							? m_ctx.currentScope->liveCalldataPointers() : nullptr)
						if (live->count(vd->name()))
							if (auto const* st = dynamic_cast<solidity::frontend::StructType const*>(
									vd->type()))
							{
								auto const* fieldW =
									m_ctx.typeMapper.map(m_memberAccess.annotation().type);
								if (fieldW == awst::WType::biguintType()
									|| fieldW == awst::WType::uint64Type())
								{
									unsigned fieldOff = st->calldataOffsetOfMember(member);
									auto off64 = builder::TypeCoercion::implicitNumericCast(
										awst::makeVarExpression("__cd_off_" + vd->name(),
											awst::WType::biguintType(), m_loc),
										awst::WType::uint64Type(), m_loc);
									auto pos = awst::makeUInt64BinOp(std::move(off64),
										awst::UInt64BinaryOperator::Add,
										awst::makeIntegerConstant(
											static_cast<uint64_t>(fieldOff), m_loc), m_loc);
									if (fieldW == awst::WType::uint64Type())
									{
										// low 8 bytes of the 32-byte word
										auto pos8 = awst::makeUInt64BinOp(std::move(pos),
											awst::UInt64BinaryOperator::Add,
											awst::makeIntegerConstant(uint64_t(24), m_loc), m_loc);
										return awst::makeBtoi(awst::makeExtract3(
											awst::makeVarExpression("__cd_blob",
												awst::WType::bytesType(), m_loc),
											std::move(pos8),
											awst::makeIntegerConstant(uint64_t(8), m_loc), m_loc), m_loc);
									}
									return awst::makeAsBiguint(awst::makeExtract3(
										awst::makeVarExpression("__cd_blob",
											awst::WType::bytesType(), m_loc),
										std::move(pos),
										awst::makeIntegerConstant(uint64_t(32), m_loc), m_loc), m_loc);
								}
							}

	auto base = buildExpr(baseExpression());

	if (base->wtype && base->wtype->kind() == awst::WTypeKind::ARC4Struct)
	{
		auto const* structType = static_cast<awst::ARC4Struct const*>(base->wtype);
		awst::WType const* arc4FieldType = awst::structFieldType(structType, member);

		auto field = awst::makeFieldExpression(std::move(base), member, arc4FieldType ? arc4FieldType
			: m_ctx.typeMapper.map(m_memberAccess.annotation().type), m_loc);

		auto* nativeType = m_ctx.typeMapper.map(m_memberAccess.annotation().type);
		if (arc4FieldType && !awst::structurallyEquivalent(arc4FieldType, nativeType))
		{
			std::shared_ptr<awst::Expression> decode =
				awst::makeARC4Decode(std::move(field), nativeType, m_loc);
			// Signed sub-word field (arc4.intN, N<64): decode yields raw N-bit
			// value (-60 int24 → +16777156). Sign-extend to 64-bit two's-complement.
			// ONLY for rvalue reads: assignment target (willBeWrittenTo) must see
			// the bare ARC4Decode/FieldExpression for the write-back path
			// (SolAssignment::tryStructOrNamedTupleFieldAssignment).
			if (!m_memberAccess.annotation().willBeWrittenTo)
			{
				if (auto const* fieldInt = dynamic_cast<solidity::frontend::IntegerType const*>(
						m_memberAccess.annotation().type))
					if (fieldInt->isSigned() && fieldInt->numBits() < 64
						&& nativeType == awst::WType::uint64Type())
						decode = TypeCoercion::signExtendToUint64(
							std::move(decode), fieldInt->numBits(), m_loc);
				// 64<N<256 signed fields (e.g. int128): sign-extend to canonical 256-bit
				// two's-complement. Same class as int128[] array-element + transient fixes.
				// No-op for unsigned / int256 / <=64-bit.
				decode = TypeCoercion::signExtendSignedElement(
					std::move(decode), m_memberAccess.annotation().type, m_loc);
			}
			return decode;
		}
		return field;
	}

	if (base->wtype && base->wtype->kind() == awst::WTypeKind::WTuple)
	{
		auto e = awst::makeFieldExpression(std::move(base), member, m_ctx.typeMapper.map(m_memberAccess.annotation().type), m_loc);
		return e;
	}

	return nullptr;
}

} // namespace puyasol::builder::sol_ast
