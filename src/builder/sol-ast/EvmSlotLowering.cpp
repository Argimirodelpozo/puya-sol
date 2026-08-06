/// @file EvmSlotLowering.cpp
/// See EvmSlotLowering.h. Slot derivation mirrors Solidity's storage layout
/// exactly (StorageLayout is tripwire-verified against solc's own tables), so
/// Yul-side slot arithmetic and Solidity-side accesses address the same words.

#include "builder/sol-ast/EvmSlotLowering.h"
#include "builder/sol-ast/Context.h"
#include "builder/storage/EvmLayoutMode.h"
#include "builder/storage/StorageLayout.h"
#include "builder/storage/SlotHandleAccess.h"
#include "builder/storage/SlotWordCodec.h"
#include "builder/sol-types/TypeCoercion.h"
#include "awst/NameGen.h"
#include "Logger.h"

#include <libsolutil/Keccak256.h>
#include <libsolutil/CommonData.h>

namespace puyasol::builder::sol_ast
{

using namespace solidity::frontend;

namespace
{

VariableDeclaration const* referencedVar(Expression const& _e)
{
	auto const* id = dynamic_cast<Identifier const*>(&_e);
	if (!id)
		return nullptr;
	return dynamic_cast<VariableDeclaration const*>(id->annotation().referencedDeclaration);
}

bool isPersistentStateVar(VariableDeclaration const* _vd)
{
	return _vd && _vd->isStateVariable() && !_vd->isConstant() && !_vd->immutable()
		&& _vd->referenceLocation() != VariableDeclaration::Location::Transient;
}

/// biguint → uint64 (values known < 2^64: page offsets, lengths).
std::shared_ptr<awst::Expression> biguintToU64(
	std::shared_ptr<awst::Expression> _v, awst::SourceLocation const& _loc)
{
	auto cat = awst::makeLeftPad(awst::makeAsBytes(std::move(_v), _loc), 8, _loc);
	return awst::makeBtoi(awst::makeExtractLastN(std::move(cat), 8, _loc), _loc);
}

std::shared_ptr<awst::Expression> toBiguintIndex(
	std::shared_ptr<awst::Expression> _idx, awst::SourceLocation const& _loc)
{
	if (_idx->wtype == awst::WType::uint64Type())
		return awst::makeAsBiguint(awst::makeItob(std::move(_idx), _loc), _loc);
	if (_idx->wtype && _idx->wtype->kind() == awst::WTypeKind::ARC4UIntN)
		return awst::makeARC4Decode(std::move(_idx), awst::WType::biguintType(), _loc);
	return _idx;
}

/// slot (biguint) → its 32-byte big-endian word form.
std::shared_ptr<awst::Expression> slot32Bytes(
	std::shared_ptr<awst::Expression> _slot, awst::SourceLocation const& _loc)
{
	return awst::makeLeftPadToN(awst::makeAsBytes(std::move(_slot), _loc), 32, _loc);
}

} // namespace

namespace
{
/// A storage-located local/param — in this mode the variable IS a biguint
/// slot handle (assigned at declaration / by the call convention).
bool isSlotHandleLocal(VariableDeclaration const* _vd)
{
	return _vd && !_vd->isStateVariable()
		&& (_vd->isLocalVariable() || _vd->isCallableOrCatchParameter())
		&& _vd->referenceLocation() == VariableDeclaration::Location::Storage;
}

/// A non-identifier root (library call returning `T storage`, etc.) whose
/// TYPE designates storage — its built value is the biguint slot.
bool isStorageTypedRoot(Expression const& _e)
{
	auto const* t = _e.annotation().type;
	if (!t)
		return false;
	if (dynamic_cast<MappingType const*>(t))
		return true;
	auto const* rt = dynamic_cast<ReferenceType const*>(t);
	return rt && rt->dataStoredIn(DataLocation::Storage);
}
} // namespace

bool EvmSlotLowering::isStorageStateRef(Expression const& _e)
{
	Expression const* cur = &_e;
	for (;;)
	{
		if (auto const* ia = dynamic_cast<IndexAccess const*>(cur))
		{
			cur = &ia->baseExpression();
			continue;
		}
		if (auto const* ma = dynamic_cast<MemberAccess const*>(cur))
		{
			// Only peel STRUCT member access — `.length`/`.push` etc. are not
			// storage lvalue layers.
			if (dynamic_cast<StructType const*>(ma->expression().annotation().type))
			{
				cur = &ma->expression();
				continue;
			}
			return false;
		}
		break;
	}
	if (auto const* vd = referencedVar(*cur))
		return isPersistentStateVar(vd) || isSlotHandleLocal(vd);
	// Non-identifier root: a storage-typed expression (e.g. a library call
	// returning `T storage`) builds to a biguint slot handle in this mode.
	return isStorageTypedRoot(*cur);
}

std::shared_ptr<awst::Expression> EvmSlotLowering::biguintConst(std::string const& _v)
{
	return awst::makeIntegerConstant(_v, m_loc, awst::WType::biguintType());
}

std::shared_ptr<awst::Expression> EvmSlotLowering::readSlotWord(
	std::shared_ptr<awst::Expression> _slot, awst::SourceLocation const& _loc)
{
	return SlotHandleAccess::readSlot(std::move(_slot), _loc);
}

std::shared_ptr<awst::Expression> EvmSlotLowering::dynDataBase(
	std::shared_ptr<awst::Expression> _slot, awst::SourceLocation const& _loc)
{
	// CONSTANT slot (every declared dynamic array): fold keccak256(slot32) in
	// the compiler — the on-chain hash (130 budget units) never runs. Mirrors
	// the default model's compile-time ArrayData SlotRoutes.
	if (auto const* ic = dynamic_cast<awst::IntegerConstant const*>(_slot.get()))
	{
		solidity::u256 slotVal{ic->value};
		auto k = solidity::u256(solidity::util::keccak256(
			solidity::toBigEndian(slotVal)));
		return awst::makeIntegerConstant(k.str(), _loc, awst::WType::biguintType());
	}
	return awst::makeAsBiguint(
		awst::makeKeccak256(slot32Bytes(std::move(_slot), _loc), _loc), _loc);
}

std::shared_ptr<awst::Expression> EvmSlotLowering::mappingEntrySlot(
	std::shared_ptr<awst::Expression> _base,
	std::shared_ptr<awst::Expression> _key,
	Type const* _keyType)
{
	// EVM: entry slot = keccak256(encodedKey ++ slot32). Value-type keys encode
	// as their 32-byte word; string/bytes keys contribute their raw bytes.
	std::shared_ptr<awst::Expression> keyBytes;
	auto const* keyArr = dynamic_cast<ArrayType const*>(_keyType);
	bool isDynamicBytesKey = keyArr && keyArr->isByteArrayOrString();
	if (isDynamicBytesKey)
	{
		keyBytes = _key->wtype == awst::WType::stringType()
			? awst::makeAsBytes(std::move(_key), m_loc)
			: std::move(_key);
		if (keyBytes->wtype != awst::WType::bytesType())
			keyBytes = awst::makeAsBytes(std::move(keyBytes), m_loc);
	}
	else
	{
		// Coerce to the mapped key carrier first (integer literals may build
		// narrower), then take the value's slot-word form.
		auto const* keyW = m_ctx.typeMapper.map(_keyType);
		if (keyW && _key->wtype != keyW
			&& (keyW == awst::WType::uint64Type() || keyW == awst::WType::biguintType()))
			_key = TypeCoercion::implicitNumericCast(std::move(_key), keyW, m_loc);
		auto const* effW = _key->wtype;
		if (auto const* bw = dynamic_cast<awst::BytesWType const*>(effW);
			bw && bw->length().has_value())
		{
			// bytesN keys are LEFT-aligned in the key word (EVM right-pads).
			unsigned n = static_cast<unsigned>(*bw->length());
			keyBytes = awst::makeAsBytes(std::move(_key), m_loc);
			if (n < 32)
				keyBytes = awst::makeConcat(
					std::move(keyBytes), awst::makeBzero(32 - static_cast<int>(n), m_loc), m_loc);
		}
		else
			keyBytes = SlotWordCodec::nativeToPackedBytes(std::move(_key), effW, 32, m_loc);
	}

	auto preimage = awst::makeConcat(
		std::move(keyBytes), slot32Bytes(std::move(_base), m_loc), m_loc);
	return awst::makeAsBiguint(awst::makeKeccak256(std::move(preimage), m_loc), m_loc);
}

namespace
{
/// Shadow slot carrying the HIGH 12 bytes of a PACKED address (a 20-byte
/// window cannot hold a 32-byte AVM address; the word keeps the EVM-shaped
/// trailing-20 so asm sees EVM layout, and Solidity reads recombine).
/// keccak-derived, so it can never collide with declared or mapping slots.
std::shared_ptr<awst::Expression> packedAddrAuxSlot(
	std::shared_ptr<awst::Expression> _slot,
	std::shared_ptr<awst::Expression> const& _byteOffset,
	awst::SourceLocation const& _loc)
{
	auto pre = awst::makeConcat(
		awst::makeLeftPadToN(awst::makeAsBytes(std::move(_slot), _loc), 32, _loc),
		_byteOffset
			? std::shared_ptr<awst::Expression>(awst::makeItob(_byteOffset, _loc))
			: std::shared_ptr<awst::Expression>(
				awst::makeBytesConstant(std::vector<uint8_t>(8, 0), _loc)),
		_loc);
	pre = awst::makeConcat(std::move(pre),
		awst::makeUtf8BytesConstant("addraux", _loc), _loc);
	return awst::makeAsBiguint(awst::makeKeccak256(std::move(pre), _loc), _loc);
}
} // namespace

EvmSlotLowering::Addr EvmSlotLowering::makeLeafAddr(
	std::shared_ptr<awst::Expression> _slot,
	std::shared_ptr<awst::Expression> _byteOffset,
	unsigned _size,
	bool _aloneInSlot,
	Type const* _solType)
{
	Addr a;
	a.slot = std::move(_slot);
	a.byteOffset = std::move(_byteOffset);
	a.size = _size;
	a.solType = _solType;
	a.wtype = m_ctx.typeMapper.map(_solType);
	// Full-slot AVM account: keep all 32 bytes so real addresses round-trip.
	// (EVM's 20-byte packing survives only where the address shares its slot.)
	if (a.wtype == awst::WType::accountType() && _aloneInSlot && !a.byteOffset)
		a.size = 32;
	return a;
}

std::optional<EvmSlotLowering::Addr> EvmSlotLowering::resolve(Expression const& _e)
{
	if (auto const* id = dynamic_cast<Identifier const*>(&_e))
		return resolveIdentifier(*id);
	if (auto const* ia = dynamic_cast<IndexAccess const*>(&_e))
		return resolveIndexAccess(*ia);
	if (auto const* ma = dynamic_cast<MemberAccess const*>(&_e))
		return resolveMemberAccess(*ma);
	// Type conversion over a storage ref (`bytes(a)` on a storage string):
	// same location, different label — peel it.
	if (auto const* fc = dynamic_cast<FunctionCall const*>(&_e))
		if (fc->annotation().kind.set()
			&& *fc->annotation().kind == FunctionCallKind::TypeConversion
			&& !fc->arguments().empty() && isStorageTypedRoot(_e))
			return resolve(*fc->arguments()[0]);
	// Any other storage-typed root (library call returning `T storage`, a
	// parenthesized/tuple-wrapped ref, ...): its built value IS the slot.
	if (isStorageTypedRoot(_e))
	{
		auto built = m_ctx.buildExpr(_e);
		if (built && built->wtype == awst::WType::biguintType())
		{
			auto const* t = _e.annotation().type;
			return makeLeafAddr(std::move(built), nullptr,
				t ? t->storageBytes() : 32, /*alone*/ true, t);
		}
		Logger::instance().error(
			"--evm-storage-layout: storage-typed expression did not lower to a "
			"slot handle", m_loc);
		return std::nullopt;
	}
	Logger::instance().error(
		"unsupported storage expression shape in --evm-storage-layout", m_loc);
	return std::nullopt;
}

std::optional<EvmSlotLowering::Addr> EvmSlotLowering::addrForStateVar(
	VariableDeclaration const& _vd)
{
	auto const* layout = m_ctx.evmSlotLayout;
	if (!layout)
	{
		Logger::instance().error(
			"--evm-storage-layout: no storage layout in this context (state "
			"variable '" + _vd.name() + "' accessed outside a contract?)", m_loc);
		return std::nullopt;
	}
	auto const* sv = layout->getVarInfoById(_vd.id());
	if (!sv)
	{
		// A declaration owned by ANOTHER contract is not a layout bug: some
		// passes speculatively lower a foreign function body (taking
		// `Other.f.selector` registers f as a function-pointer target) and
		// DISCARD the result — default mode emits nothing for it either. Only
		// a variable that should be OURS (this contract or one of its bases)
		// means the layout walk really missed something.
		auto const* owner = dynamic_cast<solidity::frontend::ContractDefinition const*>(
			_vd.scope());
		auto const* self = layout->contract();
		bool foreign = owner && self && owner != self;
		if (foreign)
			for (auto const* base: self->annotation().linearizedBaseContracts)
				if (base == owner)
					{ foreign = false; break; }
		if (foreign)
		{
			Logger::instance().debug(
				"--evm-storage-layout: skipping foreign state variable '"
				+ _vd.name() + "' (declared in contract " + owner->name()
				+ ", compiling " + self->name() + ")", m_loc);
			return std::nullopt;
		}
		Logger::instance().error(
			"--evm-storage-layout: state variable '" + _vd.name() + "'"
			+ (owner ? " (declared in contract " + owner->name() + ")" : "")
			+ " missing from the storage layout", m_loc);
		return std::nullopt;
	}
	// Alone in its slot (isFullSlot only covers 32-byte vars; a lone address
	// is 20 bytes yet still owns the whole slot).
	bool alone = sv->isFullSlot;
	if (!alone)
		if (auto const* si = layout->getSlotInfo(sv->slot))
			alone = si->variables.size() == 1;
	return makeLeafAddr(
		biguintConst(sv->slot.str()),
		sv->byteOffset
			? awst::makeIntegerConstant(static_cast<uint64_t>(sv->byteOffset), m_loc)
			: nullptr,
		sv->byteSize,
		alone,
		_vd.type());
}

std::optional<EvmSlotLowering::Addr> EvmSlotLowering::resolveIdentifier(
	Identifier const& _id)
{
	auto const* vd = referencedVar(_id);
	if (isSlotHandleLocal(vd))
	{
		// Storage-located local/param: the variable holds the biguint slot
		// (bound at declaration / by the call convention / asm `.slot :=`).
		auto slot = awst::makeVarExpression(
			vd->name(), awst::WType::biguintType(), m_loc);
		return makeLeafAddr(std::move(slot), nullptr,
			vd->type() ? vd->type()->storageBytes() : 32, /*alone*/ true,
			vd->type());
	}
	if (!isPersistentStateVar(vd))
	{
		Logger::instance().error(
			"--evm-storage-layout: cannot resolve '" + _id.name()
			+ "' to a storage slot (not a persistent state variable)", m_loc);
		return std::nullopt;
	}
	return addrForStateVar(*vd);
}

std::optional<EvmSlotLowering::Addr> EvmSlotLowering::resolveIndexAccess(
	IndexAccess const& _ia)
{
	auto const* baseType = _ia.baseExpression().annotation().type;
	if (!_ia.indexExpression())
		return std::nullopt;

	if (auto const* mt = dynamic_cast<MappingType const*>(baseType))
	{
		auto base = resolve(_ia.baseExpression());
		if (!base)
			return std::nullopt;
		auto key = m_ctx.buildExpr(*_ia.indexExpression());
		if (!key)
			return std::nullopt;
		auto slot = mappingEntrySlot(base->slot, std::move(key), mt->keyType());
		auto const* valueType = mt->valueType();
		return makeLeafAddr(std::move(slot), nullptr,
			valueType->storageBytes(), /*alone*/ true, valueType);
	}

	if (auto const* at = dynamic_cast<ArrayType const*>(baseType);
		at && at->dataStoredIn(DataLocation::Storage))
	{
		if (at->isByteArrayOrString())
		{
			Logger::instance().error(
				"--evm-storage-layout: bytes/string element access not yet supported",
				m_loc);
			return std::nullopt;
		}
		auto base = resolve(_ia.baseExpression());
		if (!base)
			return std::nullopt;

		auto idx = toBiguintIndex(m_ctx.buildExpr(*_ia.indexExpression()), m_loc);
		if (!idx)
			return std::nullopt;

		std::shared_ptr<awst::Expression> dataBase;
		if (at->isDynamicallySized())
		{
			// Bounds: idx < length word (EVM Panic 0x32 shape). Pin the index —
			// the assert is a separate pre-statement.
			if (!dynamic_cast<awst::VarExpression const*>(idx.get())
				&& !dynamic_cast<awst::IntegerConstant const*>(idx.get()))
			{
				std::string nm = "__evm_idx_"
					+ std::to_string(awst::NameGen::next("EvmSlotLowering.idx"));
				auto const* idxWt = idx->wtype;   // read BEFORE the move (arg eval order)
				m_ctx.prePendingStatements.push_back(awst::makeAssignmentStatement(
					awst::makeVarExpression(nm, idxWt, m_loc), std::move(idx), m_loc));
				idx = awst::makeVarExpression(nm, awst::WType::biguintType(), m_loc);
			}
			auto len = readSlotWord(base->slot, m_loc);
			auto cmp = awst::makeNumericCompare(
				idx, awst::NumericComparison::Lt, std::move(len), m_loc);
			m_ctx.prePendingStatements.push_back(awst::makeExpressionStatement(
				awst::makeAssert(std::move(cmp), m_loc, "array index out of bounds"), m_loc));
			dataBase = dynDataBase(base->slot, m_loc);
		}
		else
		{
			idx = SlotHandleAccess::boundsCheckIndex(
				m_ctx.prePendingStatements, std::move(idx), at, m_loc);
			dataBase = base->slot;
		}

		return elemAddr(std::move(dataBase), std::move(idx), at->baseType());
	}

	Logger::instance().error(
		"--evm-storage-layout: unsupported index-access base in storage", m_loc);
	return std::nullopt;
}

std::optional<EvmSlotLowering::Addr> EvmSlotLowering::resolveMemberAccess(
	MemberAccess const& _ma)
{
	auto const* st = dynamic_cast<StructType const*>(_ma.expression().annotation().type);
	if (!st || !st->dataStoredIn(DataLocation::Storage))
	{
		Logger::instance().error(
			"--evm-storage-layout: unsupported member-access base in storage", m_loc);
		return std::nullopt;
	}
	auto base = resolve(_ma.expression());
	if (!base)
		return std::nullopt;

	auto const& off = st->storageOffsetsOfMember(_ma.memberName());
	auto const* fieldType = _ma.annotation().type;
	unsigned size = fieldType ? fieldType->storageBytes() : 32;

	// Alone in its slot? (Decides the full-32 account widening.)
	bool alone = true;
	for (auto const& memberDecl: st->structDefinition().members())
	{
		if (!memberDecl || memberDecl->name() == _ma.memberName())
			continue;
		auto const& mo = st->storageOffsetsOfMember(memberDecl->name());
		if (mo.first == off.first)
		{
			alone = false;
			break;
		}
	}

	auto slot = off.first == 0
		? base->slot
		: awst::makeBigUIntBinOp(base->slot, awst::BigUIntBinaryOperator::Add,
			biguintConst(off.first.str()), m_loc);
	return makeLeafAddr(std::move(slot),
		off.second
			? awst::makeIntegerConstant(static_cast<uint64_t>(off.second), m_loc)
			: nullptr,
		size, alone, fieldType);
}

EvmSlotLowering::Addr EvmSlotLowering::elemAddr(
	std::shared_ptr<awst::Expression> _dataBase,
	std::shared_ptr<awst::Expression> _idx,
	Type const* _elemType)
{
	auto l = SlotHandleAccess::layoutFor(_elemType);
	if (l.strideSlots > 1)
	{
		auto slot = awst::makeBigUIntBinOp(std::move(_dataBase),
			awst::BigUIntBinaryOperator::Add,
			awst::makeBigUIntBinOp(std::move(_idx), awst::BigUIntBinaryOperator::Mult,
				biguintConst(std::to_string(l.strideSlots)), m_loc),
			m_loc);
		return makeLeafAddr(std::move(slot), nullptr, 32, true, _elemType);
	}
	if (l.perSlot <= 1)
	{
		auto slot = awst::makeBigUIntBinOp(std::move(_dataBase),
			awst::BigUIntBinaryOperator::Add, std::move(_idx), m_loc);
		return makeLeafAddr(std::move(slot), nullptr, l.size, true, _elemType);
	}
	// Packed elements: slot = base + idx/perSlot, offset = (idx%perSlot)*size.
	auto idxOnce = awst::makeEvalOnce(std::move(_idx), m_loc);
	auto slot = awst::makeBigUIntBinOp(std::move(_dataBase),
		awst::BigUIntBinaryOperator::Add,
		awst::makeBigUIntBinOp(idxOnce, awst::BigUIntBinaryOperator::FloorDiv,
			biguintConst(std::to_string(l.perSlot)), m_loc),
		m_loc);
	auto within = awst::makeBigUIntBinOp(idxOnce, awst::BigUIntBinaryOperator::Mod,
		biguintConst(std::to_string(l.perSlot)), m_loc);
	auto off = awst::makeUInt64BinOp(biguintToU64(std::move(within), m_loc),
		awst::UInt64BinaryOperator::Mult,
		awst::makeIntegerConstant(static_cast<uint64_t>(l.size), m_loc), m_loc);
	return makeLeafAddr(std::move(slot), std::move(off), l.size, false, _elemType);
}

std::shared_ptr<awst::Expression> EvmSlotLowering::coerceToNative(
	std::shared_ptr<awst::Expression> _value, Addr const& _a)
{
	if (!_value || !_a.wtype || _value->wtype == _a.wtype)
		return _value;
	if (_value->wtype == awst::WType::arc4BoolType()
		&& _a.wtype == awst::WType::boolType())
		return awst::makeARC4Decode(std::move(_value), awst::WType::boolType(), m_loc);
	// ARC4 `address` (byte[32] alias) → native account: the backing bytes ARE
	// the address (struct address fields, Morpho's MarketParams).
	if (_a.wtype == awst::WType::accountType())
		if (auto const* sa = dynamic_cast<awst::ARC4StaticArray const*>(_value->wtype);
			sa && sa->arraySize() == 32)
			return awst::makeAsAccount(awst::makeAsBytes(std::move(_value), m_loc), m_loc);
	if (_a.wtype == awst::WType::boolType()
		&& (_value->wtype == awst::WType::uint64Type()
			|| _value->wtype == awst::WType::biguintType()))
		// bool carried numerically (0/1): the word codec's ternary needs a
		// REAL bool condition (frxeth/erc6160 backend rejection).
		return awst::makeNumericCompare(std::move(_value),
			awst::NumericComparison::Ne,
			awst::makeIntegerConstant("0", m_loc,
				_value->wtype == awst::WType::biguintType()
					? awst::WType::biguintType() : awst::WType::uint64Type()),
			m_loc);
	if (_value->wtype && _value->wtype->kind() == awst::WTypeKind::ARC4UIntN)
		_value = awst::makeARC4Decode(std::move(_value), awst::WType::biguintType(), m_loc);
	bool valueNum = _value->wtype == awst::WType::uint64Type()
		|| _value->wtype == awst::WType::biguintType();
	bool targetNum = _a.wtype == awst::WType::uint64Type()
		|| _a.wtype == awst::WType::biguintType();
	if (valueNum && targetNum)
		return TypeCoercion::implicitNumericCast(std::move(_value), _a.wtype, m_loc);
	if (auto const* bw = dynamic_cast<awst::BytesWType const*>(_a.wtype);
		bw && bw->length().has_value())
		return TypeCoercion::relabelUnsizedBytes(std::move(_value), _a.wtype, m_loc);
	return _value;
}

bool EvmSlotLowering::isBytesLike(Type const* _t)
{
	auto const* at = dynamic_cast<ArrayType const*>(_t);
	return at && at->isByteArrayOrString();
}

std::shared_ptr<awst::Expression> EvmSlotLowering::readBytesValue(Addr const& _a)
{
	auto call = awst::makeSubroutineCall(
		awst::SubroutineID{"__puyasol___evm_bytes_read"},
		awst::WType::bytesType(), m_loc);
	awst::pushCallArg(call->args, "__slot", _a.slot);
	auto const* at = dynamic_cast<ArrayType const*>(_a.solType);
	if (at && at->isString())
		return awst::makeReinterpretCast(
			std::move(call), awst::WType::stringType(), m_loc);
	return call;
}

void EvmSlotLowering::writeBytesValue(
	Addr const& _a,
	std::shared_ptr<awst::Expression> _value,
	std::vector<std::shared_ptr<awst::Statement>>& _out)
{
	if (_value && _value->wtype != awst::WType::bytesType())
		_value = awst::makeAsBytes(std::move(_value), m_loc);
	auto call = awst::makeSubroutineCall(
		awst::SubroutineID{"__puyasol___evm_bytes_write"},
		awst::WType::voidType(), m_loc);
	awst::pushCallArg(call->args, "__slot", _a.slot);
	awst::pushCallArg(call->args, "__val", std::move(_value));
	_out.push_back(awst::makeExpressionStatement(std::move(call), m_loc));
}

std::shared_ptr<awst::Expression> EvmSlotLowering::readStructValue(Addr const& _a)
{
	auto const* st = dynamic_cast<StructType const*>(_a.solType);
	auto const* structW = st
		? dynamic_cast<awst::ARC4Struct const*>(m_ctx.typeMapper.map(st)) : nullptr;
	if (!st || !structW)
	{
		Logger::instance().error(
			"--evm-storage-layout: cannot materialise non-struct storage "
			"aggregate as a value", m_loc);
		return nullptr;
	}
	// NESTED struct members recurse (their slots are member-offset bases);
	// arrays/mappings/strings inside a materialised struct stay unsupported.
	bool anyNested = false;
	for (auto const& m: st->structDefinition().members())
	{
		if (!m || !m->type())
			continue;
		if (dynamic_cast<StructType const*>(m->type()))
			anyNested = true;
		else if (isBytesLike(m->type()))
			anyNested = true;   // string/bytes member → recursive path below
		else if (!m->type()->isValueType())
		{
			Logger::instance().error(
				"--evm-storage-layout: cannot materialise struct '"
				+ st->structDefinition().name() + "' as a value — member '"
				+ m->name() + "' is not a value type", m_loc);
			return nullptr;
		}
	}
	if (!anyNested)
		return SlotHandleAccess::readStructElem(
			m_ctx.prePendingStatements, _a.slot, st, structW, m_loc);

	// pin the base once — members read in separate sub-expressions
	std::string nm = "__evm_stv_"
		+ std::to_string(awst::NameGen::next("EvmSlotLowering.structVal"));
	m_ctx.prePendingStatements.push_back(awst::makeAssignmentStatement(
		awst::makeVarExpression(nm, awst::WType::biguintType(), m_loc),
		_a.slot, m_loc));
	auto baseVar = [&]() {
		return awst::makeVarExpression(nm, awst::WType::biguintType(), m_loc);
	};
	auto ns = awst::makeNewStruct(structW, m_loc);
	for (auto const& m: st->structDefinition().members())
	{
		if (!m)
			continue;
		auto const& off = st->storageOffsetsOfMember(m->name());
		Addr fa;
		fa.slot = off.first == 0
			? std::shared_ptr<awst::Expression>(baseVar())
			: std::shared_ptr<awst::Expression>(awst::makeBigUIntBinOp(
				baseVar(), awst::BigUIntBinaryOperator::Add,
				biguintConst(off.first.str()), m_loc));
		fa.solType = m->type();
		if (auto const* nst = dynamic_cast<StructType const*>(m->type()))
		{
			fa.wtype = m_ctx.typeMapper.map(nst);
			auto v = readStructValue(fa);
			if (!v)
				return nullptr;
			ns->values[m->name()] = std::move(v);
			continue;
		}
		if (isBytesLike(m->type()))
		{
			// string/bytes member: EVM short/long format at the member slot
			fa.wtype = m_ctx.typeMapper.map(m->type());
			auto v = readBytesValue(fa);
			awst::WType const* fieldW2 = nullptr;
			for (auto const& [fname, ftype]: structW->fields())
				if (fname == m->name()) { fieldW2 = ftype; break; }
			if (v && fieldW2 && v->wtype != fieldW2)
				v = awst::makeARC4Encode(std::move(v), fieldW2, m_loc);
			if (!v)
				return nullptr;
			ns->values[m->name()] = std::move(v);
			continue;
		}
		fa.byteOffset = off.second
			? awst::makeIntegerConstant(static_cast<uint64_t>(off.second), m_loc)
			: nullptr;
		fa.size = m->type()->storageBytes();
		fa.wtype = m_ctx.typeMapper.map(m->type());
		if (fa.wtype == awst::WType::accountType() && !fa.byteOffset)
			fa.size = fa.size;   // keep declared width inside structs
		// ARC4 struct fields carry ARC4 wtypes — convert the native read.
		awst::WType const* fieldW = nullptr;
		for (auto const& [fname, ftype]: structW->fields())
			if (fname == m->name()) { fieldW = ftype; break; }
		auto v = readValue(fa);
		if (v && fieldW && v->wtype != fieldW)
		{
			if (fieldW->kind() == awst::WTypeKind::ARC4UIntN
				|| fieldW == awst::WType::arc4BoolType())
				v = awst::makeARC4Encode(std::move(v), fieldW, m_loc);
		}
		if (!v)
			return nullptr;
		ns->values[m->name()] = std::move(v);
	}
	return ns;
}

bool EvmSlotLowering::writeStructValue(
	Addr const& _a,
	std::shared_ptr<awst::Expression> _value,
	std::vector<std::shared_ptr<awst::Statement>>& _out)
{
	auto const* st = dynamic_cast<StructType const*>(_a.solType);
	auto const* structW = st
		? dynamic_cast<awst::ARC4Struct const*>(m_ctx.typeMapper.map(st)) : nullptr;
	if (!st || !structW || !_value)
	{
		Logger::instance().error(
			"--evm-storage-layout: unsupported whole-struct storage write", m_loc);
		return false;
	}
	// pin base slot + struct value: members write in separate statements
	std::string bs = "__evm_stw_"
		+ std::to_string(awst::NameGen::next("EvmSlotLowering.structW"));
	_out.push_back(awst::makeAssignmentStatement(
		awst::makeVarExpression(bs, awst::WType::biguintType(), m_loc),
		_a.slot, m_loc));
	std::string vs = bs + "_v";
	auto const* valW = _value->wtype ? _value->wtype
		: static_cast<awst::WType const*>(structW);
	_out.push_back(awst::makeAssignmentStatement(
		awst::makeVarExpression(vs, valW, m_loc), std::move(_value), m_loc));
	auto baseVar = [&]() {
		return awst::makeVarExpression(bs, awst::WType::biguintType(), m_loc);
	};
	auto valVar = [&]() {
		return awst::makeVarExpression(vs, valW, m_loc);
	};
	for (auto const& m: st->structDefinition().members())
	{
		if (!m)
			continue;
		auto const& off = st->storageOffsetsOfMember(m->name());
		Addr fa;
		fa.slot = off.first == 0
			? std::shared_ptr<awst::Expression>(baseVar())
			: std::shared_ptr<awst::Expression>(awst::makeBigUIntBinOp(
				baseVar(), awst::BigUIntBinaryOperator::Add,
				biguintConst(off.first.str()), m_loc));
		fa.solType = m->type();
		awst::WType const* fieldW = nullptr;
		for (auto const& [fname, ftype]: structW->fields())
			if (fname == m->name()) { fieldW = ftype; break; }
		auto field = awst::makeFieldExpression(valVar(), m->name(),
			fieldW ? fieldW : m_ctx.typeMapper.map(m->type()), m_loc);
		if (auto const* nst = dynamic_cast<StructType const*>(m->type()))
		{
			fa.wtype = m_ctx.typeMapper.map(nst);
			if (!writeStructValue(fa, std::move(field), _out))
				return false;
			continue;
		}
		if (isBytesLike(m->type()))
		{
			fa.wtype = m_ctx.typeMapper.map(m->type());
			std::shared_ptr<awst::Expression> fv = std::move(field);
			if (fv->wtype && fv->wtype->kind() != awst::WTypeKind::Bytes
				&& fv->wtype != awst::WType::stringType())
				fv = awst::makeARC4Decode(std::move(fv),
					awst::WType::bytesType(), m_loc);
			writeBytesValue(fa, std::move(fv), _out);
			continue;
		}
		if (!m->type()->isValueType())
		{
			Logger::instance().error(
				"--evm-storage-layout: whole-struct write with non-value member '"
				+ m->name() + "' is not yet supported", m_loc);
			return false;
		}
		fa.byteOffset = off.second
			? awst::makeIntegerConstant(static_cast<uint64_t>(off.second), m_loc)
			: nullptr;
		fa.size = m->type()->storageBytes();
		fa.wtype = m_ctx.typeMapper.map(m->type());
		auto v = coerceToNative(std::move(field), fa);
		if (!v)
			return false;
		writeValue(fa, std::move(v), _out);
	}
	return true;
}

namespace
{
/// (storage bytes, ARC4 bytes, elements-per-slot) for a dynamic array element.
/// ARC4 width differs from storage width for `address` (20 stored, 32 encoded)
/// and for sub-word ints only when the ARC4 alias rounds up — both are read
/// straight off the mapped element wtype so the two never drift.
struct DynElemMetrics
{
	unsigned size = 32;
	unsigned arc4Width = 32;
	unsigned perSlot = 1;
	bool ok = false;
};

DynElemMetrics dynElemMetrics(
	solidity::frontend::Type const* _elemType, awst::WType const* _elemW)
{
	DynElemMetrics m;
	if (!_elemType || !_elemW)
		return m;
	if (_elemType->isDynamicallySized()
		|| dynamic_cast<solidity::frontend::StructType const*>(_elemType)
		|| !_elemType->isValueType())
		return m;
	m.size = puyasol::builder::SlotHandleAccess::layoutFor(_elemType).size;
	if (m.size == 0 || m.size > 32)
		return m;
	m.perSlot = 32u / m.size;
	if (auto const* ui = dynamic_cast<awst::ARC4UIntN const*>(_elemW))
		m.arc4Width = static_cast<unsigned>(ui->n()) / 8u;
	else if (_elemW == awst::WType::accountType()
		|| _elemW->name() == "address")
		m.arc4Width = 32;
	else if (auto const* sa = dynamic_cast<awst::ARC4StaticArray const*>(_elemW))
		m.arc4Width = static_cast<unsigned>(sa->arraySize());
	else if (_elemW == awst::WType::arc4BoolType())
		return m;   // ARC4 bool is BIT-packed — not a byte-width element
	else
		m.arc4Width = m.size;
	if (m.arc4Width < m.size)
		return m;
	m.ok = true;
	return m;
}
} // namespace

std::shared_ptr<awst::Expression> EvmSlotLowering::readArrayValue(
	Addr const& _a, ArrayType const* _at)
{
	if (!_at)
		return nullptr;
	if (_at->isDynamicallySized())
	{
		// runtime-length: [u16 count][32B elems] via __evm_dynarr_read
		auto const* arrW = m_ctx.typeMapper.map(_at);
		auto const* da = dynamic_cast<awst::ARC4DynamicArray const*>(arrW);
		auto met = da ? dynElemMetrics(_at->baseType(), da->elementType())
					  : DynElemMetrics{};
		if (!met.ok || dynamic_cast<awst::ARC4Struct const*>(
				da ? da->elementType() : nullptr))
		{
			Logger::instance().error(
				"--evm-storage-layout: cannot materialise dynamic storage array "
				"with aggregate / bit-packed elements as a value", m_loc);
			return nullptr;
		}
		auto call = awst::makeSubroutineCall(
			awst::SubroutineID{"__puyasol___evm_dynarr_read"},
			awst::WType::bytesType(), m_loc);
		awst::pushCallArg(call->args, "__slot", _a.slot);
		awst::pushCallArg(call->args, "__size",
			awst::makeIntegerConstant(uint64_t{met.size}, m_loc));
		awst::pushCallArg(call->args, "__aw",
			awst::makeIntegerConstant(uint64_t{met.arc4Width}, m_loc));
		awst::pushCallArg(call->args, "__per",
			awst::makeIntegerConstant(uint64_t{met.perSlot}, m_loc));
		return awst::makeReinterpretCast(std::move(call), arrW, m_loc);
	}
	auto lenU = _at->length();
	if (lenU == 0 || lenU > 64)
	{
		Logger::instance().error(
			"--evm-storage-layout: cannot materialise storage array of length "
			+ lenU.str() + " (unrolled reads capped at 64)", m_loc);
		return nullptr;
	}
	unsigned len = static_cast<unsigned>(lenU);
	auto const* elemType = _at->baseType();
	auto const* arrW = m_ctx.typeMapper.map(_at);
	auto arr = awst::makeNewArray(arrW, m_loc);

	std::string tmp = "__evmarr_"
		+ std::to_string(awst::NameGen::next("EvmSlotLowering.arr"));
	m_ctx.prePendingStatements.push_back(awst::makeAssignmentStatement(
		awst::makeVarExpression(tmp, awst::WType::biguintType(), m_loc),
		_a.slot, m_loc));
	auto baseVar = [&]() {
		return awst::makeVarExpression(tmp, awst::WType::biguintType(), m_loc);
	};

	if (auto const* structElem = dynamic_cast<StructType const*>(elemType))
	{
		auto const* structW = dynamic_cast<awst::ARC4Struct const*>(
			m_ctx.typeMapper.map(structElem));
		if (!structW)
			return nullptr;
		auto stride = structElem->storageSize();
		for (unsigned j = 0; j < len; ++j)
		{
			auto elemBase = awst::makeBigUIntBinOp(baseVar(),
				awst::BigUIntBinaryOperator::Add,
				awst::makeIntegerConstant((stride * j).str(), m_loc,
					awst::WType::biguintType()), m_loc);
			arr->values.push_back(SlotHandleAccess::readStructElem(
				m_ctx.prePendingStatements, std::move(elemBase), structElem,
				structW, m_loc));
		}
		return arr;
	}

	awst::WType const* elemW = nullptr;
	if (auto const* sa = dynamic_cast<awst::ARC4StaticArray const*>(arrW))
		elemW = sa->elementType();
	if (!elemW)
		elemW = m_ctx.typeMapper.map(elemType);
	auto l = SlotHandleAccess::layoutFor(elemType);
	for (unsigned j = 0; j < len; ++j)
	{
		auto v = SlotHandleAccess::readScalarElem(
			baseVar(), awst::makeIntegerConstant(j, m_loc, awst::WType::biguintType()),
			l, elemType, m_loc);
		if (elemW == awst::WType::arc4BoolType())
		{
			auto b = awst::makeNumericCompare(std::move(v),
				awst::NumericComparison::Ne,
				awst::makeIntegerConstant("0", m_loc, awst::WType::biguintType()),
				m_loc);
			arr->values.push_back(awst::makeARC4Encode(std::move(b), elemW, m_loc));
		}
		else
			arr->values.push_back(awst::makeARC4Encode(std::move(v), elemW, m_loc));
	}
	return arr;
}

std::shared_ptr<awst::Expression> EvmSlotLowering::readValue(Addr const& _a)
{
	auto word = readSlotWord(_a.slot, m_loc);
	if (_a.size == 32 && !_a.byteOffset)
	{
		// Fast path: a biguint carrier IS the raw word (canonical 256-bit TC);
		// only signed sub-256 needs the sign-extension step. Skipping the
		// pad/extract round-trip saves ~15 bytes of TEAL per read site.
		if (_a.wtype == awst::WType::biguintType())
			return TypeCoercion::signExtendSignedElement(
				std::move(word), _a.solType, m_loc);
		auto raw = awst::makeLeftPadToN(awst::makeAsBytes(std::move(word), m_loc), 32, m_loc);
		return SlotWordCodec::packedBytesToNative(std::move(raw), _a.wtype, _a.solType, 32, m_loc);
	}
	auto wordB = awst::makeLeftPadToN(awst::makeAsBytes(std::move(word), m_loc), 32, m_loc);
	// start = 32 - byteOffset - size (byte window, big-endian word)
	std::shared_ptr<awst::Expression> start = awst::makeIntegerConstant(
		static_cast<uint64_t>(32 - _a.size), m_loc);
	if (_a.byteOffset)
		start = awst::makeUInt64BinOp(std::move(start),
			awst::UInt64BinaryOperator::Sub, _a.byteOffset, m_loc);
	auto raw = awst::makeExtract3(std::move(wordB), std::move(start),
		awst::makeIntegerConstant(static_cast<uint64_t>(_a.size), m_loc), m_loc);
	// PACKED address: the word window holds the EVM-shaped trailing 20 bytes;
	// the high 12 live in the shadow aux slot (zeros when never written —
	// EVM-equivalent). Recombine to the full AVM address.
	if (_a.wtype == awst::WType::accountType() && _a.size == 20)
	{
		auto aux = readSlotWord(
			packedAddrAuxSlot(_a.slot, _a.byteOffset, m_loc), m_loc);
		auto hi = awst::makeExtract(
			awst::makeLeftPadToN(awst::makeAsBytes(std::move(aux), m_loc), 32, m_loc),
			20, 12, m_loc);
		return awst::makeAsAccount(
			awst::makeConcat(std::move(hi), std::move(raw), m_loc), m_loc);
	}
	return SlotWordCodec::packedBytesToNative(
		std::move(raw), _a.wtype, _a.solType, _a.size, m_loc);
}

bool EvmSlotLowering::clearAggregate(
	Addr const& _a,
	Type const* _t,
	std::vector<std::shared_ptr<awst::Statement>>& _out)
{
	if (!_t)
		return false;
	if (isBytesLike(_t))
	{
		writeBytesValue(_a, awst::makeBytesConstant({}, m_loc), _out);
		return true;
	}
	if (auto const* at = dynamic_cast<ArrayType const*>(_t))
	{
		if (at->isDynamicallySized())
		{
			// length 0 + old-tail clear == EVM delete semantics, and the
			// subroutine exists since S1. Aggregate elements' own keccak
			// regions (e.g. string[] entries) are NOT reachable this way.
			auto const* arrWc = m_ctx.typeMapper.map(at);
			auto const* dac = dynamic_cast<awst::ARC4DynamicArray const*>(arrWc);
			auto metc = dac ? dynElemMetrics(at->baseType(), dac->elementType())
							: DynElemMetrics{};
			if (!metc.ok)
			{
				Logger::instance().error(
					"--evm-storage-layout: delete on a dynamic array of "
					"aggregate elements not yet supported", m_loc);
				return false;
			}
			auto call = awst::makeSubroutineCall(
				awst::SubroutineID{"__puyasol___evm_dynarr_write"},
				awst::WType::voidType(), m_loc);
			awst::pushCallArg(call->args, "__slot", _a.slot);
			awst::pushCallArg(call->args, "__val",
				awst::makeBytesConstant({0, 0}, m_loc));
			awst::pushCallArg(call->args, "__size",
				awst::makeIntegerConstant(uint64_t{metc.size}, m_loc));
			awst::pushCallArg(call->args, "__aw",
				awst::makeIntegerConstant(uint64_t{metc.arc4Width}, m_loc));
			awst::pushCallArg(call->args, "__per",
				awst::makeIntegerConstant(uint64_t{metc.perSlot}, m_loc));
			_out.push_back(awst::makeExpressionStatement(std::move(call), m_loc));
			return true;
		}
		auto lenU = at->length();
		if (lenU == 0 || lenU > 64)
		{
			Logger::instance().error(
				"--evm-storage-layout: delete on storage array of length "
				+ lenU.str() + " not supported (cap 64)", m_loc);
			return false;
		}
		unsigned len = static_cast<unsigned>(lenU);
		auto const* elemType = at->baseType();
		// pin the base once
		std::string bs = "__evmcl_"
			+ std::to_string(awst::NameGen::next("EvmSlotLowering.clr"));
		_out.push_back(awst::makeAssignmentStatement(
			awst::makeVarExpression(bs, awst::WType::biguintType(), m_loc),
			_a.slot, m_loc));
		auto baseVar = [&]() {
			return awst::makeVarExpression(bs, awst::WType::biguintType(), m_loc);
		};
		if (elemType->isValueType())
		{
			// the array owns its whole slot span — zero it (packed included)
			auto span = at->storageSize();
			for (solidity::u256 j = 0; j < span; ++j)
				_out.push_back(SlotHandleAccess::writeSlot(
					awst::makeBigUIntBinOp(baseVar(),
						awst::BigUIntBinaryOperator::Add,
						awst::makeIntegerConstant(j.str(), m_loc,
							awst::WType::biguintType()), m_loc),
					awst::makeZero(m_loc, awst::WType::biguintType()), m_loc));
			return true;
		}
		auto stride = elemType->storageSize();
		for (unsigned j = 0; j < len; ++j)
		{
			Addr ea;
			ea.slot = awst::makeBigUIntBinOp(baseVar(),
				awst::BigUIntBinaryOperator::Add,
				awst::makeIntegerConstant((stride * j).str(), m_loc,
					awst::WType::biguintType()), m_loc);
			ea.solType = elemType;
			ea.wtype = m_ctx.typeMapper.map(elemType);
			if (!clearAggregate(ea, elemType, _out))
				return false;
		}
		return true;
	}
	if (auto const* st = dynamic_cast<StructType const*>(_t))
	{
		std::string bs = "__evmcl_"
			+ std::to_string(awst::NameGen::next("EvmSlotLowering.clrS"));
		_out.push_back(awst::makeAssignmentStatement(
			awst::makeVarExpression(bs, awst::WType::biguintType(), m_loc),
			_a.slot, m_loc));
		auto baseVar = [&]() {
			return awst::makeVarExpression(bs, awst::WType::biguintType(), m_loc);
		};
		// zero the struct's slot span (mapping-member slots hold no data, so
		// zeroing them is harmless; EVM delete skips mapping CONTENT, and so
		// do we — the keccak regions are untouched)
		auto span = st->storageSize();
		if (span > 64)
		{
			Logger::instance().error(
				"--evm-storage-layout: delete on struct spanning "
				+ span.str() + " slots not supported (cap 64)", m_loc);
			return false;
		}
		for (solidity::u256 j = 0; j < span; ++j)
			_out.push_back(SlotHandleAccess::writeSlot(
				awst::makeBigUIntBinOp(baseVar(),
					awst::BigUIntBinaryOperator::Add,
					awst::makeIntegerConstant(j.str(), m_loc,
						awst::WType::biguintType()), m_loc),
				awst::makeZero(m_loc, awst::WType::biguintType()), m_loc));
		// dynamic members: clear their keccak-region data too
		for (auto const& m: st->structDefinition().members())
		{
			if (!m || !m->type())
				continue;
			auto const* mt = m->type();
			bool needsRegion = isBytesLike(mt)
				|| dynamic_cast<ArrayType const*>(mt)
				|| dynamic_cast<StructType const*>(mt);
			if (dynamic_cast<solidity::frontend::MappingType const*>(mt))
				continue;
			if (!needsRegion || mt->isValueType())
				continue;
			auto const& off = st->storageOffsetsOfMember(m->name());
			Addr fa;
			fa.slot = off.first == 0
				? std::shared_ptr<awst::Expression>(baseVar())
				: std::shared_ptr<awst::Expression>(awst::makeBigUIntBinOp(
					baseVar(), awst::BigUIntBinaryOperator::Add,
					awst::makeIntegerConstant(off.first.str(), m_loc,
						awst::WType::biguintType()), m_loc));
			fa.solType = mt;
			fa.wtype = m_ctx.typeMapper.map(mt);
			// slot span already zeroed above; for bytes/dynarrays that also
			// zeroed the length/short word — only LONG-form data remains,
			// which is unreachable once the length word is zero. EVM leaves
			// it too (SSTORE-refund era cleared, but reads cannot observe
			// the difference through Solidity). Match observable semantics.
			(void)fa;
		}
		return true;
	}
	Logger::instance().error(
		"--evm-storage-layout: delete on this aggregate shape not yet "
		"supported", m_loc);
	return false;
}

bool EvmSlotLowering::writeArrayValue(
	Addr const& _a,
	ArrayType const* _at,
	std::shared_ptr<awst::Expression> _value,
	std::vector<std::shared_ptr<awst::Statement>>& _out)
{
	if (!_at || !_value)
		return false;
	if (_at->isDynamicallySized())
	{
		auto const* arrWw = m_ctx.typeMapper.map(_at);
		auto const* daw = dynamic_cast<awst::ARC4DynamicArray const*>(arrWw);
		auto metw = daw ? dynElemMetrics(_at->baseType(), daw->elementType())
						: DynElemMetrics{};
		if (!metw.ok)
		{
			Logger::instance().error(
				"--evm-storage-layout: dynamic array assignment with "
				"aggregate / bit-packed elements not yet supported", m_loc);
			return false;
		}
		if (_value->wtype != awst::WType::bytesType())
			_value = awst::makeReinterpretCast(
				std::move(_value), awst::WType::bytesType(), m_loc);
		auto call = awst::makeSubroutineCall(
			awst::SubroutineID{"__puyasol___evm_dynarr_write"},
			awst::WType::voidType(), m_loc);
		awst::pushCallArg(call->args, "__slot", _a.slot);
		awst::pushCallArg(call->args, "__val", std::move(_value));
		awst::pushCallArg(call->args, "__size",
			awst::makeIntegerConstant(uint64_t{metw.size}, m_loc));
		awst::pushCallArg(call->args, "__aw",
			awst::makeIntegerConstant(uint64_t{metw.arc4Width}, m_loc));
		awst::pushCallArg(call->args, "__per",
			awst::makeIntegerConstant(uint64_t{metw.perSlot}, m_loc));
		_out.push_back(awst::makeExpressionStatement(std::move(call), m_loc));
		return true;
	}

	auto lenU = _at->length();
	if (lenU == 0 || lenU > 64)
	{
		Logger::instance().error(
			"--evm-storage-layout: cannot assign storage array of length "
			+ lenU.str() + " (unrolled writes capped at 64)", m_loc);
		return false;
	}
	unsigned len = static_cast<unsigned>(lenU);
	auto const* elemType = _at->baseType();
	auto const* arrW = _value->wtype;
	awst::WType const* elemW = nullptr;
	if (auto const* sa = dynamic_cast<awst::ARC4StaticArray const*>(arrW))
		elemW = sa->elementType();
	else if (auto const* da = dynamic_cast<awst::ARC4DynamicArray const*>(arrW))
		elemW = da->elementType();

	// pin base + value: both feed one statement per element
	std::string bs = "__evmaw_b_"
		+ std::to_string(awst::NameGen::next("EvmSlotLowering.arrWB"));
	_out.push_back(awst::makeAssignmentStatement(
		awst::makeVarExpression(bs, awst::WType::biguintType(), m_loc),
		_a.slot, m_loc));
	std::string vs = "__evmaw_v_"
		+ std::to_string(awst::NameGen::next("EvmSlotLowering.arrWV"));
	_out.push_back(awst::makeAssignmentStatement(
		awst::makeVarExpression(vs, arrW, m_loc), std::move(_value), m_loc));
	auto baseVar = [&]() {
		return awst::makeVarExpression(bs, awst::WType::biguintType(), m_loc);
	};
	auto valVar = [&]() {
		return awst::makeVarExpression(vs, arrW, m_loc);
	};
	auto elemAt = [&](unsigned j) {
		return awst::makeIndexExpression(valVar(),
			awst::makeIntegerConstant(static_cast<uint64_t>(j), m_loc),
			elemW, m_loc);
	};

	if (auto const* structElem = dynamic_cast<StructType const*>(elemType))
	{
		auto stride = structElem->storageSize();
		for (unsigned j = 0; j < len; ++j)
		{
			Addr ea;
			ea.slot = awst::makeBigUIntBinOp(baseVar(),
				awst::BigUIntBinaryOperator::Add,
				awst::makeIntegerConstant((stride * j).str(), m_loc,
					awst::WType::biguintType()), m_loc);
			ea.solType = structElem;
			ea.wtype = elemW;
			if (!writeStructValue(ea, elemAt(j), _out))
				return false;
		}
		return true;
	}
	if (isBytesLike(elemType))
	{
		for (unsigned j = 0; j < len; ++j)
		{
			Addr ea;
			ea.slot = awst::makeBigUIntBinOp(baseVar(),
				awst::BigUIntBinaryOperator::Add,
				awst::makeIntegerConstant(static_cast<uint64_t>(j), m_loc,
					awst::WType::biguintType()), m_loc);
			ea.solType = elemType;
			ea.wtype = m_ctx.typeMapper.map(elemType);
			auto ev = elemAt(j);
			std::shared_ptr<awst::Expression> fv = std::move(ev);
			if (fv->wtype && fv->wtype->kind() != awst::WTypeKind::Bytes
				&& fv->wtype != awst::WType::stringType())
				fv = awst::makeARC4Decode(std::move(fv),
					awst::WType::bytesType(), m_loc);
			writeBytesValue(ea, std::move(fv), _out);
		}
		return true;
	}
	if (!elemType->isValueType())
	{
		Logger::instance().error(
			"--evm-storage-layout: fixed array assignment with aggregate "
			"elements of this shape not yet supported", m_loc);
		return false;
	}
	auto l = SlotHandleAccess::layoutFor(elemType);
	auto const* nativeW = m_ctx.typeMapper.map(elemType);
	for (unsigned j = 0; j < len; ++j)
	{
		auto ev = elemAt(j);
		std::shared_ptr<awst::Expression> nat = std::move(ev);
		if (nat->wtype != nativeW)
			nat = awst::makeARC4Decode(std::move(nat), nativeW, m_loc);
		auto canonical = awst::makeAsBiguint(
			SlotWordCodec::nativeToPackedBytes(std::move(nat), nativeW, 32, m_loc),
			m_loc);
		SlotHandleAccess::writeScalarElem(_out, baseVar(),
			awst::makeIntegerConstant(static_cast<uint64_t>(j), m_loc,
				awst::WType::biguintType()),
			l, std::move(canonical), m_loc);
	}
	return true;
}

void EvmSlotLowering::writeValue(
	Addr const& _a,
	std::shared_ptr<awst::Expression> _value,
	std::vector<std::shared_ptr<awst::Statement>>& _out)
{
	// The value's carrier can lag the slot's declared wtype (storing
	// `new helper()` leaves an APPLICATION where the contract-typed state
	// var maps to account — 15 slot-lane fixtures died in puya on
	// asBytes(application)). The canonical conversions already live in
	// coerceForAssignment; route through them once, up front.
	if (_value && _a.wtype && _value->wtype != _a.wtype
		&& _value->wtype != awst::WType::biguintType())
		_value = TypeCoercion::coerceForAssignment(std::move(_value), _a.wtype, m_loc);
	if (_a.size == 32 && !_a.byteOffset)
	{
		// Fast path: a biguint value is already the canonical word.
		if (_value && _value->wtype == awst::WType::biguintType())
		{
			_out.push_back(SlotHandleAccess::writeSlot(
				_a.slot, std::move(_value), m_loc));
			return;
		}
		auto packed = SlotWordCodec::nativeToPackedBytes(
			std::move(_value), _a.wtype, 32, m_loc);
		_out.push_back(SlotHandleAccess::writeSlot(
			_a.slot, awst::makeAsBiguint(std::move(packed), m_loc), m_loc));
		return;
	}
	// Sub-word: read-modify-write the word. Both slot uses live in ONE
	// statement (the write call), so EvalOnce is safe.
	auto slotOnce = awst::makeEvalOnce(_a.slot, m_loc);
	// PACKED address: stash the high 12 bytes in the shadow aux slot (the
	// word window keeps the EVM-shaped trailing 20 for asm fidelity). The
	// value feeds TWO statements — pin it to a named temp first.
	if (_a.wtype == awst::WType::accountType() && _a.size == 20)
	{
		std::string nm = "__evm_addr_"
			+ std::to_string(awst::NameGen::next("EvmSlotLowering.addrPin"));
		auto const* pinW = _value->wtype ? _value->wtype : _a.wtype;
		_out.push_back(awst::makeAssignmentStatement(
			awst::makeVarExpression(nm, pinW, m_loc), std::move(_value), m_loc));
		_value = awst::makeVarExpression(nm, pinW, m_loc);
		auto hi = awst::makeExtract(
			awst::makeAsBytes(awst::makeVarExpression(nm, pinW, m_loc), m_loc),
			0, 12, m_loc);
		_out.push_back(SlotHandleAccess::writeSlot(
			packedAddrAuxSlot(slotOnce, _a.byteOffset, m_loc),
			awst::makeAsBiguint(std::move(hi), m_loc), m_loc));
	}
	auto packed = SlotWordCodec::nativeToPackedBytes(
		std::move(_value), _a.wtype, _a.size, m_loc);
	auto wordB = awst::makeLeftPadToN(
		awst::makeAsBytes(readSlotWord(slotOnce, m_loc), m_loc), 32, m_loc);
	std::shared_ptr<awst::Expression> start = awst::makeIntegerConstant(
		static_cast<uint64_t>(32 - _a.size), m_loc);
	if (_a.byteOffset)
		start = awst::makeUInt64BinOp(std::move(start),
			awst::UInt64BinaryOperator::Sub, _a.byteOffset, m_loc);
	auto newWord = awst::makeReplace3(
		std::move(wordB), std::move(start), std::move(packed), m_loc);
	_out.push_back(SlotHandleAccess::writeSlot(
		slotOnce, awst::makeAsBiguint(std::move(newWord), m_loc), m_loc));
}

} // namespace puyasol::builder::sol_ast
