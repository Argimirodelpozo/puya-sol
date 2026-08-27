/// @file SolIdentifier.cpp — variable/constant/state variable resolution.

#include "builder/sol-ast/exprs/SolIdentifier.h"
#include "Logger.h"
#include "builder/sol-ast/EvmSlotLowering.h"
#include "builder/sol-ast/StorageRefPointer.h"
#include "builder/contract/ContractBuilder.h"
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

namespace
{

// Bare `this` is CONTRACT-typed (solc), and contract-typed values carry the
// FAKE app-id address form `bzero(24) ++ itob(appId)` everywhere else in
// this codebase (see TypeCoercion's application→account cast) — external
// calls recover the app id from the last 8 bytes. `this` used to lower to
// the REAL app address, so a contract-typed `this` flowing into an opaque
// context (e.g. `I i = this; i.f()` through a library param) made the
// inner-txn target `btoi(hash garbage)`. The value-consumers that need the
// REAL address (`address(this)`, `.balance`, external self-calls) all
// special-case the `this` identifier upstream and never see this lowering.
std::shared_ptr<awst::Expression> buildThisValue(awst::SourceLocation const& loc)
{
	auto appId = awst::makeGlobal(
		std::string("CurrentApplicationID"), awst::WType::uint64Type(), loc);
	auto idBytes = awst::makeItob(std::move(appId), loc);
	auto fake = awst::makeLeftPad(std::move(idBytes), 24, loc);
	return awst::makeReinterpretCast(std::move(fake), awst::WType::accountType(), loc);
}

// --evm-storage-layout: a storage-located local/param IS its biguint
// slot handle (bound at declaration / by the call convention).
bool isSlotStorageLocal(eb::ContractContext& ctx, VariableDeclaration const& varDecl)
{
	return ctx.typeMapper.profile().evmStorageLayout
		&& !varDecl.isStateVariable()
		&& (varDecl.isLocalVariable() || varDecl.isCallableOrCatchParameter())
		&& varDecl.referenceLocation() == VariableDeclaration::Location::Storage;
}

// Blob-backed aggregate: variable travels as a uint64 base offset into the
// memory blob. A bare reference of an ARRAY/STRUCT resolves to that offset
// (field/index access go through SolIndexAccess). But a bytes/string blob
// buffer (the OZ Strings.toString idiom) is small + materialisable: a bare
// value-use reads [len word][data] out of the blob and returns the value.
// (In assembly the AssemblyBuilder resolves it to the offset via
// m_blobOffsetVars — this path is the OUTSIDE-asm value-use.)
std::shared_ptr<awst::Expression> tryBlobAggregateValue(
	eb::ContractContext& ctx, Context& scope,
	VariableDeclaration const& varDecl, awst::SourceLocation const& loc)
{
	auto off = scope.findBlobAggregate(varDecl.id());
	if (off.empty())
		return nullptr;
	auto const* vt = ctx.typeMapper.map(varDecl.type());
	if (auto value = builder::materializeBlobValue(
			ctx.typeMapper, varDecl.type(), vt, off, loc,
			ctx.preEffects()))
		return value;
	return awst::makeVarExpression(
		off, awst::WType::uint64Type(), loc);
}

// Struct storage-ref param (e.g. V4 `Pool.State storage self`): travels
// as box-key PREFIX in bytes. When used as struct value (`self.field`),
// resolve to a box-backed struct keyed by the runtime prefix so ARC4Struct
// field machinery handles member access. Other uses bypass this:
// function-pass → SolInternalCall::extractMappingKeyPrefix;
// `self.nestedMap[k]` → SolIndexAccess::buildInitialPrefix.
std::shared_ptr<awst::Expression> tryStructRefParamValue(
	eb::ContractContext& ctx, Context& scope,
	VariableDeclaration const& varDecl, std::string const& name,
	awst::SourceLocation const& loc)
{
	if (scope.findMappingKeyParam(varDecl.id()).empty()
		|| !varDecl.type()
		|| varDecl.type()->category() != solidity::frontend::Type::Category::Struct)
		return nullptr;
	auto* structType = ctx.typeMapper.map(varDecl.type());
	auto key = awst::makeVarExpression(name, awst::WType::bytesType(), loc);
	auto boxKey = awst::makeReinterpretCast(
		std::move(key), awst::WType::boxKeyType(), loc);
	auto boxExpr = awst::makeBoxValueExpression(std::move(boxKey), structType, loc);
	return builder::StorageMapper::makeStateGetWithDefault(
		std::move(boxExpr), structType, loc);
}

// Constants: inline the value. Immutables: DO NOT inline — the constructor
// may mutate them (e.g. `int immutable x = 1; constructor() { x--; }`).
// Caller guards isConstant() && value().
std::shared_ptr<awst::Expression> buildConstantValue(
	eb::ContractContext& ctx, VariableDeclaration const& varDecl)
{
	auto val = ctx.buildExpr(*varDecl.value());

	// bytes[N] constant from integer literal → BytesConstant
	auto* targetType = ctx.typeMapper.map(varDecl.type());
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

// --evm-storage-layout: persistent state vars read from their EVM
// slot address. Aggregates never build here — index/member/write
// accesses intercept on the AST above this identifier.
// Caller guards profile + non-constant/immutable + non-transient.
std::shared_ptr<awst::Expression> buildSlotModeStateRead(
	eb::ContractContext& ctx, Context& scope, Identifier const& ident,
	VariableDeclaration const& varDecl, std::string const& name,
	awst::SourceLocation const& loc)
{
	if (varDecl.type() && varDecl.type()->isValueType())
	{
		EvmSlotLowering low(ctx, scope, loc);
		if (auto addr = low.resolve(ident))
			return low.readValue(*addr);
		return nullptr;
	}
	if (EvmSlotLowering::isBytesLike(varDecl.type()))
	{
		EvmSlotLowering low(ctx, scope, loc);
		if (auto addr = low.resolve(ident))
			return low.readBytesValue(*addr);
		return nullptr;
	}
	if (dynamic_cast<StructType const*>(varDecl.type()))
	{
		EvmSlotLowering low(ctx, scope, loc);
		if (auto addr = low.resolve(ident))
			return low.readStructValue(*addr);
		return nullptr;
	}
	if (auto const* at0 = dynamic_cast<ArrayType const*>(varDecl.type());
		at0 && !at0->isByteArrayOrString())
	{
		EvmSlotLowering low(ctx, scope, loc);
		if (auto addr = low.resolve(ident))
			return low.readArrayValue(*addr, at0);
		return nullptr;
	}
	// MAPPING as a value: the only legal contexts are pointer
	// bindings (decl init, ptr assignment, tuple components, fn
	// args — solc rejects everything else), and the pointer
	// convention IS the biguint slot. Hand it over directly.
	// Same for aggregates CONTAINING mappings (mapping(...)[]): they
	// are equally uncopyable, so a value-use is always a binding.
	if (dynamic_cast<MappingType const*>(varDecl.type())
		|| builder::containsMappingType(varDecl.type()))
	{
		EvmSlotLowering low(ctx, scope, loc);
		if (auto addr = low.resolve(ident))
			return addr->slot;
		return nullptr;
	}
	Logger::instance().error(
		"--evm-storage-layout: aggregate state variable '" + name
		+ "' used as a value is not yet supported", loc);
	return nullptr;
}

// State-variable read. Caller guards isStateVariable().
std::shared_ptr<awst::Expression> buildStateVarRead(
	eb::ContractContext& ctx, Context& scope, Identifier const& ident,
	VariableDeclaration const& varDecl, std::string const& name,
	awst::SourceLocation const& loc)
{
	auto* type = ctx.typeMapper.map(varDecl.type());

	if (ctx.typeMapper.profile().evmStorageLayout
		&& !varDecl.isConstant() && !varDecl.immutable()
		&& varDecl.referenceLocation() != VariableDeclaration::Location::Transient)
		return buildSlotModeStateRead(ctx, scope, ident, varDecl, name, loc);

	// Mapping used as a VALUE (`r = a;` where r is a storage-pointer alias):
	// return its name as bytes so the alias holds the box-key prefix.
	// Mapping has no own value; SolIndexAccess still builds box access from the name.
	if (varDecl.type()
		&& varDecl.type()->category() == solidity::frontend::Type::Category::Mapping)
	{
		return awst::makeUtf8BytesConstant(name, loc, awst::WType::bytesType());
	}

	// Transient state vars: packed blob in the transient scratch slot
	// (same slot asm tload/tstore uses); StorageBackend dispatches to TransientStorage.
	if (varDecl.referenceLocation() == VariableDeclaration::Location::Transient
		&& ctx.storageBackend && ctx.storageBackend->isTransient(varDecl))
	{
		if (auto read = ctx.storageBackend->emitReadForVar(varDecl, name, type, loc))
			return read;
	}

	auto binding = ctx.storageMapper.physicalBindingFor(varDecl);

	// Dynamic arrays in box storage: placeholder
	if (type && type->kind() == awst::WTypeKind::ReferenceArray
		&& binding.kind == awst::AppStorageKind::Box)
	{
		auto placeholder = awst::makeVarExpression(name, type, loc);
		return placeholder;
	}

	// Constants (redundant guard); immutables always read from state.
	if (varDecl.isConstant() && varDecl.value())
		return ctx.buildExpr(*varDecl.value());

	return ctx.storageMapper.createStateRead(
		binding.name, type, binding.kind, loc);
}

// Dynamic CALLDATA param with LIVE pointer locals (an assembly block seeded or
// repointed __cd_off_<name>/__cd_len_<name>): the param's VALUE is the byte
// range the pointer designates inside __cd_blob — `assembly { x.offset := 1 }
// return x;` must honour the repoint (EVM calldata-pointer semantics), not
// return the originally-decoded param.
std::shared_ptr<awst::Expression> tryLiveCalldataPointerValue(
	eb::ContractContext& ctx, Declaration const* decl,
	std::string const& name, awst::SourceLocation const& loc)
{
	auto const* vd = dynamic_cast<VariableDeclaration const*>(decl);
	if (!vd || vd->referenceLocation() != solidity::frontend::VariableDeclaration::Location::CallData)
		return nullptr;
	auto* live = ctx.currentScope ? ctx.currentScope->liveCalldataPointers() : nullptr;
	if (!live || !live->count(name))
		return nullptr;
	return builder::TypeCoercion::calldataPointerValueRead(name, loc);
}

// Regular local variable.
std::shared_ptr<awst::Expression> buildLocalVarExpression(
	eb::ContractContext& ctx, Context& scope, Declaration const* decl,
	std::string const& name, solidity::frontend::Type const* solType,
	awst::SourceLocation const& loc)
{
	auto e = std::make_shared<awst::VarExpression>();
	e->sourceLocation = loc;
	if (decl)
	{
		if (auto const* vd = dynamic_cast<VariableDeclaration const*>(decl))
		{
			e->name = scope.awstVarName(*vd); // renamed for shadowing
			e->wtype = ctx.typeMapper.map(vd->type());
		}
		else
			e->name = name;
	}
	else
		e->name = name;
	if (!e->wtype || e->wtype == awst::WType::voidType())
	{
		if (solType)
			e->wtype = ctx.typeMapper.map(solType);
	}
	return e;
}

} // anonymous namespace

std::shared_ptr<awst::Expression> SolIdentifier::toAwst()
{
	std::string name = m_ident.name();

	if (name == "this")
		return buildThisValue(m_loc);

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
		if (isSlotStorageLocal(m_ctx, *varDecl))
			return awst::makeVarExpression(
				varDecl->name(), awst::WType::biguintType(), m_loc);

		if (auto blobValue = tryBlobAggregateValue(m_ctx, m_scope, *varDecl, m_loc))
			return blobValue;

		if (auto refValue = tryStructRefParamValue(m_ctx, m_scope, *varDecl, name, m_loc))
			return refValue;

		if (varDecl->isConstant() && varDecl->value())
			return buildConstantValue(m_ctx, *varDecl);

		if (varDecl->isStateVariable())
			return buildStateVarRead(m_ctx, m_scope, m_ident, *varDecl, name, m_loc);
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

	if (auto cdValue = tryLiveCalldataPointerValue(m_ctx, decl, name, m_loc))
		return cdValue;

	return buildLocalVarExpression(m_ctx, m_scope, decl, name, m_solType, m_loc);
}

} // namespace puyasol::builder::sol_ast
