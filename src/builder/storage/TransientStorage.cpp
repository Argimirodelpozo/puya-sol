#include "builder/storage/TransientStorage.h"
#include "builder/assembly/AssemblyBuilder.h"
#include "builder/contract/StateVarWalker.h"
#include "builder/sol-types/TypeCoercion.h"
#include "builder/sol-types/SolIntType.h"
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

	// Collect transient vars base-first, de-duplicating by name (same as StorageLayout).
	std::vector<VariableDeclaration const*> allVars;
	forEachStateVarReverse(_contract, [&](auto const* var)
	{
		if (var->isConstant() || var->immutable())
			return;
		if (var->referenceLocation() != VariableDeclaration::Location::Transient)
			return;
		bool alreadySeen = false;
		for (auto const* existing: allVars)
			if (existing->name() == var->name()) { alreadySeen = true; break; }
		if (alreadySeen) return;
		allVars.push_back(var);
	});

	unsigned currentSlot = 0;
	unsigned currentOffset = 0;
	bool overflowReported = false;
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

		// AVM addresses are 32-byte public keys (not EVM's 20-byte hashes).
		// Store at 32B so acct_params_get/balance lookups round-trip;
		// diverges from Solidity's 20-byte .slot/.offset packing.
		auto* mappedType = _typeMapper.map(solType);
		if (dynamic_cast<AddressType const*>(solType))
			byteSize = 32;
		// Function pointers: Solidity says 24B but AWST is bytes[12] (external)
		// or uint64 (internal); use AWST width so read/write sizes match.
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

		// Start a new slot for dynamic, oversized, or spilling vars.
		if (isDynamic || byteSize > 32 || currentOffset + byteSize > 32)
		{
			if (currentOffset > 0)
				currentSlot++;
			currentOffset = 0;
		}

		if (currentSlot >= MAX_SLOTS)
		{
			// Transient state overflows the MAX_SLOTS scratch blob; must fail
			// rather than silently drop vars (nullptr reads/writes = wrong code).
			// Report once — slots only grow.
			if (!overflowReported)
			{
				Logger::instance().error(
					"too much transient state: '" + var->name() + "' overflows the "
					+ std::to_string(MAX_SLOTS) + "-slot ("
					+ std::to_string(MAX_SLOTS * SLOT_SIZE) + "-byte) transient scratch "
					"blob. Reduce the number or width of `transient` state variables.");
				overflowReported = true;
			}
			continue;
		}

		TransientVar tv;
		tv.name = var->name();
		tv.declId = var->id();
		tv.slot = currentSlot;
		tv.byteOffset = currentOffset;
		tv.byteSize = byteSize;
		tv.wtype = mappedType;
		tv.solType = solType;

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
		return awst::makeLoadSlot(AssemblyBuilder::TRANSIENT_SLOT, _loc);
	}

	/// Extract byteSize bytes from the transient blob at absByte.
	std::shared_ptr<awst::Expression> extractBytes(
		unsigned absByte, unsigned byteSize, awst::SourceLocation const& _loc)
	{
		auto blob = loadTransientBlob(_loc);
		return awst::makeExtract(
			std::move(blob),
			static_cast<int>(absByte), static_cast<int>(byteSize), _loc);
	}
}

std::shared_ptr<awst::Expression> TransientStorage::buildRead(
	std::string const& _name, awst::WType const* _type,
	awst::SourceLocation const& _loc) const
{
	auto const* info = getVarInfo(_name);
	if (!info)
		return nullptr;

	// byteOffset is from the low (LSB) end; blob stores slot S at big-endian
	// bytes [S*32..S*32+32), so absByte = S*32 + (32 - byteOffset - byteSize).
	unsigned absByte = info->slot * SLOT_SIZE + (SLOT_SIZE - info->byteOffset - info->byteSize);
	unsigned sz = info->byteSize;

	// uint64/bool: extract ≤8 bytes and btoi.
	if (_type == awst::WType::uint64Type() || _type == awst::WType::boolType())
	{
		unsigned readSize = sz <= 8 ? sz : 8;
		auto raw = extractBytes(absByte, readSize, _loc);
		auto btoi = awst::makeBtoi(std::move(raw), _loc);
		// bool: btoi gives uint64; compare != 0 for callers expecting bool (e.g. `!lock`).
		if (_type == awst::WType::boolType())
		{
			auto zero = awst::makeZero(_loc);
			return awst::makeNumericCompare(
				std::move(btoi), awst::NumericComparison::Ne, std::move(zero), _loc);
		}
		// Sub-64 signed: the cell holds byteSize-truncated TC (write side
		// truncates), but the uint64 carrier convention is 64-bit TC —
		// re-extend from the declared width (same rule as SlotWordCodec;
		// this branch previously returned the raw btoi, so a transient
		// int32 x = -1 read back as +4294967295).
		std::shared_ptr<awst::Expression> result = std::move(btoi);
		if (auto it = SolIntType::fromSol(info->solType);
			it && it->isSigned && it->bits < 64)
			result = TypeCoercion::signExtendToUint64(std::move(result), it->bits, _loc);
		return result;
	}

	// biguint: extract sz bytes and reinterpret (leading-zero-invariant).
	if (_type == awst::WType::biguintType())
	{
		auto raw = extractBytes(absByte, sz, _loc);
		std::shared_ptr<awst::Expression> bg = awst::makeAsBiguint(std::move(raw), _loc);
		// Sign-extend signed sub-256 (e.g. int128) to canonical 256-bit on read.
		// No-op for unsigned/int256/non-integer (see TypeCoercion).
		return TypeCoercion::signExtendSignedElement(std::move(bg), info->solType, _loc);
	}

	// Account: stored as 20B (EVM layout), AVM needs 32B; left-pad 12 zeros.
	if (_type == awst::WType::accountType() && sz < 32)
	{
		auto raw = extractBytes(absByte, sz, _loc);
		auto cat = awst::makeLeftPad(std::move(raw), 32 - sz, _loc);
		return awst::makeAsAccount(std::move(cat), _loc);
	}

	// Default (e.g. bytesN): raw bytes; reinterpret to requested type.
	auto raw = extractBytes(absByte, sz, _loc);
	if (_type && _type != awst::WType::bytesType())
		return awst::makeReinterpretCast(std::move(raw), _type, _loc);
	return raw;
}

namespace
{
	/// Truncate value to byteSize bytes for writing into a packed transient slot.
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
			// itob → 8 big-endian bytes; pad or truncate to byteSize.
			auto itob = awst::makeItob(std::move(_value), _loc);
			if (byteSize >= 8)
				raw = awst::makeLeftPad(std::move(itob), byteSize - 8, _loc);
			else
				raw = awst::makeExtract(std::move(itob),
					static_cast<int>(8 - byteSize), static_cast<int>(byteSize), _loc);
		}
		else if (_value->wtype == awst::WType::biguintType())
		{
			// Pad to 32B (biguint may be shorter), then extract trailing byteSize bytes.
			auto bytesView = awst::makeAsBytes(std::move(_value), _loc);
			auto padded = awst::makeZeroExtendToN(std::move(bytesView), 32, _loc);
			raw = awst::makeExtract(std::move(padded),
				static_cast<int>(32 - byteSize), static_cast<int>(byteSize), _loc);
		}
		else if (_value->wtype == awst::WType::accountType() && byteSize < 32)
		{
			// AVM account is 32B; drop leading (32-byteSize) bytes to match EVM layout.
			auto bytesView = awst::makeAsBytes(std::move(_value), _loc);
			raw = awst::makeExtract(
				std::move(bytesView),
				static_cast<int>(32 - byteSize), static_cast<int>(byteSize), _loc);
		}
		else
		{
			// Already raw bytes (e.g. bytesN); reinterpret to bytes, assume width matches.
			if (_value->wtype != awst::WType::bytesType())
				_value = awst::makeAsBytes(std::move(_value), _loc);
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

	// byteOffset is low-end; write to the tail of the slot (same offset formula as buildRead).
	unsigned absByte = info->slot * SLOT_SIZE + (SLOT_SIZE - info->byteOffset - info->byteSize);

	auto raw = truncateToBytes(std::move(_value), info->byteSize, _loc);

	auto blobRead = loadTransientBlob(_loc);

	// replace2(blob, raw) with compile-time absByte immediate.
	auto replace = awst::makeIntrinsicCall("replace2", awst::WType::bytesType(), _loc);
	replace->immediates = {static_cast<int>(absByte)};
	replace->stackArgs.push_back(std::move(blobRead));
	replace->stackArgs.push_back(std::move(raw));

	auto storeOp = awst::makeStoreSlot(
		AssemblyBuilder::TRANSIENT_SLOT, std::move(replace), _loc);

	return awst::makeExpressionStatement(std::move(storeOp), _loc);
}

} // namespace puyasol::builder
