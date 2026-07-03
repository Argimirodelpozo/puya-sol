/// @file SolIdentifier.cpp — variable/constant/state variable resolution.

#include "builder/sol-ast/exprs/SolIdentifier.h"
#include "builder/itxn/FunctionPointerBuilder.h"
#include "builder/storage/StorageBackend.h"
#include "builder/storage/StorageMapper.h"
#include "builder/storage/TransientStorage.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/sol-types/TypeCoercion.h"

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
		// Blob-backed aggregate (>4KB): variable travels as uint64 base offset.
		// Bare reference resolves to that offset; field/index access go through
		// SolIndexAccess/resolveBlobOffset.
		if (auto off = m_scope.findBlobAggregate(varDecl->id()); !off.empty())
			return awst::makeVarExpression(off, awst::WType::uint64Type(), m_loc);

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
