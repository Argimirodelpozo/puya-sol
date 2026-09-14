/// @file SolFieldAccess.cpp
/// Struct field access (ARC4Struct, WTuple).

#include "builder/ast/members/SolFieldAccess.h"
#include "builder/eb/MappingPrefix.h"
#include "builder/solc/SolcFacts.h"
#include "builder/codec/EvmValueCodec.h"
#include "builder/solc/StorageRefPointer.h"
#include "builder/types/TypeMapper.h"
#include "builder/types/TypeCoercion.h"
#include "builder/storage/StorageMapper.h"
#include "builder/storage/slot/EvmSlotLowering.h"
#include "builder/target/EvmLayoutMode.h"
#include "Logger.h"

namespace puyasol::builder::sol_ast
{

std::shared_ptr<awst::Expression> SolFieldAccess::toAwst()
{
	std::string member = memberName();

	using namespace solidity::frontend;
	if ((m_ctx.typeMapper.profile().evmStorageLayout && EvmSlotLowering::isStorageStateRef(m_memberAccess))
		|| (!m_memberAccess.annotation().willBeWrittenTo
			&& EvmSlotLowering::isSlotHandleRef(m_memberAccess, m_ctx, m_scope)))
	{
		EvmSlotLowering low(m_ctx, m_scope, m_loc);
		auto addr = low.resolve(m_memberAccess);
		return addr ? low.readAny(*addr, m_solType) : nullptr;
	}

	// Live calldata struct fields use solc offsets and the same validated word
	// decoder as ABI input. Never fall back to a stale decoded parameter.
	if (auto const* id = dynamic_cast<Identifier const*>(&baseExpression()))
		if (auto const* declaration = dynamic_cast<VariableDeclaration const*>(id->annotation().referencedDeclaration);
			declaration && declaration->referenceLocation() == VariableDeclaration::Location::CallData)
			if (auto const* live = m_scope.liveCalldataPointers();
				live && live->contains(m_scope.awstVarName(*declaration)))
			{
				auto const* structure = dynamic_cast<StructType const*>(declaration->type());
				if (!structure || !codec::isWordType(m_solType))
					throw SizeError("aggregate member reads through a live calldata pointer are not supported");
				auto name = m_scope.awstVarName(*declaration);
				auto position = m_ctx.emitSequencedOperand({}, awst::makeBigUIntBinOp(
					awst::makeBigUIntBinOp(awst::makeVarExpression("__cd_off_" + name,
						awst::WType::biguintType(), m_loc), awst::BigUIntBinaryOperator::Add,
						awst::makeIntegerConstant(structure->calldataOffsetOfMember(member),
							m_loc, awst::WType::biguintType()), m_loc), awst::BigUIntBinaryOperator::Mod,
					makePow256(m_loc), m_loc), true, m_loc);
				auto blob = awst::makeVarExpression("__cd_blob", awst::WType::bytesType(), m_loc);
				auto length = awst::makeLen(blob, m_loc);
				// calldataload zero-pads, even for a full-width out-of-range pointer.
				auto offset = awst::makeConditional(awst::makeNumericCompare(position,
					awst::NumericComparison::Lt, TypeCoercion::implicitNumericCast(
						length, awst::WType::biguintType(), m_loc), m_loc),
					TypeCoercion::implicitNumericCast(position, awst::WType::uint64Type(), m_loc),
					length, awst::WType::uint64Type(), m_loc);
				auto word = awst::makeExtract3(awst::makeConcat(blob, awst::makeBzero(32, m_loc), m_loc),
					std::move(offset), awst::makeIntegerConstant(32, m_loc), m_loc);
				return codec::valueFromEvmWord(m_ctx.typeMapper, m_solType, std::move(word),
					m_loc, m_ctx.preEffects(), codec::PaddingPolicy::Validate);
			}

	if (dynamic_cast<solidity::frontend::MappingType const*>(m_memberAccess.annotation().type))
	{
		auto holder = resolveStorageHolder(m_ctx, m_scope, m_memberAccess, m_loc);
		if (!holder.key) throw SizeError("mapping field requires a resolved storage holder");
		return awst::makeAsBytes(std::move(holder.key), m_loc);
	}

	std::shared_ptr<awst::Expression> base;
	// `_p(id).n` where `_p` returns a mapping-value struct reference: the callee
	// hands back the entry's BOX KEY (bytes-keyed storage-ref return). Wrap it
	// as the box value `ps[id]` lowers to, so member reads and writes address
	// the entry — bare bytes had no members (reads yielded nothing, writes
	// were rejected as constants).
	auto const* call = dynamic_cast<FunctionCall const*>(&baseExpression());
	auto const* callee = call ? SolcFacts::resolveInternalCall(*call, m_ctx.currentContract) : nullptr;
	if (callee && !m_ctx.typeMapper.profile().evmStorageLayout
		&& builder::storageRefReturnIsBytesKeyed(callee, m_ctx.typeMapper.analysis()))
	{
		auto receiver = m_ctx.lower(baseExpression(), false);
		auto key = m_ctx.emitSequencedOperand(
			std::move(receiver.effects), std::move(receiver.value), true, m_loc);
		auto const* wt = m_ctx.typeMapper.map(baseExpression().annotation().type);
		base = awst::makeBoxValueExpression(
			awst::makeReinterpretCast(std::move(key), awst::WType::boxKeyType(), m_loc), wt, m_loc);
		if (!m_memberAccess.annotation().willBeWrittenTo)
			base = StorageMapper::makeStateGetWithDefault(std::move(base), wt, m_loc);
	}
	if (!base)
	{
		// A storage reference returned by a call that is NOT box-keyed (a
		// top-level struct root) arrives as a value copy; writing through it
		// would be dropped, and puya rejects the call as an lvalue base with an
		// unreadable deserialization error. Fail loud with the working form.
		// Only user functions: a builtin such as `arr.push()` also arrives as a
		// FunctionCall and yields a real element reference.
		if (callee && m_memberAccess.annotation().willBeWrittenTo
			&& !m_ctx.typeMapper.profile().evmStorageLayout)
			if (auto const* refType = dynamic_cast<solidity::frontend::ReferenceType const*>(
					baseExpression().annotation().type);
				refType && refType->location() == solidity::frontend::DataLocation::Storage)
				Logger::instance().error(
					"cannot write through a storage reference returned by a call to a "
					"top-level state variable; bind it first (`T storage r = f(); r."
					+ member + " = …`)", m_loc);
		base = buildExpr(baseExpression());
	}
	if (!m_ctx.typeMapper.profile().evmStorageLayout
		&& transparentMappingWrapper(baseExpression().annotation().type))
		// The represented fields are already the inner struct's. Preserve the
		// place: an aggregate reinterpret cast is neither valid AWST nor an lvalue.
		return base;

	if (base->wtype && base->wtype->kind() == awst::WTypeKind::ARC4Struct)
	{
		auto const* structType = static_cast<awst::ARC4Struct const*>(base->wtype);
		awst::WType const* arc4FieldType = awst::structFieldType(structType, member);

		std::shared_ptr<awst::Expression> field = awst::makeFieldExpression(std::move(base), member, arc4FieldType ? arc4FieldType
			: m_ctx.typeMapper.map(m_memberAccess.annotation().type), m_loc);
		if (!m_memberAccess.annotation().willBeWrittenTo
			&& m_memberAccess.annotation().type->isValueType())
			field = StorageMapper::makePartialBoxReadWithDefault(
				m_ctx.typeMapper, std::move(field), m_ctx.preEffects(), m_loc);

		if (!m_memberAccess.annotation().willBeWrittenTo)
			return codec::valueFromArc4(m_ctx.typeMapper, m_solType, std::move(field), m_loc);
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
