#include "builder/storage/TransientStorage.h"
#include "builder/assembly/AssemblyBuilder.h"
#include "Logger.h"

#include <libsolidity/ast/Types.h>

namespace puyasol::builder
{

using namespace solidity::frontend;

void TransientStorage::collectVars(
	solidity::frontend::ContractDefinition const& _contract,
	TypeMapper& _typeMapper)
{
	m_vars.clear();
	m_varByName.clear();
	m_varById.clear();
	m_totalSlots = 0;

	// Collect transient vars across the linearization, most-base first,
	// de-duplicating by name (inherited-not-overridden handled like in
	// StorageLayout).
	auto const& linearized = _contract.annotation().linearizedBaseContracts;
	std::vector<VariableDeclaration const*> allVars;
	for (auto it = linearized.rbegin(); it != linearized.rend(); ++it)
	{
		for (auto const* var: (*it)->stateVariables())
		{
			if (var->isConstant() || var->immutable())
				continue;
			if (var->referenceLocation() != VariableDeclaration::Location::Transient)
				continue;
			bool alreadySeen = false;
			for (auto const* existing: allVars)
				if (existing->name() == var->name()) { alreadySeen = true; break; }
			if (alreadySeen) continue;
			allVars.push_back(var);
		}
	}

	unsigned currentSlot = 0;
	unsigned currentOffset = 0;
	for (auto const* var: allVars)
	{
		auto const* solType = var->type();
		unsigned byteSize = 32;
		if (solType)
			byteSize = solType->storageBytes();

		bool isDynamic = false;
		if (dynamic_cast<MappingType const*>(solType)
			|| dynamic_cast<ArrayType const*>(solType))
		{
			isDynamic = true;
			byteSize = 32;
		}

		// AVM addresses are 32-byte account public keys, not EVM's 20-byte
		// hashes. Store addresses at their native 32-byte width so that
		// acct_params_get / asset_holding_get / balance lookups round-trip.
		// This diverges from Solidity's 20-byte .slot/.offset packing for
		// address-typed transient vars — the AVM address semantics wins.
		auto* mappedType = _typeMapper.map(solType);
		if (dynamic_cast<AddressType const*>(solType))
			byteSize = 32;
		// Function pointers: Solidity says 24 bytes (address+selector) but
		// puya's AWST representation is bytes[12] (external) or uint64
		// (internal). Use the AWST width so read/write sizes match.
		if (dynamic_cast<FunctionType const*>(solType))
		{
			if (auto const* bwt = dynamic_cast<awst::BytesWType const*>(mappedType))
			{
				if (bwt->length().has_value() && *bwt->length() > 0)
					byteSize = static_cast<unsigned>(*bwt->length());
			}
			else if (mappedType == awst::WType::uint64Type())
				byteSize = 8;
		}

		// New slot if dynamic, >32 bytes, or overflow
		if (isDynamic || byteSize > 32 || currentOffset + byteSize > 32)
		{
			if (currentOffset > 0)
				currentSlot++;
			currentOffset = 0;
		}

		if (currentSlot >= MAX_SLOTS)
		{
			Logger::instance().warning(
				"transient variable '" + var->name() + "' exceeds max slots ("
				+ std::to_string(MAX_SLOTS) + "), skipped");
			continue;
		}

		TransientVar tv;
		tv.name = var->name();
		tv.declId = var->id();
		tv.slot = currentSlot;
		tv.byteOffset = currentOffset;
		tv.byteSize = byteSize;
		tv.wtype = mappedType;

		size_t idx = m_vars.size();
		m_vars.push_back(tv);
		m_varByName[tv.name] = idx;
		m_varById[tv.declId] = idx;

		currentOffset += byteSize;
		if (isDynamic)
		{
			currentSlot++;
			currentOffset = 0;
		}
	}

	m_totalSlots = (currentOffset > 0) ? currentSlot + 1 : currentSlot;
}

bool TransientStorage::isTransient(VariableDeclaration const& _var) const
{
	return m_varById.count(_var.id()) > 0;
}

TransientStorage::TransientVar const* TransientStorage::getVarInfo(std::string const& _name) const
{
	auto it = m_varByName.find(_name);
	return (it != m_varByName.end()) ? &m_vars[it->second] : nullptr;
}

TransientStorage::TransientVar const* TransientStorage::getVarInfoById(int64_t _declId) const
{
	auto it = m_varById.find(_declId);
	return (it != m_varById.end()) ? &m_vars[it->second] : nullptr;
}

namespace
{
	std::shared_ptr<awst::Expression> loadTransientBlob(awst::SourceLocation const& _loc)
	{
		auto loadOp = awst::makeIntrinsicCall("load", awst::WType::bytesType(), _loc);
		loadOp->immediates = {AssemblyBuilder::TRANSIENT_SLOT};
		return loadOp;
	}

	/// Extract `byteSize` bytes from the transient blob at absolute byte
	/// offset `absByte`, returned as raw bytes.
	std::shared_ptr<awst::Expression> extractBytes(
		unsigned absByte, unsigned byteSize, awst::SourceLocation const& _loc)
	{
		auto blob = loadTransientBlob(_loc);
		auto extract = awst::makeIntrinsicCall("extract", awst::WType::bytesType(), _loc);
		extract->immediates = {static_cast<int>(absByte), static_cast<int>(byteSize)};
		extract->stackArgs.push_back(std::move(blob));
		return extract;
	}
}

std::shared_ptr<awst::Expression> TransientStorage::buildRead(
	std::string const& _name, awst::WType const* _type,
	awst::SourceLocation const& _loc) const
{
	auto const* info = getVarInfo(_name);
	if (!info)
		return nullptr;

	// Solidity's `byteOffset` is from the LEAST significant byte of the slot
	// (i.e., low end of the big-endian word). Our packed scratch blob stores
	// slot S as the big-endian bytes [S*32 .. S*32+32), so the absolute byte
	// offset of the variable is S*32 + (32 - byteOffset - byteSize).
	unsigned absByte = info->slot * SLOT_SIZE + (SLOT_SIZE - info->byteOffset - info->byteSize);
	unsigned sz = info->byteSize;

	// uint64 / bool-ish path: extract up to 8 bytes and btoi
	if (_type == awst::WType::uint64Type() || _type == awst::WType::boolType())
	{
		// Clamp extract width to ≤ 8 bytes (EVM clean value semantics for
		// packed small types).
		unsigned readSize = sz <= 8 ? sz : 8;
		// For uint64 stored as 8 bytes, just extract those bytes.
		auto raw = extractBytes(absByte, readSize, _loc);
		auto btoi = awst::makeIntrinsicCall("btoi", awst::WType::uint64Type(), _loc);
		btoi->stackArgs.push_back(std::move(raw));
		// Bool: compare to 0 to produce a proper bool-typed expression
		// (btoi returns uint64; callers like `!lock` require bool).
		if (_type == awst::WType::boolType())
		{
			auto zero = awst::makeIntegerConstant("0", _loc);
			return awst::makeNumericCompare(
				std::move(btoi), awst::NumericComparison::Ne, std::move(zero), _loc);
		}
		return btoi;
	}

	// biguint path: extract `sz` bytes, reinterpret as biguint (big-endian).
	// AVM biguint values may be shorter than 32 bytes — they're value-equal
	// regardless of leading zeros.
	if (_type == awst::WType::biguintType())
	{
		auto raw = extractBytes(absByte, sz, _loc);
		return awst::makeReinterpretCast(std::move(raw), awst::WType::biguintType(), _loc);
	}

	// Account: stored as 20 bytes (EVM-compatible offset layout), but AVM
	// accounts are 32 bytes. Left-pad with 12 zero bytes and reinterpret
	// so comparisons with address(0)/account literals work.
	if (_type == awst::WType::accountType() && sz < 32)
	{
		auto raw = extractBytes(absByte, sz, _loc);
		auto prefix = awst::makeIntrinsicCall("bzero", awst::WType::bytesType(), _loc);
		auto prefLen = awst::makeIntegerConstant(std::to_string(32 - sz), _loc);
		prefix->stackArgs.push_back(std::move(prefLen));
		auto cat = awst::makeIntrinsicCall("concat", awst::WType::bytesType(), _loc);
		cat->stackArgs.push_back(std::move(prefix));
		cat->stackArgs.push_back(std::move(raw));
		return awst::makeReinterpretCast(std::move(cat), awst::WType::accountType(), _loc);
	}

	// Default: raw bytes of the stored width (e.g., bytesN). Reinterpret to
	// the requested type so downstream type checks pass.
	auto raw = extractBytes(absByte, sz, _loc);
	if (_type && _type != awst::WType::bytesType())
		return awst::makeReinterpretCast(std::move(raw), _type, _loc);
	return raw;
}

namespace
{
	/// Truncate value to `byteSize` bytes suitable for writing into a
	/// packed transient slot. Produces a raw-bytes expression of exactly
	/// `byteSize` bytes.
	std::shared_ptr<awst::Expression> truncateToBytes(
		std::shared_ptr<awst::Expression> _value,
		unsigned byteSize,
		awst::SourceLocation const& _loc)
	{
		bool isUint64 = (_value->wtype == awst::WType::uint64Type());
		bool isBool = (_value->wtype == awst::WType::boolType());

		std::shared_ptr<awst::Expression> raw;

		if (isUint64 || isBool)
		{
			// itob produces 8 big-endian bytes.
			auto itob = awst::makeIntrinsicCall("itob", awst::WType::bytesType(), _loc);
			itob->stackArgs.push_back(std::move(_value));
			if (byteSize >= 8)
			{
				// Left-pad with bzero(byteSize - 8) to reach full width.
				auto prefix = awst::makeIntrinsicCall("bzero", awst::WType::bytesType(), _loc);
				auto prefLen = awst::makeIntegerConstant(std::to_string(byteSize - 8), _loc);
				prefix->stackArgs.push_back(std::move(prefLen));
				auto cat = awst::makeIntrinsicCall("concat", awst::WType::bytesType(), _loc);
				cat->stackArgs.push_back(std::move(prefix));
				cat->stackArgs.push_back(std::move(itob));
				raw = std::move(cat);
			}
			else
			{
				// Truncate: take the last `byteSize` bytes of the 8-byte itob result.
				auto ext = awst::makeIntrinsicCall("extract", awst::WType::bytesType(), _loc);
				ext->immediates = {static_cast<int>(8 - byteSize), static_cast<int>(byteSize)};
				ext->stackArgs.push_back(std::move(itob));
				raw = std::move(ext);
			}
		}
		else if (_value->wtype == awst::WType::biguintType())
		{
			// biguint values are big-endian bytes possibly shorter than 32.
			// Pad to exactly 32 bytes via `b| bzero(32)` (b| yields
			// max(len(a), len(b)); biguint ≤ 32 bytes so result is 32).
			// Then extract the trailing `byteSize` bytes at compile-time offset.
			auto bytesView = awst::makeReinterpretCast(std::move(_value), awst::WType::bytesType(), _loc);
			auto zeros = awst::makeIntrinsicCall("bzero", awst::WType::bytesType(), _loc);
			auto sz = awst::makeIntegerConstant("32", _loc);
			zeros->stackArgs.push_back(std::move(sz));
			auto padded = awst::makeIntrinsicCall("b|", awst::WType::bytesType(), _loc);
			padded->stackArgs.push_back(std::move(zeros));
			padded->stackArgs.push_back(std::move(bytesView));
			auto ext = awst::makeIntrinsicCall("extract", awst::WType::bytesType(), _loc);
			ext->immediates = {static_cast<int>(32 - byteSize), static_cast<int>(byteSize)};
			ext->stackArgs.push_back(std::move(padded));
			raw = std::move(ext);
		}
		else if (_value->wtype == awst::WType::accountType() && byteSize < 32)
		{
			// AVM account is 32 bytes; Solidity address layout is 20.
			// Reinterpret as bytes and truncate to the low `byteSize` bytes
			// (drop the leading 32 - byteSize bytes) to match EVM layout.
			auto bytesView = awst::makeReinterpretCast(
				std::move(_value), awst::WType::bytesType(), _loc);
			auto ext = awst::makeIntrinsicCall("extract", awst::WType::bytesType(), _loc);
			ext->immediates = {static_cast<int>(32 - byteSize), static_cast<int>(byteSize)};
			ext->stackArgs.push_back(std::move(bytesView));
			raw = std::move(ext);
		}
		else
		{
			// Already raw bytes (e.g., bytesN). Reinterpret to bytes for
			// uniform downstream handling; assume width matches.
			if (_value->wtype != awst::WType::bytesType())
				_value = awst::makeReinterpretCast(
					std::move(_value), awst::WType::bytesType(), _loc);
			raw = std::move(_value);
		}

		return raw;
	}
}

std::shared_ptr<awst::Statement> TransientStorage::buildWrite(
	std::string const& _name, std::shared_ptr<awst::Expression> _value,
	awst::SourceLocation const& _loc) const
{
	auto const* info = getVarInfo(_name);
	if (!info)
		return nullptr;

	// See buildRead: byteOffset is low-end, so we write to the tail of the slot.
	unsigned absByte = info->slot * SLOT_SIZE + (SLOT_SIZE - info->byteOffset - info->byteSize);

	auto raw = truncateToBytes(std::move(_value), info->byteSize, _loc);

	auto blobRead = loadTransientBlob(_loc);

	// replace2(blob, raw) at compile-time absByte immediate
	auto replace = awst::makeIntrinsicCall("replace2", awst::WType::bytesType(), _loc);
	replace->immediates = {static_cast<int>(absByte)};
	replace->stackArgs.push_back(std::move(blobRead));
	replace->stackArgs.push_back(std::move(raw));

	auto storeOp = awst::makeIntrinsicCall("store", awst::WType::voidType(), _loc);
	storeOp->immediates = {AssemblyBuilder::TRANSIENT_SLOT};
	storeOp->stackArgs.push_back(std::move(replace));

	return awst::makeExpressionStatement(std::move(storeOp), _loc);
}

} // namespace puyasol::builder
