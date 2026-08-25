/// @file MappingPrefix.cpp — see MappingPrefix.h.
#include "builder/sol-ast/MappingPrefix.h"
#include "builder/sol-ast/Context.h"
#include "builder/sol-eb/ContractContext.h"
#include "builder/storage/StorageMapper.h"

#include <libsolidity/ast/AST.h>

#include <vector>

namespace puyasol::builder::sol_ast
{

using namespace solidity::frontend;

std::shared_ptr<awst::Expression> resolveHolderRoot(
	eb::ContractContext& _ctx,
	Context& _scope,
	Expression const& _rootExpr,
	awst::SourceLocation const& _loc)
{
	if (auto const* baseId = dynamic_cast<Identifier const*>(&_rootExpr))
	{
		auto const* decl = baseId->annotation().referencedDeclaration;
		if (!decl)
			return nullptr;

		auto const& selfPrefix = _scope.findMappingKeyParam(decl->id());
		if (!selfPrefix.empty())
			return awst::makeVarExpression(
				selfPrefix, awst::WType::bytesType(), _loc);

		if (auto const* alias = _scope.findStorageAlias(decl->id()))
		{
			// Peel wrapper layers in ANY interleaving, COLLECTING the field
			// names the alias walked (`p = st.a` aliases FieldExpression(st,
			// "a"): the holder is utf8(st) ++ "a", not utf8(st) — dropping
			// them keyed p.m[k] and st.a.m[k] to DIFFERENT boxes).
			auto e = alias->expr;
			std::vector<std::string> aliasFields;
			for (;;)
			{
				if (auto sg = std::dynamic_pointer_cast<awst::StateGet>(e))
				{
					e = sg->field;
					continue;
				}
				if (auto rc = std::dynamic_pointer_cast<awst::ReinterpretCast>(e))
				{
					e = rc->expr;
					continue;
				}
				if (auto fe = std::dynamic_pointer_cast<awst::FieldExpression>(e))
				{
					aliasFields.insert(aliasFields.begin(), fe->name);
					e = fe->base;
					continue;
				}
				break;
			}
			std::shared_ptr<awst::Expression> holder;
			if (auto boxVal = std::dynamic_pointer_cast<awst::BoxValueExpression>(e))
				holder = boxVal->key;
			else if (auto appState = std::dynamic_pointer_cast<awst::AppStateExpression>(e))
				holder = appState->key;
			// Holder-name alias (BytesConstant of the state var's encoded
			// name) — itself a valid prefix start.
			else if (std::dynamic_pointer_cast<awst::BytesConstant>(e))
				holder = e;
			if (!holder)
				return nullptr;
			for (auto const& f: aliasFields)
				holder = awst::makeConcat(std::move(holder),
					awst::makeUtf8BytesConstant(f, _loc), _loc);
			return holder;
		}

		if (auto const* vd = dynamic_cast<VariableDeclaration const*>(decl);
			vd && vd->isStateVariable() && !vd->isConstant() && !vd->immutable())
			return awst::makeUtf8BytesConstant(
				_ctx.storageMapper.physicalBindingFor(*vd).name, _loc);
		return nullptr;
	}

	// Mapping element root (`mm[k].…`): the built element's runtime box key.
	if (auto const* baseIdx = dynamic_cast<IndexAccess const*>(&_rootExpr))
		if (dynamic_cast<MappingType const*>(
				baseIdx->baseExpression().annotation().type))
		{
			auto built = awst::unwrapStateGet(_ctx.buildExpr(_rootExpr));
			if (auto const* box =
					dynamic_cast<awst::BoxValueExpression const*>(built.get()))
				return box->key;
		}

	return nullptr;
}

std::shared_ptr<awst::Expression> resolveMappingHolderPrefix(
	eb::ContractContext& _ctx,
	Context& _scope,
	Expression const& _expr,
	awst::SourceLocation const& _loc)
{
	auto const* ma = dynamic_cast<MemberAccess const*>(&_expr);
	if (!ma)
		return nullptr;

	std::vector<std::string> fields{ma->memberName()};
	Expression const* rootE = &ma->expression();
	while (auto const* innerMa = dynamic_cast<MemberAccess const*>(rootE))
	{
		fields.insert(fields.begin(), innerMa->memberName());
		rootE = &innerMa->expression();
	}

	auto holder = resolveHolderRoot(_ctx, _scope, *rootE, _loc);
	if (!holder)
		return nullptr;
	for (auto const& f: fields)
		holder = awst::makeConcat(std::move(holder),
			awst::makeUtf8BytesConstant(f, _loc), _loc);
	return holder;
}

} // namespace puyasol::builder::sol_ast
