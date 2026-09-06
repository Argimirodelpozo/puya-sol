#include "builder/storage/TransientStorage.h"
#include "builder/storage/SlotWordCodec.h"
#include "builder/sol-types/EncodedSize.h"
#include "awst/NameGen.h"

#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/TypeProvider.h>
#include <libsolidity/ast/Types.h>

namespace puyasol::builder
{

using namespace solidity::frontend;

void TransientStorage::collectVars(ContractDefinition const& _contract, TypeMapper& _typeMapper)
{
	m_scratchSlot = _typeMapper.profile().scratchLayout.transientSlot();
	m_vars.clear();
	m_varById.clear();
	m_totalSlots = 0;
	m_hasAddressShadow = false;

	for (auto const& [var, slot, offset]:
		TypeProvider::contract(_contract)->linearizedStateVariables(DataLocation::Transient))
	{
		if (slot >= MAX_SLOTS)
			throw SizeError("too much transient state: '" + var->name() + "' overflows the "
				+ std::to_string(MAX_SLOTS) + "-slot transient declaration capacity");
		auto const* type = var->type();
		unsigned size = type->storageBytes();
		if (!type->isValueType() || !size || size > SLOT_SIZE || offset > SLOT_SIZE - size)
			throw SizeError("unsupported transient scalar layout for '" + var->name() + "'");
		auto const* native = _typeMapper.map(type);
		TransientVar info;
		info.slot = checkedSize<unsigned>(slot, "transient logical slot");
		info.byteOffset = offset;
		info.byteSize = size;
		info.wtype = native;
		info.solType = type;
		info.hasAddressShadow = native == awst::WType::accountType() && size == 20;
		m_hasAddressShadow |= info.hasAddressShadow;
		m_totalSlots = std::max(m_totalSlots, info.slot + 1);
		m_varById.emplace(var->id(), m_vars.size());
		m_vars.push_back(info);
	}
}

bool TransientStorage::isTransient(VariableDeclaration const& _var) const
{
	return m_varById.count(_var.id()) > 0;
}

TransientStorage::TransientVar const* TransientStorage::getVarInfoById(int64_t _declId) const
{
	auto it = m_varById.find(_declId);
	return it != m_varById.end() ? &m_vars[it->second] : nullptr;
}

namespace
{
unsigned bytePosition(TransientStorage::TransientVar const& info)
{
	// Solc offsets start at the low end; each scratch word is big-endian.
	return info.slot * 32 + 32 - info.byteOffset - info.byteSize;
}

std::shared_ptr<awst::Statement> replaceScratch(
	int slot, unsigned offset, std::shared_ptr<awst::Expression> bytes,
	awst::SourceLocation const& loc)
{
	auto replacement = awst::makeReplace3(awst::makeLoadSlot(slot, loc),
		awst::makeIntegerConstant(offset, loc), std::move(bytes), loc);
	return awst::makeExpressionStatement(awst::makeStoreSlot(slot, std::move(replacement), loc), loc);
}
}

std::shared_ptr<awst::Expression> TransientStorage::buildRead(
	VariableDeclaration const& _var,
	awst::SourceLocation const& _loc) const
{
	auto const* info = getVarInfoById(_var.id());
	if (!info) return nullptr;
	auto raw = awst::makeExtract(awst::makeLoadSlot(m_scratchSlot, _loc),
		bytePosition(*info), info->byteSize, _loc);
	if (info->hasAddressShadow)
	{
		auto high = awst::makeExtract(awst::makeLoadSlot(addressShadowSlot(), _loc),
			info->slot * 12, 12, _loc);
		return awst::makeAsAccount(awst::makeConcat(std::move(high), std::move(raw), _loc), _loc);
	}
	return SlotWordCodec::packedBytesToNative(std::move(raw), info->wtype,
		info->solType, info->byteSize, _loc);
}

std::shared_ptr<awst::Statement> TransientStorage::buildWrite(
	VariableDeclaration const& _var, std::shared_ptr<awst::Expression> _value,
	awst::SourceLocation const& _loc) const
{
	auto const* info = getVarInfoById(_var.id());
	if (!info) return nullptr;
	auto body = awst::makeBlock(_loc);
	if (info->hasAddressShadow)
	{
		// A typed address supplies two buffers; pin once even if the source is
		// effectful. The shadow is private scratch, not a transient logical slot.
		auto name = "__transient_address_" + std::to_string(awst::NameGen::next("TransientStorage.address"));
		auto value = awst::makeVarExpression(name, info->wtype, _loc);
		body->body.push_back(awst::makeAssignmentStatement(value, std::move(_value), _loc));
		_value = value;
		body->body.push_back(replaceScratch(addressShadowSlot(), info->slot * 12,
			awst::makeExtract(awst::makeAsBytes(value, _loc), 0, 12, _loc), _loc));
	}
	body->body.push_back(replaceScratch(m_scratchSlot, bytePosition(*info),
		SlotWordCodec::nativeToPackedBytes(std::move(_value), info->wtype, info->byteSize, _loc), _loc));
	return body;
}

void TransientStorage::clearAddressShadowForWord(
	std::shared_ptr<awst::Expression> const& _slot,
	std::vector<std::shared_ptr<awst::Statement>>& _out,
	awst::SourceLocation const& _loc) const
{
	for (auto const& info: m_vars)
		if (info.hasAddressShadow)
		{
			auto matches = awst::makeNumericCompare(_slot, awst::NumericComparison::Eq,
				awst::makeIntegerConstant(info.slot, _loc, awst::WType::biguintType()), _loc);
			auto clear = awst::makeBlock(_loc);
			clear->body.push_back(replaceScratch(addressShadowSlot(), info.slot * 12,
				awst::makeBzero(12, _loc), _loc));
			_out.push_back(awst::makeIfElse(std::move(matches), std::move(clear), nullptr, _loc));
		}
}

} // namespace puyasol::builder
