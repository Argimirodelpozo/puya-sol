/// @file EvmSlotValueLowering.cpp
/// Reading, materialising, writing, and clearing values after
/// EvmSlotLowering has resolved their storage word addresses.

#include "builder/sol-ast/EvmSlotLowering.h"
#include "builder/AwstShorthand.h"
#include "builder/BuildArtifacts.h"
#include "builder/sol-ast/Context.h"
#include "builder/codec/EvmValueCodec.h"
#include "builder/storage/SlotHandleAccess.h"
#include "builder/storage/SlotWordCodec.h"
#include "builder/sol-types/TypeCoercion.h"
#include "builder/sol-types/Arc4Defaults.h"
#include "awst/NameGen.h"
#include "Logger.h"

#include <libsolutil/Keccak256.h>
// Uses solc AST/Type definitions directly; the hub headers only
// forward-declare them now.
#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/Types.h>

#include <algorithm>

namespace puyasol::builder::sol_ast
{

using namespace solidity::frontend;

namespace
{
/// Shadow slot carrying the HIGH 12 bytes of a PACKED address (a 20-byte
/// window cannot hold a 32-byte AVM address; the word keeps the EVM-shaped
/// trailing-20 so asm sees EVM layout, and Solidity reads recombine).
/// Domain-separated from the ordinary slot derivations.
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

namespace
{
/// ARC4Decode target for a bytes-like leaf: puya type-checks the decode, so a
/// Solidity `string` (arc4 len+utf8[]) must decode to `string`, not `bytes`.
awst::WType const* bytesLikeDecodeTarget(solidity::frontend::Type const* _t)
{
	if (auto const* at = dynamic_cast<solidity::frontend::ArrayType const*>(_t))
		if (at->isString())
			return awst::WType::stringType();
	return awst::WType::bytesType();
}
} // namespace

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

std::shared_ptr<awst::Expression> EvmSlotLowering::materializeRefValue(
	eb::ContractContext& _ctx,
	Context& _scope,
	std::shared_ptr<awst::Expression> _value,
	solidity::frontend::Type const* _srcSolType,
	awst::WType const* _targetW,
	awst::SourceLocation const& _loc)
{
	if (!_value || !_targetW
		|| _value->wtype != awst::WType::biguintType())
		return _value;
	auto k = _targetW->kind();
	if (k != awst::WTypeKind::ARC4Struct
		&& k != awst::WTypeKind::ARC4StaticArray
		&& k != awst::WTypeKind::ARC4DynamicArray
		&& !(isBytesLike(_srcSolType)
			&& (k == awst::WTypeKind::Bytes || _targetW == awst::WType::stringType())))
		return _value;
	if (!_srcSolType
		|| !_srcSolType->dataStoredIn(solidity::frontend::DataLocation::Storage))
		return _value;
	EvmSlotLowering low(_ctx, _scope, _loc);
	Addr a;
	a.slot = std::move(_value);
	a.solType = _srcSolType;
	a.wtype = _targetW;
	if (isBytesLike(_srcSolType)) return low.readBytesValue(a);
	if (dynamic_cast<StructType const*>(_srcSolType))
		return low.readStructValue(a);
	if (auto const* at = dynamic_cast<ArrayType const*>(_srcSolType))
		return low.readArrayValue(a, at);
	return a.slot;
}

std::shared_ptr<awst::Expression> EvmSlotLowering::materializeRefValue(
	eb::ContractContext& _ctx,
	std::shared_ptr<awst::Expression> _value,
	solidity::frontend::Type const* _srcSolType,
	awst::WType const* _targetW,
	awst::SourceLocation const& _loc)
{
	if (!_ctx.currentScope)
		return _value;
	return materializeRefValue(
		_ctx, *_ctx.currentScope, std::move(_value), _srcSolType, _targetW, _loc);
}

std::shared_ptr<awst::Expression> EvmSlotLowering::readStructValue(Addr const& _a)
{
	ValueDir d{false, nullptr, m_ctx.preEffects()};
	if (!lowerStructValue(_a, d))
		return nullptr;
	return d.value;
}

bool EvmSlotLowering::writeStructValue(
	Addr const& _a,
	std::shared_ptr<awst::Expression> _value,
	std::vector<std::shared_ptr<awst::Statement>>& _out)
{
	ValueDir d{true, std::move(_value), _out};
	return lowerStructValue(_a, d);
}

bool EvmSlotLowering::lowerStructValue(Addr const& _a, ValueDir& _d)
{
	auto const* st = dynamic_cast<StructType const*>(_a.solType);
	auto const* structW = st
		? dynamic_cast<awst::ARC4Struct const*>(m_ctx.typeMapper.map(st)) : nullptr;
	if (!st || !structW || (_d.write && !_d.value))
	{
		Logger::instance().error(_d.write
			? "--evm-storage-layout: unsupported whole-struct storage write"
			: "--evm-storage-layout: cannot materialise non-struct storage "
				"aggregate as a value", m_loc);
		return false;
	}

	// Named-layout assembly slots are rebound to a host-specific dispatcher
	// after lowering; keep those calls in the contract body for that rewrite.
	if (!m_ctx.typeMapper.profile().evmStorageLayout
		|| (_d.write && !awst::structurallyEquivalent(_d.value->wtype, structW)))
		return lowerStructMembers(_a, _d);
	// The slot runtime is unit-wide, just like recursive aggregate clearing.
	// Key by solc's located type, not a struct name or a particular state root.
	// Bytes are value snapshots; neither reads nor writes share mutable args.
	auto& subs = m_ctx.typeMapper.artifacts().bufferSubroutines;
	std::string key = std::string(_d.write ? "storage-write:" : "storage-read:") + st->identifier();
	auto [entry, fresh] = subs.try_emplace(key);
	if (fresh)
	{
		std::string id = std::string(_d.write ? "__puyasol_storage_encode_" : "__puyasol_storage_decode_")
			+ std::to_string(subs.size());
		auto const* bytes = awst::WType::bytesType();
		std::vector<awst::SubroutineArgument> args{{"slot", awst::WType::biguintType(), m_loc}};
		if (_d.write) args.emplace_back("value", bytes, m_loc);
		auto sub = awst::makeSubroutine(id, id, std::move(args),
			_d.write ? awst::WType::voidType() : bytes, awst::makeBlock(m_loc), false, m_loc);
		sub->inlineOpt = false;
		entry->second = sub;
		// Nested reads queue their temporaries into the active effect frame.
		// Capture them inside the helper, never into its first caller's body.
		auto lowered = m_ctx.lowerOperand([&]() {
			Addr address;
			address.slot = awst::makeVarExpression("slot", awst::WType::biguintType(), m_loc);
			address.solType = st;
			address.wtype = structW;
			ValueDir direction{_d.write, _d.write ? awst::makeReinterpretCast(
				awst::makeVarExpression("value", bytes, m_loc), structW, m_loc) : nullptr, m_ctx.preEffects()};
			if (!lowerStructMembers(address, direction)) return false;
			direction.out.push_back(awst::makeReturnStatement(
				_d.write ? nullptr : awst::makeAsBytes(std::move(direction.value), m_loc), m_loc));
			return true;
		}, false);
		if (!lowered.value) return false;
		assert(lowered.effects.post.empty());
		sub->body->body = std::move(lowered.effects.pre);
	}
	auto call = awst::makeSubroutineCall(awst::SubroutineID{entry->second->id},
		entry->second->returnType, m_loc);
	awst::pushCallArg(call->args, _a.slot);
	if (_d.write)
	{
		awst::pushCallArg(call->args, awst::makeAsBytes(std::move(_d.value), m_loc));
		_d.out.push_back(awst::makeExpressionStatement(std::move(call), m_loc));
	}
	else
		_d.value = awst::makeReinterpretCast(std::move(call), structW, m_loc);
	return true;
}

bool EvmSlotLowering::lowerStructMembers(Addr const& _a, ValueDir& _d)
{
	auto const* st = dynamic_cast<StructType const*>(_a.solType);
	auto const* structW = dynamic_cast<awst::ARC4Struct const*>(m_ctx.typeMapper.map(st));
	// pin the base once — members read in separate sub-expressions / write
	// in separate statements
	std::string bs = (_d.write ? "__evm_stw_" : "__evm_stv_")
		+ std::to_string(awst::NameGen::next(
			_d.write ? "EvmSlotLowering.structW" : "EvmSlotLowering.structVal"));
	_d.out.push_back(awst::makeAssignmentStatement(
		awst::makeVarExpression(bs, awst::WType::biguintType(), m_loc),
		_a.slot, m_loc));
	auto baseVar = [&]() {
		return awst::makeVarExpression(bs, awst::WType::biguintType(), m_loc);
	};
	// a write pins the struct value too; a read collects into a NewStruct
	std::string vs = bs + "_v";
	awst::WType const* valW = nullptr;
	std::shared_ptr<awst::NewStruct> ns;
	if (_d.write)
	{
		valW = _d.value->wtype ? _d.value->wtype
			: static_cast<awst::WType const*>(structW);
		_d.out.push_back(awst::makeAssignmentStatement(
			awst::makeVarExpression(vs, valW, m_loc), std::move(_d.value), m_loc));
	}
	else
		ns = awst::makeNewStruct(structW, m_loc);
	auto valVar = [&]() {
		return awst::makeVarExpression(vs, valW, m_loc);
	};
	std::map<solidity::u256, std::shared_ptr<awst::Expression>> words;
	for (auto const& m: st->structDefinition().members())
	{
		if (!m)
			continue;
		// Mapping content lives at keccak-derived slots and is never copied;
		// Solidity skips mapping members on whole-struct assignment too.
		if (dynamic_cast<MappingType const*>(m->type()))
			continue;
		auto fa = memberAddr(baseVar(), st, m->name(), m->type());
		// ARC4 struct fields carry ARC4 wtypes
		awst::WType const* fieldW = awst::structFieldType(structW, m->name());
		if (_d.write)
		{
			auto field = awst::makeFieldExpression(valVar(), m->name(),
				fieldW ? fieldW : m_ctx.typeMapper.map(m->type()), m_loc);
			if (!writeAny(fa, m->type(), std::move(field), _d.out))
				return false;
			continue;
		}
		std::shared_ptr<awst::Expression> v;
		if (m->type()->isValueType())
		{
			// One main-word read per occupied slot. Decoding still uses the
			// same declared-type leaf policy as a direct field/element read.
			auto offset = st->storageOffsetsOfMember(m->name()).first;
			auto& word = words[offset];
			if (!word)
			{
				word = awst::makeVarExpression(bs + "_word_" + offset.str(), awst::WType::biguintType(), m_loc);
				_d.out.push_back(awst::makeAssignmentStatement(word, readSlotWord(fa.slot, m_loc), m_loc));
			}
			v = readValue(fa, word);
		}
		else v = readAny(fa, m->type());
		if (!v) return false;
		v = codec::valueToArc4(m_ctx.typeMapper, m->type(), std::move(v), fieldW, m_loc);
		ns->values[m->name()] = std::move(v);
	}
	if (!_d.write)
		_d.value = ns;
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
	unsigned lanesPerElem = 1;   // fixed-array / uniform-struct elements: the
	                             // element is this many LANES, identical in
	                             // slot layout and ARC4 concatenation order
	bool bitPacked = false;       // fixed bool[N]: byte lanes in EVM storage,
	                              // MSB-first bits in each ARC4 element
	bool ok = false;
};

DynElemMetrics dynElemMetrics(
	solidity::frontend::Type const* _elemType, awst::WType const* _elemW)
{
	DynElemMetrics m;
	if (!_elemType || !_elemW)
		return m;
	// FIXED-array element (uint256[2], uint24[3]): lanes are the inner
	// scalars. Accepted when the lanes are full words (any element slot
	// count) or the whole element packs into ONE slot — both keep the global
	// lane index aligned with the per-slot packing math.
	if (auto const* fat = dynamic_cast<solidity::frontend::ArrayType const*>(_elemType);
		fat && !fat->isDynamicallySized() && !fat->isByteArrayOrString())
	{
		auto const* sa = dynamic_cast<awst::ARC4StaticArray const*>(_elemW);
		if (!sa)
			return m;
		// Solidity gives every bool one storage byte, while ARC4 packs a
		// bool[N] into ceil(N/8) bytes, MSB-first. A fixed array used as a
		// dynamic-array element starts on a fresh EVM slot, so support the
		// one-slot form here and let the runtime codec reset its ARC4 bit
		// cursor at every outer-element boundary.
		if (dynamic_cast<solidity::frontend::BoolType const*>(fat->baseType())
			&& sa->elementType() == awst::WType::arc4BoolType())
		{
			auto lanesU = fat->length();
			if (lanesU == 0 || lanesU > 32)
				return m;
			unsigned lanes = static_cast<unsigned>(lanesU);
			m.size = 1;
			m.arc4Width = (lanes + 7u) / 8u;
			m.perSlot = lanes;
			m.lanesPerElem = lanes;
			m.bitPacked = true;
			m.ok = true;
			return m;
		}
		auto inner = dynElemMetrics(fat->baseType(), sa->elementType());
		if (!inner.ok || inner.bitPacked)
			return m;
		auto lanesU = fat->length();
		if (lanesU == 0 || lanesU > 64
			|| inner.lanesPerElem > 64 / static_cast<unsigned>(lanesU))
			return m;
		unsigned lanes = static_cast<unsigned>(lanesU);
		if (inner.lanesPerElem == 1
			&& inner.size != 32 && lanes * inner.size > 32)
			return m;
		m.size = inner.size;
		m.arc4Width = inner.arc4Width;
		// A nested fixed array starts on the same storage alignment as its
		// immediate child. Preserve that child's lanes-per-slot and multiply
		// only the logical lanes in one outer element. This recursively flattens
		// uint8[2][3][4] (and full-word equivalents) without assuming a rank.
		m.perSlot = inner.lanesPerElem == 1
			? (inner.size == 32 ? 1 : lanes)
			: inner.perSlot;
		m.lanesPerElem = lanes * inner.lanesPerElem;
		m.ok = true;
		return m;
	}
	// Uniform STRUCT element (struct S { uint256 a; uint256 b; }): every
	// member a full-slot 32-byte value whose ARC4 encoding is its canonical
	// word — the element is a plain word concatenation.
	if (auto const* st = dynamic_cast<solidity::frontend::StructType const*>(_elemType))
	{
		auto const* sw = dynamic_cast<awst::ARC4Struct const*>(_elemW);
		if (!sw)
			return m;
		unsigned lanes = 0;
		for (auto const& mem: st->structDefinition().members())
		{
			if (!mem || !mem->type())
				return m;
			auto const* mt = mem->type();
			if (!mt->isValueType() || mt->storageBytes() != 32)
				return m;
			if (dynamic_cast<solidity::frontend::FixedBytesType const*>(mt) == nullptr
				&& mt->category() != solidity::frontend::Type::Category::Integer)
				return m;
			++lanes;
		}
		if (lanes == 0 || lanes > 64)
			return m;
		m.size = 32;
		m.arc4Width = 32;
		m.perSlot = 1;
		m.lanesPerElem = lanes;
		m.ok = true;
		return m;
	}
	if (_elemType->isDynamicallySized()
		|| !_elemType->isValueType())
		return m;
	m.size = puyasol::builder::SlotHandleAccess::layoutFor(_elemType).size;
	if (m.size == 0 || m.size > 32)
		return m;
	bool const isAddress = _elemW == awst::WType::accountType()
		|| _elemW->name() == "address";
	// An address ELEMENT of a dynamic array occupies a whole slot on its own
	// (20 bytes packs 1-per-slot either way), and the writer stores the full
	// 32-byte Algorand account there. Slicing the EVM 20-byte width back out
	// silently dropped the account's high 12 bytes, so the aggregate read
	// disagreed with the element-wise read of the SAME storage. Addresses are
	// Algorand accounts here, so take the whole word.
	//
	// Deliberately scoped to the alone-in-a-slot case — the same rule the
	// packed-slot-address getter uses. An address PACKED beside other fields
	// keeps its 20-byte EVM width, so struct layouts stay asm-compatible.
	if (isAddress && m.size == 20)
		m.size = 32;
	m.perSlot = 32u / m.size;
	if (auto const* ui = dynamic_cast<awst::ARC4UIntN const*>(_elemW))
		m.arc4Width = static_cast<unsigned>(ui->n()) / 8u;
	else if (isAddress)
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

struct DynamicArrayChain
{
	unsigned depth = 0;
	DynElemMetrics leafMetrics;
};

DynamicArrayChain dynamicArrayChain(
	solidity::frontend::ArrayType const* _root,
	awst::WType const* _rootW)
{
	using namespace solidity::frontend;
	DynamicArrayChain result;
	auto const* array = _root;
	auto const* arrayW = dynamic_cast<awst::ARC4DynamicArray const*>(_rootW);
	while (array && arrayW && array->isDynamicallySized()
		&& !array->isByteArrayOrString())
	{
		++result.depth;
		auto const* element = array->baseType();
		auto const* elementW = arrayW->elementType();
		auto const* nested = dynamic_cast<ArrayType const*>(element);
		if (!nested || !nested->isDynamicallySized()
			|| nested->isByteArrayOrString())
		{
			result.leafMetrics = dynElemMetrics(element, elementW);
			break;
		}
		array = nested;
		arrayW = dynamic_cast<awst::ARC4DynamicArray const*>(elementW);
	}
	return result;
}

/// Element-metric arguments shared by the runtime dynamic-array codecs.
void pushDynElemMetricArgs(
	std::vector<awst::CallArg>& _args,
	DynElemMetrics const& _m,
	awst::SourceLocation const& _loc)
{
	awst::pushCallArg(_args, "__size",
		awst::makeIntegerConstant(uint64_t{_m.size}, _loc));
	awst::pushCallArg(_args, "__aw",
		awst::makeIntegerConstant(uint64_t{_m.arc4Width}, _loc));
	awst::pushCallArg(_args, "__per",
		awst::makeIntegerConstant(uint64_t{_m.perSlot}, _loc));
	awst::pushCallArg(_args, "__mul",
		awst::makeIntegerConstant(uint64_t{_m.lanesPerElem}, _loc));
	awst::pushCallArg(_args, "__bp",
		awst::makeIntegerConstant(uint64_t{_m.bitPacked}, _loc));
}
} // namespace

std::shared_ptr<awst::Expression> EvmSlotLowering::readArrayValue(
	Addr const& _a, ArrayType const* _at)
{
	ValueDir d{false, nullptr, m_ctx.preEffects()};
	if (!lowerArrayValue(_a, _at, d))
		return nullptr;
	return d.value;
}

bool EvmSlotLowering::writeArrayValue(
	Addr const& _a,
	ArrayType const* _at,
	std::shared_ptr<awst::Expression> _value,
	std::vector<std::shared_ptr<awst::Statement>>& _out)
{
	if (!_value)
		return false;
	ValueDir d{true, std::move(_value), _out};
	return lowerArrayValue(_a, _at, d);
}

bool EvmSlotLowering::lowerArrayValue(
	Addr const& _a, ArrayType const* _at, ValueDir& _d)
{
	if (!_at)
		return false;
	if (!_at->isDynamicallySized())
		return lowerFixedArray(_a, _at, _d);
	// Consecutive dynamic-array layers use one depth-driven recursive
	// codec. The leaf metrics describe the first non-dynamic-array element,
	// so T[], T[][] and T[][][] differ only by the depth argument.
	auto const* arrW = m_ctx.typeMapper.map(_at);
	auto chain = dynamicArrayChain(_at, arrW);
	// both codecs consume the written value as raw ARC4 bytes
	if (_d.write && _d.value->wtype != awst::WType::bytesType())
		_d.value = awst::makeReinterpretCast(
			std::move(_d.value), awst::WType::bytesType(), m_loc);
	if (chain.depth == 0 || !chain.leafMetrics.ok)
		return lowerDynArrayGeneric(_a, _at, arrW, _d);
	std::string const helper = chain.depth == 1
		? (_d.write ? "__puyasol___evm_dynarr_write" : "__puyasol___evm_dynarr_read")
		: (_d.write
			? "__puyasol___evm_dynarr_recursive_write"
			: "__puyasol___evm_dynarr_recursive_read");
	auto call = awst::makeSubroutineCall(awst::SubroutineID{helper},
		_d.write ? awst::WType::voidType() : awst::WType::bytesType(), m_loc);
	awst::pushCallArg(call->args, "__slot", _a.slot);
	if (_d.write)
		awst::pushCallArg(call->args, "__val", std::move(_d.value));
	if (chain.depth > 1)
		awst::pushCallArg(call->args, "__depth",
			awst::makeIntegerConstant(uint64_t{chain.depth}, m_loc));
	pushDynElemMetricArgs(call->args, chain.leafMetrics, m_loc);
	if (_d.write)
		_d.out.push_back(awst::makeExpressionStatement(std::move(call), m_loc));
	else
		_d.value = awst::makeReinterpretCast(std::move(call), arrW, m_loc);
	return true;
}

bool EvmSlotLowering::lowerDynArrayGeneric(
	Addr const& _a, ArrayType const* _at, awst::WType const* _arrW, ValueDir& _d)
{
	// Mixed aggregate tree (e.g. T[][2][], string[], or struct-with-
	// array[]): emit the type-directed loop here. The homogeneous
	// dynamic-chain codec remains the compact fast path, while this
	// fallback recursively delegates each child to readAny/writeAny.
	auto const* elemType = _at->baseType();
	bool const dynamic = _at->isDynamicallySized();
	unsigned const headerBytes = dynamic ? 2 : 0;
	unsigned sourceCount = dynamic ? 0 : static_cast<unsigned>(_at->length());
	if (_d.write && !dynamic)
	{
		if (auto const* fixed = dynamic_cast<awst::ARC4StaticArray const*>(_d.value->wtype))
			sourceCount = static_cast<unsigned>(fixed->arraySize());
		_d.value = awst::makeAsBytes(std::move(_d.value), m_loc);
	}
	if (dynamic_cast<MappingType const*>(elemType))
	{
		Logger::instance().error(_d.write
			? "--evm-storage-layout: whole assignment of mapping-element arrays "
				"is not defined"
			: "--evm-storage-layout: mappings cannot be materialised as array values",
			m_loc);
		return false;
	}
	auto const* elemArc4 = m_ctx.typeMapper.mapSolTypeToARC4(elemType);
	bool const elemDynamic = arc4IsDynamic(elemArc4);
	bool const bitPacked = elemArc4 == awst::WType::arc4BoolType();
	int const elemSize = computeEncodedElementSize(elemArc4).fixedBytes<int>().value_or(0);
	if (!elemArc4 || (!elemDynamic && !bitPacked && elemSize <= 0))
	{
		Logger::instance().error(
			"--evm-storage-layout: array element has no representable ARC4 encoding",
			m_loc);
		return false;
	}

	int uid = awst::NameGen::next(_d.write
		? "EvmSlotLowering.genericArrayWrite" : "EvmSlotLowering.genericArrayRead");
	auto name = [&](char const* tag) {
		return std::string(_d.write ? "__evm_gaw_" : "__evm_gar_") + tag + "_"
			+ std::to_string(uid);
	};
	std::string slotN = name("slot"), dataN = name("base"), nN = name("n"),
		iN = name("i");
	auto bv = [&](std::string const& n) {
		return awst::makeVarExpression(n, awst::WType::biguintType(), m_loc);
	};
	auto uv = [&](std::string const& n) { return shorthand::u64Var(n, m_loc); };
	auto xv = [&](std::string const& n) { return shorthand::bytesVar(n, m_loc); };
	auto u64c = [&](uint64_t v) { return shorthand::u64(v, m_loc); };
	auto toU64 = [&](std::shared_ptr<awst::Expression> v) {
		return awst::makeBtoi(awst::makeExtractLastN(
			awst::makeZeroExtendToN(awst::makeAsBytes(std::move(v), m_loc),
				8, m_loc), 8, m_loc), m_loc);
	};
	auto asBigIndex = [&](std::shared_ptr<awst::Expression> v) {
		return awst::makeAsBiguint(awst::makeItob(std::move(v), m_loc), m_loc);
	};
	// element i of the data region, typed for the recursive read/write
	auto childAddr = [&]() {
		Addr child = elemAddr(bv(dataN), asBigIndex(uv(iN)), elemType);
		child.solType = elemType;
		child.wtype = m_ctx.typeMapper.map(elemType);
		return child;
	};
	// `i += 1` closes the body, which then runs while i < bound
	auto emitLoop = [&](std::shared_ptr<awst::Block> body, std::string const& bound) {
		body->body.push_back(awst::makeAssignmentStatement(
			uv(iN), awst::makeUInt64BinOp(uv(iN),
				awst::UInt64BinaryOperator::Add, u64c(1), m_loc), m_loc));
		_d.out.push_back(awst::makeWhileLoop(
			awst::makeNumericCompare(uv(iN), awst::NumericComparison::Lt,
				uv(bound), m_loc), std::move(body), m_loc));
	};
	auto& out = _d.out;
	out.push_back(awst::makeAssignmentStatement(bv(slotN), _a.slot, m_loc));

	if (!_d.write)
	{
		std::string headsN = name("heads"), tailsN = name("tails"),
			offN = name("off"), innerN = name("inner"), resultN = name("result");
		auto u16 = [&](std::shared_ptr<awst::Expression> v) {
			return awst::makeExtract(awst::makeItob(std::move(v), m_loc), 6, 2, m_loc);
		};
		out.push_back(awst::makeAssignmentStatement(
			uv(nN), dynamic ? toU64(readSlotWord(bv(slotN), m_loc)) : u64c(sourceCount), m_loc));
		out.push_back(awst::makeAssignmentStatement(
			bv(dataN), dynamic ? dynDataBase(bv(slotN), m_loc) : bv(slotN), m_loc));
		out.push_back(awst::makeAssignmentStatement(uv(iN), u64c(0), m_loc));
		auto prefix = [&]() -> std::shared_ptr<awst::Expression> {
			if (dynamic) return u16(uv(nN));
			return awst::makeBytesConstant({}, m_loc);
		};
		if (elemDynamic)
		{
			out.push_back(awst::makeAssignmentStatement(
				xv(headsN), awst::makeBytesConstant({}, m_loc), m_loc));
			out.push_back(awst::makeAssignmentStatement(
				xv(tailsN), awst::makeBytesConstant({}, m_loc), m_loc));
			out.push_back(awst::makeAssignmentStatement(
				uv(offN), awst::makeUInt64BinOp(u64c(2),
					awst::UInt64BinaryOperator::Mult, uv(nN), m_loc), m_loc));
		}
		else if (bitPacked)
			out.push_back(awst::makeAssignmentStatement(xv(resultN), awst::makeConcat(prefix(),
				awst::makeBzero(awst::makeUInt64BinOp(awst::makeUInt64BinOp(uv(nN),
					awst::UInt64BinaryOperator::Add, u64c(7), m_loc),
					awst::UInt64BinaryOperator::FloorDiv, u64c(8), m_loc), m_loc), m_loc), m_loc));
		else
			out.push_back(awst::makeAssignmentStatement(
				xv(resultN), prefix(), m_loc));

		auto loop = awst::makeBlock(m_loc);
		Addr child = childAddr();
		auto lowered = m_ctx.lowerOperand([&]() {
			return readAny(child, elemType);
		}, true);
		for (auto& statement: lowered.effects.pre)
			loop->body.push_back(std::move(statement));
		if (!lowered.value)
			return false;
		if (bitPacked)
			loop->body.push_back(awst::makeAssignmentStatement(xv(resultN), awst::makeSetbit(
				xv(resultN), awst::makeUInt64BinOp(u64c(headerBytes * 8), awst::UInt64BinaryOperator::Add, uv(iN), m_loc),
				std::move(lowered.value), m_loc), m_loc));
		else
			loop->body.push_back(awst::makeAssignmentStatement(xv(innerN), awst::makeAsBytes(
				codec::valueToArc4(m_ctx.typeMapper, elemType, std::move(lowered.value), elemArc4, m_loc), m_loc), m_loc));
		for (auto& statement: lowered.effects.post)
			loop->body.push_back(std::move(statement));
		if (elemDynamic)
		{
			loop->body.push_back(awst::makeAssignmentStatement(
				xv(headsN), awst::makeConcat(
					xv(headsN), u16(uv(offN)), m_loc), m_loc));
			loop->body.push_back(awst::makeAssignmentStatement(
				xv(tailsN), awst::makeConcat(
					xv(tailsN), xv(innerN), m_loc), m_loc));
			loop->body.push_back(awst::makeAssignmentStatement(
				uv(offN), awst::makeUInt64BinOp(uv(offN),
					awst::UInt64BinaryOperator::Add,
					awst::makeLen(xv(innerN), m_loc), m_loc), m_loc));
		}
		else if (!bitPacked)
			loop->body.push_back(awst::makeAssignmentStatement(
				xv(resultN), awst::makeConcat(
					xv(resultN), xv(innerN), m_loc), m_loc));
		emitLoop(std::move(loop), nN);
		if (elemDynamic)
			out.push_back(awst::makeAssignmentStatement(
				xv(resultN), awst::makeConcat(prefix(),
					awst::makeConcat(xv(headsN), xv(tailsN), m_loc), m_loc),
				m_loc));
		_d.value = awst::makeReinterpretCast(xv(resultN), _arrW, m_loc);
		return true;
	}

	std::string valN = name("val"), oldN = name("old"), startN = name("start"),
		endN = name("end");
	auto headAbs = [&](std::shared_ptr<awst::Expression> idx) {
		return awst::makeUInt64BinOp(u64c(headerBytes),
			awst::UInt64BinaryOperator::Add,
			awst::makeBtoi(awst::makeExtract3(xv(valN),
				awst::makeUInt64BinOp(u64c(headerBytes),
					awst::UInt64BinaryOperator::Add,
					awst::makeUInt64BinOp(u64c(2),
						awst::UInt64BinaryOperator::Mult,
						std::move(idx), m_loc), m_loc),
				u64c(2), m_loc), m_loc), m_loc);
	};

	out.push_back(awst::makeAssignmentStatement(xv(valN), std::move(_d.value), m_loc));
	out.push_back(awst::makeAssignmentStatement(
		uv(oldN), dynamic ? toU64(readSlotWord(bv(slotN), m_loc))
			: u64c(static_cast<unsigned>(_at->length())), m_loc));
	out.push_back(awst::makeAssignmentStatement(
		uv(nN), dynamic ? awst::makeBtoi(awst::makeExtract(xv(valN), 0, 2, m_loc), m_loc) : u64c(sourceCount),
		m_loc));
	if (dynamic)
		out.push_back(SlotHandleAccess::writeSlot(bv(slotN), asBigIndex(uv(nN)), m_loc));
	out.push_back(awst::makeAssignmentStatement(
		bv(dataN), dynamic ? dynDataBase(bv(slotN), m_loc) : bv(slotN), m_loc));
	out.push_back(awst::makeAssignmentStatement(uv(iN), u64c(0), m_loc));

	auto loop = awst::makeBlock(m_loc);
	std::shared_ptr<awst::Expression> childBytes;
	std::shared_ptr<awst::Expression> childValue;
	if (bitPacked)
		childValue = awst::makeGetbit(xv(valN), awst::makeUInt64BinOp(
			u64c(headerBytes * 8), awst::UInt64BinaryOperator::Add, uv(iN), m_loc), m_loc);
	else if (elemDynamic)
	{
		loop->body.push_back(awst::makeAssignmentStatement(
			uv(startN), headAbs(uv(iN)), m_loc));
		auto hasNext = awst::makeNumericCompare(
			awst::makeUInt64BinOp(uv(iN), awst::UInt64BinaryOperator::Add,
				u64c(1), m_loc), awst::NumericComparison::Lt, uv(nN), m_loc);
		loop->body.push_back(awst::makeAssignmentStatement(
			uv(endN), awst::makeConditional(std::move(hasNext),
				headAbs(awst::makeUInt64BinOp(uv(iN),
					awst::UInt64BinaryOperator::Add, u64c(1), m_loc)),
				awst::makeLen(xv(valN), m_loc),
				awst::WType::uint64Type(), m_loc), m_loc));
		childBytes = awst::makeExtract3(xv(valN), uv(startN),
			awst::makeUInt64BinOp(uv(endN), awst::UInt64BinaryOperator::Sub,
				uv(startN), m_loc), m_loc);
	}
	else
	{
		auto start = awst::makeUInt64BinOp(u64c(headerBytes),
			awst::UInt64BinaryOperator::Add,
			awst::makeUInt64BinOp(uv(iN),
				awst::UInt64BinaryOperator::Mult,
				u64c(static_cast<uint64_t>(elemSize)), m_loc), m_loc);
		childBytes = awst::makeExtract3(xv(valN), std::move(start),
			u64c(static_cast<uint64_t>(elemSize)), m_loc);
	}
	Addr child = childAddr();
	if (!childValue) childValue = awst::makeReinterpretCast(std::move(childBytes), elemArc4, m_loc);
	if (!writeAny(child, elemType, std::move(childValue), loop->body))
		return false;
	emitLoop(std::move(loop), nN);

	// Recursively clear elements made unreachable by a shrink. Mapping
	// content is intentionally excluded above, matching Solidity delete.
	auto clearLoop = awst::makeBlock(m_loc);
	Addr oldChild = childAddr();
	if (!clearAggregate(oldChild, elemType, clearLoop->body))
		return false;
	emitLoop(std::move(clearLoop), oldN);
	return true;
}

bool EvmSlotLowering::lowerFixedArray(
	Addr const& address, ArrayType const* type, ValueDir& direction)
{
	auto length = type->length();
	if (length == 0 || length > 64)
	{
		Logger::instance().error("--evm-storage-layout: fixed-array value traversal of length "
			+ length.str() + " exceeds the supported extent of 64", m_loc);
		return false;
	}
	if (length > 4)
		return lowerDynArrayGeneric(address, type, m_ctx.typeMapper.map(type), direction);
	auto const* elementType = type->baseType();
	auto const* arrayType = direction.write ? direction.value->wtype : m_ctx.typeMapper.map(type);
	auto const* elementWType = awst::arrayElementType(arrayType);
	if (!elementWType) return false;
	auto name = "__evm_fixed_" + std::to_string(awst::NameGen::next("EvmSlotLowering.fixed"));
	auto base = awst::makeVarExpression(name + "_slot", awst::WType::biguintType(), m_loc);
	direction.out.push_back(awst::makeAssignmentStatement(base, address.slot, m_loc));
	auto input = awst::makeVarExpression(name + "_value", arrayType, m_loc);
	auto output = awst::makeNewArray(arrayType, m_loc);
	unsigned sourceLength = static_cast<unsigned>(length);
	if (direction.write)
	{
		direction.out.push_back(awst::makeAssignmentStatement(input, std::move(direction.value), m_loc));
		if (auto const* fixed = dynamic_cast<awst::ARC4StaticArray const*>(arrayType))
			sourceLength = static_cast<unsigned>(fixed->arraySize());
	}
	auto const* nested = dynamic_cast<ArrayType const*>(elementType);
	bool const dynamicArray = nested && nested->isDynamicallySized() && !isBytesLike(elementType);
	auto raw = awst::makeAsBytes(input, m_loc);
	auto headAt = [&](unsigned index) {
		return awst::makeBtoi(awst::makeExtract(raw, static_cast<int>(2 * index), 2, m_loc), m_loc);
	};
	for (unsigned i = 0; i < static_cast<unsigned>(length); ++i)
	{
		auto child = elemAddr(base, awst::makeIntegerConstant(i, m_loc, awst::WType::biguintType()), elementType);
		if (!direction.write)
		{
			auto value = readAny(child, elementType);
			if (!value) return false;
			output->values.push_back(codec::valueToArc4(
				m_ctx.typeMapper, elementType, std::move(value), elementWType, m_loc));
			continue;
		}
		// Solc copies only the source extent, then clears the destination tail.
		if (i >= sourceLength)
		{
			if (!clearAggregate(child, elementType, direction.out)) return false;
			continue;
		}
		std::shared_ptr<awst::Expression> value;
		if (dynamicArray)
		{
			// ARC4 indexing does not project dynamic sub-arrays of a fixed array.
			// Head offsets refer to the SOURCE extent, not the destination length.
			auto start = headAt(i);
			auto end = i + 1 < sourceLength ? headAt(i + 1)
				: std::shared_ptr<awst::Expression>(awst::makeLen(raw, m_loc));
			value = awst::makeReinterpretCast(awst::makeExtract3(raw, start,
				awst::makeUInt64BinOp(std::move(end), awst::UInt64BinaryOperator::Sub, start, m_loc), m_loc),
				elementWType, m_loc);
		}
		else value = awst::makeIndexExpression(input, awst::makeIntegerConstant(i, m_loc), elementWType, m_loc);
		if (!writeAny(child, elementType, std::move(value), direction.out)) return false;
	}
	if (!direction.write) direction.value = std::move(output);
	return true;
}

std::shared_ptr<awst::Expression> EvmSlotLowering::readValue(
	Addr const& _a, std::shared_ptr<awst::Expression> _word)
{
	// A packed address reads both the main slot and its auxiliary high-byte
	// slot inside one expression tree.  Share a SingleEvaluation node so a
	// computed slot expression is evaluated exactly once.
	auto readSlot = (_a.wtype == awst::WType::accountType() && _a.size == 20)
		? std::shared_ptr<awst::Expression>(awst::makeEvalOnce(_a.slot, m_loc))
		: _a.slot;
	auto word = _word ? std::move(_word) : readSlotWord(readSlot, m_loc);
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
			packedAddrAuxSlot(readSlot, _a.byteOffset, m_loc), m_loc);
		auto hi = awst::makeExtract(
			awst::makeLeftPadToN(awst::makeAsBytes(std::move(aux), m_loc), 32, m_loc),
			20, 12, m_loc);
		return awst::makeAsAccount(
			awst::makeConcat(std::move(hi), std::move(raw), m_loc), m_loc);
	}
	return SlotWordCodec::packedBytesToNative(
		std::move(raw), _a.wtype, _a.solType, _a.size, m_loc);
}

bool EvmSlotLowering::writeAny(
	Addr& _a,
	Type const* _t,
	std::shared_ptr<awst::Expression> _value,
	std::vector<std::shared_ptr<awst::Statement>>& _out)
{
	if (!_t || !_value)
		return false;
	_a.solType = _t;
	if (!_a.wtype)
		_a.wtype = m_ctx.typeMapper.map(_t);
	if (_t->isValueType())
	{
		auto nat = coerceToNative(std::move(_value), _a);
		if (!nat)
			return false;
		writeValue(_a, std::move(nat), _out);
		return true;
	}
	if (isBytesLike(_t))
	{
		if (_value->wtype && _value->wtype->kind() != awst::WTypeKind::Bytes
			&& _value->wtype != awst::WType::stringType())
			_value = awst::makeARC4Decode(
				std::move(_value), bytesLikeDecodeTarget(_t), m_loc);
		writeBytesValue(_a, std::move(_value), _out);
		return true;
	}
	if (auto const* at = dynamic_cast<ArrayType const*>(_t))
		return writeArrayValue(_a, at, std::move(_value), _out);
	if (dynamic_cast<StructType const*>(_t))
		return writeStructValue(_a, std::move(_value), _out);
	Logger::instance().error(
		"--evm-storage-layout: declared type cannot be written as a value", m_loc);
	return false;
}

std::shared_ptr<awst::Expression> EvmSlotLowering::readAny(
	Addr const& _a, Type const* _t)
{
	if (!_t)
		return nullptr;
	Addr a = _a;
	a.solType = _t;
	if (!a.wtype)
		a.wtype = m_ctx.typeMapper.map(_t);
	if (_t->isValueType())
		return readValue(a);
	if (isBytesLike(_t))
		return readBytesValue(a);
	if (auto const* at = dynamic_cast<ArrayType const*>(_t))
		return readArrayValue(a, at);
	if (dynamic_cast<StructType const*>(_t))
		return readStructValue(a);
	Logger::instance().error(
		"--evm-storage-layout: declared type cannot be materialised as a value",
		m_loc);
	return nullptr;
}

bool EvmSlotLowering::clearAggregate(
	Addr const& _a,
	Type const* _t,
	std::vector<std::shared_ptr<awst::Statement>>& _out)
{
	bool ok = clearAggregateImpl(_a, _t, _out);
	if (m_clearTypeStack.empty())
		synthesizePendingClearSubs();
	return ok;
}

void EvmSlotLowering::synthesizePendingClearSubs()
{
	auto& arts = m_ctx.typeMapper.artifacts();
	while (!arts.pendingEvmClearSubs.empty())
	{
		auto [target, t] = arts.pendingEvmClearSubs.back();
		arts.pendingEvmClearSubs.pop_back();
		Addr a;
		a.slot = awst::makeVarExpression(
			"__slot", awst::WType::biguintType(), m_loc);
		a.solType = t;
		a.wtype = m_ctx.typeMapper.map(t);
		std::vector<std::shared_ptr<awst::Statement>> body;
		// Empty stack: the first entry emits the real body; the nested
		// self-reference hits the guard and becomes the recursive call.
		if (!clearAggregateImpl(a, t, body))
			continue;
		body.push_back(awst::makeReturnStatement(nullptr, m_loc));
		std::vector<awst::SubroutineArgument> args;
		args.emplace_back("__slot", awst::WType::biguintType(), m_loc);
		auto blk = awst::makeBlock(m_loc);
		blk->body = std::move(body);
		arts.pendingYulSubroutines.push_back(awst::makeSubroutine(
			target, target, std::move(args), awst::WType::voidType(),
			std::move(blk), /*pure=*/false, m_loc));
	}
}

bool EvmSlotLowering::clearAggregateImpl(
	Addr const& _a,
	Type const* _t,
	std::vector<std::shared_ptr<awst::Statement>>& _out)
{
	if (!_t)
		return false;
	if (_t->isValueType())
	{
		auto address = _a;
		return writeAny(address, _t,
			TypeCoercion::makeDefaultValue(m_ctx.typeMapper.map(_t), m_loc), _out);
	}
	if (dynamic_cast<MappingType const*>(_t)) return true;
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
				// AGGREGATE elements. EVM's delete clears each element's own
				// region recursively — dynamic_multi_array_cleanup re-grows
				// the array afterwards and requires zeros, so leaving stale
				// data behind a zeroed length word is observably wrong.
				auto const* et = at->baseType();
				// A mapping element owns NOTHING at its own slot (content is
				// at keccak(key ++ elemSlot), which EVM cannot clear either),
				// so length-only IS the matching semantics.
				if (!dynamic_cast<solidity::frontend::MappingType const*>(et))
				{
					auto nm = [&](char const* tag) {
						return std::string("__evmclr_") + tag + "_"
							+ std::to_string(awst::NameGen::next("EvmSlotLowering.clrLoop"));
					};
					std::string ivar = nm("i"), lvar = nm("n"), dvar = nm("d");
					auto bv = [&](std::string const& n) {
						return awst::makeVarExpression(n, awst::WType::biguintType(), m_loc);
					};
					// length and data base must be read BEFORE anything is
					// zeroed — the loop needs both.
					_out.push_back(awst::makeAssignmentStatement(
						bv(lvar), readSlotWord(_a.slot, m_loc), m_loc));
					_out.push_back(awst::makeAssignmentStatement(
						bv(dvar), dynDataBase(_a.slot, m_loc), m_loc));
					_out.push_back(awst::makeAssignmentStatement(
						bv(ivar), awst::makeZero(m_loc, awst::WType::biguintType()), m_loc));
					auto ea = elemAddr(bv(dvar), bv(ivar), et);
					std::vector<std::shared_ptr<awst::Statement>> body;
					if (!clearAggregateImpl(ea, et, body))
						return false;
					body.push_back(awst::makeAssignmentStatement(bv(ivar),
						awst::makeBigUIntBinOp(bv(ivar),
							awst::BigUIntBinaryOperator::Add,
							awst::makeIntegerConstant("1", m_loc,
								awst::WType::biguintType()), m_loc), m_loc));
					auto blk = awst::makeBlock(m_loc);
					blk->body = std::move(body);
					_out.push_back(awst::makeWhileLoop(
						awst::makeNumericCompare(bv(ivar),
							awst::NumericComparison::Lt, bv(lvar), m_loc),
						std::move(blk), m_loc));
				}
				_out.push_back(SlotHandleAccess::writeSlot(_a.slot,
					awst::makeZero(m_loc, awst::WType::biguintType()), m_loc));
				return true;
			}
			auto call = awst::makeSubroutineCall(
				awst::SubroutineID{"__puyasol___evm_dynarr_write"},
				awst::WType::voidType(), m_loc);
			awst::pushCallArg(call->args, "__slot", _a.slot);
			awst::pushCallArg(call->args, "__val",
				awst::makeBytesConstant({0, 0}, m_loc));
			pushDynElemMetricArgs(call->args, metc, m_loc);
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
			return SlotHandleAccess::forEachIndex(span, _out, m_loc, [&](auto index, auto& body) {
				body.push_back(SlotHandleAccess::writeSlot(awst::makeBigUIntBinOp(baseVar(),
					awst::BigUIntBinaryOperator::Add, std::move(index), m_loc),
					awst::makeZero(m_loc, awst::WType::biguintType()), m_loc));
				return true;
			});
		}
		return SlotHandleAccess::forEachIndex(lenU, _out, m_loc, [&](auto index, auto& body) {
			return clearAggregateImpl(elemAddr(baseVar(), std::move(index), elemType), elemType, body);
		});
	}
	if (auto const* st = dynamic_cast<StructType const*>(_t))
	{
		// A struct on the active emission path again = recursive type
		// (S{S[] x}); inlining would never terminate. Route through a
		// per-type runtime subroutine — the data is finite, so runtime
		// recursion bottoms out where EVM's clear functions do.
		std::string typeId = st->richIdentifier();
		if (std::find(m_clearTypeStack.begin(), m_clearTypeStack.end(), typeId)
			!= m_clearTypeStack.end())
		{
			auto& arts = m_ctx.typeMapper.artifacts();
			auto [it, fresh] = arts.evmClearSubs.try_emplace(typeId,
				"__puyasol___evm_clear_"
					+ std::to_string(arts.evmClearSubs.size()));
			if (fresh)
				arts.pendingEvmClearSubs.emplace_back(it->second, _t);
			auto call = awst::makeSubroutineCall(
				awst::SubroutineID{it->second}, awst::WType::voidType(), m_loc);
			awst::pushCallArg(call->args, "__slot", _a.slot);
			_out.push_back(awst::makeExpressionStatement(std::move(call), m_loc));
			return true;
		}
		m_clearTypeStack.push_back(typeId);
		struct StackPop
		{
			std::vector<std::string>& s;
			~StackPop() { s.pop_back(); }
		} popper{m_clearTypeStack};
		// Distinct prefix from the fixed-array branch: NameGen counters are
		// PER-KEY, so sharing "__evmcl_" made both emit __evmcl_0 in one
		// function — the member clear clobbered the struct's pinned base and
		// the span zeroing shifted one slot up (slot 0 kept its value).
		std::string bs = "__evmcls_"
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
		// ORDER MATTERS: recurse into dynamic members FIRST. Their regions are
		// found through their length words, which the span zeroing below
		// destroys — clearing the span first would strand the data and a later
		// re-grow would read it back.
		// dynamic members: clear their keccak-region data too
		for (auto const& m: st->members(nullptr))
		{
			auto const* mt = m.type;
			if (dynamic_cast<MappingType const*>(mt)) continue;
			auto fa = memberAddr(baseVar(), st, m.name, mt);
			// The span clear covers ordinary value fields. Packed accounts own
			// an auxiliary word too, so use their typed leaf clear first.
			if (mt->isValueType() && !(fa.wtype == awst::WType::accountType() && fa.size == 20)) continue;
			if (!clearAggregateImpl(fa, mt, _out))
				return false;
		}
		return SlotHandleAccess::forEachIndex(span, _out, m_loc, [&](auto index, auto& body) {
			body.push_back(SlotHandleAccess::writeSlot(awst::makeBigUIntBinOp(baseVar(),
				awst::BigUIntBinaryOperator::Add, std::move(index), m_loc),
				awst::makeZero(m_loc, awst::WType::biguintType()), m_loc));
			return true;
		});
	}
	Logger::instance().error(
		"--evm-storage-layout: delete on this aggregate shape not yet "
		"supported", m_loc);
	return false;
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
	// PACKED address: stash the high 12 bytes in the shadow aux slot (the
	// word window keeps the EVM-shaped trailing 20 for asm fidelity). Both
	// the value and slot feed TWO statements, so pin each to a named temp;
	// SingleEvaluation does not persist across statement boundaries.
	std::shared_ptr<awst::Expression> slotOnce;
	if (_a.wtype == awst::WType::accountType() && _a.size == 20)
	{
		std::string slotName = "__evm_addr_slot_"
			+ std::to_string(awst::NameGen::next("EvmSlotLowering.addrSlotPin"));
		_out.push_back(awst::makeAssignmentStatement(
			awst::makeVarExpression(
				slotName, awst::WType::biguintType(), m_loc),
			_a.slot, m_loc));
		slotOnce = awst::makeVarExpression(
			slotName, awst::WType::biguintType(), m_loc);
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
	else
	{
		// For every other sub-word write both uses remain inside the final
		// write statement, where EvalOnce is the appropriate sharing scope.
		slotOnce = awst::makeEvalOnce(_a.slot, m_loc);
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
