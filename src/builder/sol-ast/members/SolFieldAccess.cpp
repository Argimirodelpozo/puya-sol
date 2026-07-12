/// @file SolFieldAccess.cpp
/// Struct field access (ARC4Struct, WTuple).
/// Migrated from MemberAccessBuilder.cpp lines 712-754.

#include "builder/sol-ast/members/SolFieldAccess.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/sol-types/TypeCoercion.h"
#include "builder/sol-types/SolIntType.h"
#include "builder/storage/SlotHandleAccess.h"

namespace puyasol::builder::sol_ast
{

std::shared_ptr<awst::Expression> SolFieldAccess::toAwst()
{
	std::string member = memberName();

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

	// Struct field through a SLOT HANDLE: base is an EVM slot number bound via
	// asm `.slot :=`. Handle LOCALS resolve with their declared struct wtype
	// through the generic identifier path — consult the registry as well.
	if (base && base->wtype != awst::WType::biguintType()
		&& !m_memberAccess.annotation().willBeWrittenTo)
		if (auto const* baseId = dynamic_cast<solidity::frontend::Identifier const*>(&baseExpression()))
			if (auto const* vd = dynamic_cast<solidity::frontend::VariableDeclaration const*>(
					baseId->annotation().referencedDeclaration))
				if (vd->isLocalVariable() && m_scope.findSlotStorageRef(vd->id()))
					base = awst::makeVarExpression(vd->name(), awst::WType::biguintType(), m_loc);
	// Writes intercept in SolAssignment (slot-handle field write).
	if (base && base->wtype == awst::WType::biguintType()
		&& !m_memberAccess.annotation().willBeWrittenTo)
		if (auto const* solStruct = dynamic_cast<solidity::frontend::StructType const*>(
				baseExpression().annotation().type))
		{
			auto const& off = solStruct->storageOffsetsOfMember(member);
			auto const* fieldSolType = m_memberAccess.annotation().type;
			unsigned size = fieldSolType ? fieldSolType->storageBytes() : 32;
			auto slotExpr = awst::makeBigUIntBinOp(std::move(base),
				awst::BigUIntBinaryOperator::Add,
				awst::makeIntegerConstant(off.first.str(), m_loc, awst::WType::biguintType()),
				m_loc);
			std::shared_ptr<awst::Expression> val;
			if (size == 32 && off.second == 0)
				val = builder::SlotHandleAccess::readSlot(std::move(slotExpr), m_loc);
			else
			{
				auto word = builder::SlotHandleAccess::readSlot(std::move(slotExpr), m_loc);
				auto wordB = awst::makeLeftPadToN(
					awst::makeAsBytes(std::move(word), m_loc), 32, m_loc);
				unsigned start = 32 - off.second - size;
				auto raw = awst::makeExtract(std::move(wordB),
					static_cast<int>(start), static_cast<int>(size), m_loc);
				val = awst::makeAsBiguint(std::move(raw), m_loc);
			}
			// canonical biguint; coerce to the native repr consumers expect
			if (auto it = builder::SolIntType::fromSol(fieldSolType);
				it && it->isSigned && it->bits < 256)
				val = builder::TypeCoercion::signExtendToUint256(std::move(val), it->bits, m_loc);
			auto const* nativeW = m_ctx.typeMapper.map(fieldSolType);
			if (nativeW == awst::WType::uint64Type())
				val = awst::makeBtoi(awst::makeExtractLastN(
					awst::makeZeroExtendToN(awst::makeAsBytes(std::move(val), m_loc), 32, m_loc),
					8, m_loc), m_loc);
			else if (nativeW == awst::WType::boolType())
				val = awst::makeNumericCompare(std::move(val), awst::NumericComparison::Ne,
					awst::makeIntegerConstant("0", m_loc, awst::WType::biguintType()), m_loc);
			return val;
		}

	if (base->wtype && base->wtype->kind() == awst::WTypeKind::ARC4Struct)
	{
		auto const* structType = static_cast<awst::ARC4Struct const*>(base->wtype);
		awst::WType const* arc4FieldType = nullptr;
		for (auto const& [fname, ftype]: structType->fields())
			if (fname == member)
			{
				arc4FieldType = ftype;
				break;
			}

		auto field = awst::makeFieldExpression(std::move(base), member, arc4FieldType ? arc4FieldType
			: m_ctx.typeMapper.map(m_memberAccess.annotation().type), m_loc);

		auto* nativeType = m_ctx.typeMapper.map(m_memberAccess.annotation().type);
		if (arc4FieldType && arc4FieldType != nativeType)
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
