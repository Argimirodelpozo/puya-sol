/// @file SolIdentifier.cpp — variable/constant/state variable resolution.

#include "builder/sol-ast/exprs/SolIdentifier.h"
#include "Logger.h"
#include "builder/sol-ast/EvmSlotLowering.h"
#include "builder/storage/EvmLayoutMode.h"
#include "builder/itxn/FunctionPointerBuilder.h"
#include "builder/storage/StorageBackend.h"
#include "builder/storage/StorageMapper.h"
#include "builder/storage/TransientStorage.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/assembly/AssemblyBuilder.h"
#include "builder/sol-types/TypeCoercion.h"
#include "builder/sol-types/Arc4Defaults.h"

#include <libsolidity/ast/AST.h>

namespace puyasol::builder::sol_ast
{

using namespace solidity::frontend;

SolIdentifier::SolIdentifier(
	eb::ContractContext& _ctx,
	Identifier const& _node)
	: SolExpression(_ctx, _node), m_ident(_node)
{
}

std::shared_ptr<awst::Expression> SolIdentifier::toAwst()
{
	std::string name = m_ident.name();

	// Bare `this` is CONTRACT-typed (solc), and contract-typed values carry the
	// FAKE app-id address form `bzero(24) ++ itob(appId)` everywhere else in
	// this codebase (see TypeCoercion's application→account cast) — external
	// calls recover the app id from the last 8 bytes. `this` used to lower to
	// the REAL app address, so a contract-typed `this` flowing into an opaque
	// context (e.g. `I i = this; i.f()` through a library param) made the
	// inner-txn target `btoi(hash garbage)`. The value-consumers that need the
	// REAL address (`address(this)`, `.balance`, external self-calls) all
	// special-case the `this` identifier upstream and never see this lowering.
	if (name == "this")
	{
		auto appId = awst::makeGlobal(
			std::string("CurrentApplicationID"), awst::WType::uint64Type(), m_loc);
		auto idBytes = awst::makeItob(std::move(appId), m_loc);
		auto fake = awst::makeLeftPad(std::move(idBytes), 24, m_loc);
		return awst::makeReinterpretCast(std::move(fake), awst::WType::accountType(), m_loc);
	}

	auto const* decl = m_ident.annotation().referencedDeclaration;
	if (decl)
	{
		// Parameter remaps (modifier parameters)
		if (auto const* remap = m_scope.findParamRemap(decl->id()))
			return awst::makeVarExpression(remap->name, remap->type, m_loc);

		// Storage pointer aliases
		if (auto const* alias = m_scope.findStorageAlias(decl->id()))
			return alias->expr;

		// Memory-aggregate alias (`T memory b = a` → b resolves to a's local).
		if (auto memAlias = m_scope.findMemoryAlias(decl->id()))
			return memAlias;
	}

	// Variable references
	if (auto const* varDecl = dynamic_cast<VariableDeclaration const*>(decl))
	{
		// Blob-backed aggregate: variable travels as a uint64 base offset into the
		// memory blob. A bare reference of an ARRAY/STRUCT resolves to that offset
		// (field/index access go through SolIndexAccess). But a bytes/string blob
		// buffer (the OZ Strings.toString idiom) is small + materialisable: a bare
		// value-use reads [len word][data] out of the blob and returns the value.
		// (In assembly the AssemblyBuilder resolves it to the offset via
		// m_blobOffsetVars — this path is the OUTSIDE-asm value-use.)
		if (auto off = m_scope.findBlobAggregate(varDecl->id()); !off.empty())
		{
			using AB = builder::AssemblyBuilder;
			auto const* vt = m_ctx.typeMapper.map(varDecl->type());
			// DYNAMIC ARRAY value-use: materialise, don't leak the offset.
			//
			// A blob-backed array used as a VALUE (returned, passed, assigned)
			// must come back out of the blob, exactly like the bytes/string case
			// below. Leaking the raw uint64 offset instead produced a subroutine
			// that returns uint64 while its declared return type is the array —
			// puya rejects the program ("invalid return type
			// [PrimitiveIRType.uint64], expected EncodedType(...)"). Hit by OZ
			// EnumerableSet.values(), whose `assembly { result := store }`
			// pointer-pun blob-backs `result` and then returns it (blocked gho).
			//
			// EVM memory layout is [32-byte COUNT][elements]; ARC4 wants a
			// 2-byte count prefix, so the re-encode is just swapping the header —
			// valid only while an element occupies 32 bytes in BOTH (address,
			// bytes32, uint256). Anything else (bool bit-packing, uint8[] at one
			// byte per element, dynamic elements needing an offset table) would
			// need a per-element re-encode; refuse loudly rather than emit a
			// wrong length or a mis-strided read.
			if (vt && vt->kind() == awst::WTypeKind::ARC4DynamicArray)
			{
				auto const* arr = static_cast<awst::ARC4DynamicArray const*>(vt);
				int esz = builder::computeEncodedElementSize(arr->elementType());
				if (esz != 32)
				{
					Logger::instance().error(
						"cannot materialise assembly-backed array '" + name
						+ "': element type '" + arr->elementType()->name()
						+ "' encodes to " + std::to_string(esz)
						+ " bytes, not 32, so the EVM memory layout and the ARC4"
						" layout disagree on stride",
						m_loc);
					return awst::makeVarExpression(off, awst::WType::uint64Type(), m_loc);
				}
				auto offRead = [&]() {
					return awst::makeVarExpression(off, awst::WType::uint64Type(), m_loc);
				};
				// count = low 8 bytes of the 32-byte count word at the buffer offset
				auto count = awst::makeEvalOnce(
					awst::makeExtractUInt64(
						AB::readMemWordDirect(offRead(), m_loc),
						awst::makeIntegerConstant("24", m_loc), m_loc),
					m_loc);
				auto dataStart = awst::makeUInt64BinOp(offRead(),
					awst::UInt64BinaryOperator::Add,
					awst::makeIntegerConstant("32", m_loc), m_loc);
				auto byteLen = awst::makeUInt64BinOp(count,
					awst::UInt64BinaryOperator::Mult,
					awst::makeIntegerConstant("32", m_loc), m_loc);
				auto data = awst::makeExtract3(
					awst::makeLoadSlot(AB::MEMORY_SLOT_FIRST, m_loc),
					std::move(dataStart), std::move(byteLen), m_loc);
				// ARC4 dynamic array = uint16 count ++ elements
				auto hdr = awst::makeExtract3(awst::makeItob(count, m_loc),
					awst::makeIntegerConstant("6", m_loc),
					awst::makeIntegerConstant("2", m_loc), m_loc);
				auto val = awst::makeConcat(std::move(hdr), std::move(data), m_loc);
				return awst::makeReinterpretCast(std::move(val), vt, m_loc);
			}
			if (vt != awst::WType::bytesType() && vt != awst::WType::stringType())
				return awst::makeVarExpression(off, awst::WType::uint64Type(), m_loc);
			auto offRead = [&]() {
				return awst::makeVarExpression(off, awst::WType::uint64Type(), m_loc);
			};
			// length = low 8 bytes of the 32-byte length word at the buffer offset.
			auto length = awst::makeExtractUInt64(
				AB::readMemWordDirect(offRead(), m_loc),
				awst::makeIntegerConstant("24", m_loc), m_loc);
			// data = extract3(blob, offset + 32, length).
			auto dataStart = awst::makeUInt64BinOp(offRead(),
				awst::UInt64BinaryOperator::Add, awst::makeIntegerConstant("32", m_loc), m_loc);
			auto data = awst::makeExtract3(
				awst::makeLoadSlot(AB::MEMORY_SLOT_FIRST, m_loc),
				std::move(dataStart), std::move(length), m_loc);
			if (vt == awst::WType::stringType())
				return awst::makeReinterpretCast(std::move(data), awst::WType::stringType(), m_loc);
			return std::shared_ptr<awst::Expression>(std::move(data));
		}

		// Struct storage-ref param (e.g. V4 `Pool.State storage self`): travels
		// as box-key PREFIX in bytes. When used as struct value (`self.field`),
		// resolve to a box-backed struct keyed by the runtime prefix so ARC4Struct
		// field machinery handles member access. Other uses bypass this:
		// function-pass → SolInternalCall::extractMappingKeyPrefix;
		// `self.nestedMap[k]` → SolIndexAccess::buildInitialPrefix.
		if (!m_scope.findMappingKeyParam(varDecl->id()).empty()
			&& varDecl->type()
			&& varDecl->type()->category() == solidity::frontend::Type::Category::Struct)
		{
			auto* structType = m_ctx.typeMapper.map(varDecl->type());
			auto key = awst::makeVarExpression(name, awst::WType::bytesType(), m_loc);
			auto boxKey = awst::makeReinterpretCast(
				std::move(key), awst::WType::boxKeyType(), m_loc);
			auto boxExpr = awst::makeBoxValueExpression(std::move(boxKey), structType, m_loc);
			return builder::StorageMapper::makeStateGetWithDefault(
				std::move(boxExpr), structType, m_loc);
		}

		// Constants: inline the value. Immutables: DO NOT inline — the constructor
		// may mutate them (e.g. `int immutable x = 1; constructor() { x--; }`).
		if (varDecl->isConstant() && varDecl->value())
		{
			auto val = buildExpr(*varDecl->value());

			// bytes[N] constant from integer literal → BytesConstant
			auto* targetType = m_ctx.typeMapper.map(varDecl->type());
			if (auto const* bytesType = dynamic_cast<awst::BytesWType const*>(targetType))
			{
				if (auto* intConst = dynamic_cast<awst::IntegerConstant*>(val.get()))
				{
					int len = bytesType->length().value_or(0);
					return awst::makeBytesConstant(
						builder::TypeCoercion::intLiteralToBytesN(intConst->value, len),
						val->sourceLocation, awst::BytesEncoding::Base16, targetType);
				}
			}
			// String → bytes[N] right-pad
			if (auto const* bwt = dynamic_cast<awst::BytesWType const*>(targetType))
			{
				if (bwt->length().has_value() && *bwt->length() > 0)
				{
					if (auto padded = builder::TypeCoercion::stringToBytesN(
							val.get(), targetType, *bwt->length(), val->sourceLocation))
						return padded;
				}
			}
			// String → bytes cast
			if (targetType == awst::WType::bytesType()
				&& val->wtype == awst::WType::stringType())
			{
				auto cast = awst::makeAsBytes(std::move(val), val->sourceLocation);
				return cast;
			}
			return val;
		}

		if (varDecl->isStateVariable())
		{
			auto* type = m_ctx.typeMapper.map(varDecl->type());

			// --evm-storage-layout: persistent state vars read from their EVM
			// slot address. Aggregates never build here — index/member/write
			// accesses intercept on the AST above this identifier.
			if (builder::evmStorageLayout()
				&& !varDecl->isConstant() && !varDecl->immutable()
				&& varDecl->referenceLocation() != VariableDeclaration::Location::Transient)
			{
				if (varDecl->type() && varDecl->type()->isValueType())
				{
					EvmSlotLowering low(m_ctx, m_scope, m_loc);
					if (auto addr = low.resolve(m_ident))
						return low.readValue(*addr);
					return nullptr;
				}
				if (EvmSlotLowering::isBytesLike(varDecl->type()))
				{
					EvmSlotLowering low(m_ctx, m_scope, m_loc);
					if (auto addr = low.resolve(m_ident))
						return low.readBytesValue(*addr);
					return nullptr;
				}
				Logger::instance().error(
					"--evm-storage-layout: aggregate state variable '" + name
					+ "' used as a value is not yet supported", m_loc);
				return nullptr;
			}

			// Mapping used as a VALUE (`r = a;` where r is a storage-pointer alias):
			// return its name as bytes so the alias holds the box-key prefix.
			// Mapping has no own value; SolIndexAccess still builds box access from the name.
			if (varDecl->type()
				&& varDecl->type()->category() == solidity::frontend::Type::Category::Mapping)
			{
				return awst::makeUtf8BytesConstant(name, m_loc, awst::WType::bytesType());
			}

			// Transient state vars: packed blob in AssemblyBuilder::TRANSIENT_SLOT
			// (same slot asm tload/tstore uses); StorageBackend dispatches to TransientStorage.
			if (varDecl->referenceLocation() == VariableDeclaration::Location::Transient
				&& m_ctx.storageBackend && m_ctx.storageBackend->isTransient(*varDecl))
			{
				if (auto read = m_ctx.storageBackend->emitReadForVar(*varDecl, name, type, m_loc))
					return read;
			}

			auto kind = builder::StorageMapper::shouldUseBoxStorage(*varDecl)
				? awst::AppStorageKind::Box
				: awst::AppStorageKind::AppGlobal;

			// Dynamic arrays in box storage: placeholder
			if (type && type->kind() == awst::WTypeKind::ReferenceArray
				&& kind == awst::AppStorageKind::Box)
			{
				auto placeholder = awst::makeVarExpression(name, type, m_loc);
				return placeholder;
			}

			// Constants (redundant guard); immutables always read from state.
			if (varDecl->isConstant() && varDecl->value())
				return buildExpr(*varDecl->value());

			return m_ctx.storageMapper.createStateRead(name, type, kind, m_loc);
		}
	}

	// Function pointer reference (identifier used as a value, not a call target):
	// type annotation is FunctionType{Internal/External}.
	if (auto const* funcDef = dynamic_cast<solidity::frontend::FunctionDefinition const*>(decl))
	{
		if (auto const* ft = dynamic_cast<solidity::frontend::FunctionType const*>(m_solType))
			if (ft->kind() == solidity::frontend::FunctionType::Kind::Internal
				|| ft->kind() == solidity::frontend::FunctionType::Kind::External)
				return eb::FunctionPointerBuilder::buildFunctionReference(m_ctx, funcDef, m_loc, ft);
		// Otherwise: function used as call target, fall through.
	}

	// Dynamic CALLDATA param with LIVE pointer locals (an assembly block seeded or
	// repointed __cd_off_<name>/__cd_len_<name>): the param's VALUE is the byte
	// range the pointer designates inside __cd_blob — `assembly { x.offset := 1 }
	// return x;` must honour the repoint (EVM calldata-pointer semantics), not
	// return the originally-decoded param.
	if (auto const* vd = dynamic_cast<VariableDeclaration const*>(decl))
		if (vd->referenceLocation() == solidity::frontend::VariableDeclaration::Location::CallData)
			if (auto* live = m_ctx.currentScope ? m_ctx.currentScope->liveCalldataPointers() : nullptr)
				if (live->count(name))
					return builder::TypeCoercion::calldataPointerValueRead(name, m_loc);

	// Regular local variable
	auto e = std::make_shared<awst::VarExpression>();
	e->sourceLocation = m_loc;
	if (decl)
	{
		if (auto const* vd = dynamic_cast<VariableDeclaration const*>(decl))
		{
			e->name = m_scope.awstVarName(*vd); // renamed for shadowing
			e->wtype = m_ctx.typeMapper.map(vd->type());
		}
		else
			e->name = name;
	}
	else
		e->name = name;
	if (!e->wtype || e->wtype == awst::WType::voidType())
	{
		if (m_solType)
			e->wtype = m_ctx.typeMapper.map(m_solType);
	}
	return e;
}

} // namespace puyasol::builder::sol_ast
