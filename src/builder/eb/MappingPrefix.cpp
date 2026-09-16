#include "builder/eb/MappingPrefix.h"
#include "builder/solc/SolcFacts.h"
#include "builder/context/TranslationContext.h"
#include "builder/solc/StorageRefPointer.h"
#include "builder/context/ContractContext.h"
#include "builder/storage/StorageMapper.h"
#include "builder/storage/StoragePlace.hpp"

#include <libsolidity/ast/AST.h>

namespace puyasol::builder::sol_ast
{
using namespace solidity::frontend;

namespace
{
StorageHolder withValue(eb::ContractContext& ctx, std::shared_ptr<awst::Expression> key,
	Type const* type, awst::SourceLocation const& loc)
{
	key = ctx.emitSequencedOperand({}, std::move(key), true, loc);
	if (dynamic_cast<MappingType const*>(type)) return {key, key};
	return {key, StoragePathWalker::boxedValue(ctx.typeMapper, key, type, loc)};
}

StorageHolder element(eb::ContractContext& ctx, StorageHolder base, ArrayType const& type,
	std::shared_ptr<awst::Expression> index, awst::SourceLocation const& loc)
{
	if (!base.key || !base.value) return {};
	return StoragePathWalker(ctx.typeMapper, &type, loc)
		.step(std::move(base), std::move(index), ctx.preEffects());
}
}

StorageHolder resolveBuiltStorageHolder(eb::ContractContext& ctx,
	std::shared_ptr<awst::Expression> const& value, awst::SourceLocation const& loc)
{
	if (!value) return {};
	if (dynamic_cast<awst::BoxValueExpression const*>(value.get())
		|| dynamic_cast<awst::AppStateExpression const*>(value.get()))
	{
		auto place = StoragePlace::fromRead(value);
		auto key = ctx.emitSequencedOperand({}, place->key, true, loc);
		return {key, place->makeField(key, loc)};
	}
	if (dynamic_cast<awst::StateGet const*>(value.get())
		|| dynamic_cast<awst::ReinterpretCast const*>(value.get()))
	{
		auto result = resolveBuiltStorageHolder(ctx, StoragePlace::projectionBase(value), loc);
		if (result.key) result.value = StoragePlace::withProjectionBase(value, std::move(result.value));
		return result;
	}
	if (auto field = std::dynamic_pointer_cast<awst::FieldExpression>(value))
	{
		auto const* type = dynamic_cast<StructType const*>(ctx.typeMapper.solcAggregateFor(field->base->wtype));
		if (!type) throw SizeError("storage holder alias lacks solc struct facts");
		return StoragePathWalker::member(resolveBuiltStorageHolder(ctx, field->base, loc), *type, field->name, value->wtype, loc);
	}
	if (auto index = std::dynamic_pointer_cast<awst::IndexExpression>(value))
	{
		auto const* type = dynamic_cast<ArrayType const*>(ctx.typeMapper.solcAggregateFor(index->base->wtype));
		if (!type) throw SizeError("storage holder alias lacks solc array facts");
		return element(ctx, resolveBuiltStorageHolder(ctx, index->base, loc), *type, index->index, loc);
	}
	if (value->wtype == awst::WType::bytesType() || value->wtype == awst::WType::boxKeyType())
	{
		auto key = ctx.emitSequencedOperand({}, value, true, loc);
		return {key, key};
	}
	return {};
}

StorageHolder resolveStorageHolder(eb::ContractContext& ctx, Context& scope,
	Expression const& source, awst::SourceLocation const& loc)
{
	auto const& expression = SolcFacts::unparenthesized(source);
	if (auto const* id = SolcFacts::expressionAs<Identifier>(&expression))
	{
		auto const* declaration = id->annotation().referencedDeclaration;
		if (!declaration) return {};
		auto const& parameter = scope.bindings.mappingKeyParams.get(declaration->id());
		if (!parameter.empty())
			return withValue(ctx, awst::makeVarExpression(parameter, awst::WType::bytesType(), loc),
				expression.annotation().type, loc);
		if (auto const* alias = scope.bindings.storageAliases.find(declaration->id()))
			return resolveBuiltStorageHolder(ctx, alias->expr, loc);
		if (auto const* var = dynamic_cast<VariableDeclaration const*>(declaration);
			var && var->isStateVariable() && !var->isConstant() && !var->immutable())
		{
			auto binding = ctx.storageMapper.physicalBindingFor(*var);
			auto key = awst::makeUtf8BytesConstant(binding.key, loc, awst::WType::boxKeyType());
			return {key, dynamic_cast<MappingType const*>(var->type())
				? std::shared_ptr<awst::Expression>(key) : ctx.storageMapper.createStateRead(binding, loc)};
		}
	}
	if (auto const* field = SolcFacts::expressionAs<MemberAccess>(&expression))
	{
		if (auto const* type = dynamic_cast<StructType const*>(field->expression().annotation().type))
			return StoragePathWalker::member(resolveStorageHolder(ctx, scope, field->expression(), loc), *type,
				field->memberName(), ctx.typeMapper.mapSolTypeToARC4(expression.annotation().type), loc);
		// Qualified inherited declaration, e.g. Base.mappingVar.
		if (auto const* var = dynamic_cast<VariableDeclaration const*>(field->annotation().referencedDeclaration);
			var && var->isStateVariable())
		{
			auto binding = ctx.storageMapper.physicalBindingFor(*var);
			return withValue(ctx, awst::makeUtf8BytesConstant(binding.key, loc), var->type(), loc);
		}
	}
	if (auto const* index = SolcFacts::expressionAs<IndexAccess>(&expression))
	{
		if (auto const* array = dynamic_cast<ArrayType const*>(index->baseExpression().annotation().type);
			array && index->indexExpression())
		{
			auto base = resolveStorageHolder(ctx, scope, index->baseExpression(), loc);
			return element(ctx, std::move(base), *array, ctx.buildExpr(*index->indexExpression()), loc);
		}
		if (dynamic_cast<MappingType const*>(index->baseExpression().annotation().type))
			return resolveBuiltStorageHolder(ctx, ctx.buildExpr(expression), loc);
	}
	if (SolcFacts::expressionAs<FunctionCall>(&expression))
		return withValue(ctx, ctx.buildExpr(expression), expression.annotation().type, loc);
	if (SolcFacts::expressionAs<Conditional>(&expression)
		&& dynamic_cast<MappingType const*>(expression.annotation().type))
		return resolveBuiltStorageHolder(ctx, ctx.buildExpr(expression), loc);
	return {};
}

std::shared_ptr<awst::Expression> storageReferenceKey(
	eb::ContractContext& ctx, Context& scope,
	Expression const& expression, awst::SourceLocation const& loc)
{
	auto holder = resolveStorageHolder(ctx, scope, expression, loc);
	if (!holder.key) throw SizeError("storage reference requires a resolved holder");
	auto const* type = expression.annotation().type;
	if (containsMappingType(type) && !dynamic_cast<MappingType const*>(type))
	{
		auto place = StoragePlace::fromRead(holder.value);
		if (!place || place->kind != StoragePlaceKind::Box)
			throw SizeError("mapping-containing aggregate storage references require a whole-box root; "
				"interior reference paths are unsupported. Pass the enclosing aggregate or its mapping field instead");
	}
	return awst::makeAsBytes(std::move(holder.key), loc);
}

} // namespace puyasol::builder::sol_ast
