#include "builder/sol-ast/MappingPrefix.h"
#include "builder/sol-ast/Context.h"
#include "builder/sol-ast/StorageRefPointer.h"
#include "builder/sol-eb/ContractContext.h"
#include "builder/sol-types/TypeCoercion.h"
#include "builder/storage/StorageKey.h"
#include "builder/storage/StorageMapper.h"
#include "builder/storage/StoragePlace.hpp"
#include "awst/NameGen.h"

#include <libsolidity/ast/AST.h>

namespace puyasol::builder::sol_ast
{
using namespace solidity::frontend;

namespace
{
std::shared_ptr<awst::Expression> pin(eb::ContractContext& ctx,
	std::shared_ptr<awst::Expression> value, awst::SourceLocation const& loc)
{
	if (dynamic_cast<awst::BytesConstant const*>(value.get())
		|| dynamic_cast<awst::IntegerConstant const*>(value.get())) return value;
	auto variable = awst::makeVarExpression("__holder_" + std::to_string(
		awst::NameGen::next("MappingPrefix.pin")), value->wtype, loc);
	ctx.preEffects().push_back(awst::makeAssignmentStatement(variable, std::move(value), loc));
	return variable;
}

StorageHolder withValue(eb::ContractContext& ctx, std::shared_ptr<awst::Expression> key,
	Type const* type, awst::SourceLocation const& loc)
{
	key = pin(ctx, std::move(key), loc);
	if (dynamic_cast<MappingType const*>(type)) return {key, key};
	auto const* wt = ctx.typeMapper.map(type);
	auto box = awst::makeBoxValueExpression(key, wt, loc);
	return {key, StorageMapper::makeStateGetWithDefault(std::move(box), wt, loc)};
}

StorageHolder member(StorageHolder base, StructType const& type,
	std::string const& name, awst::WType const* valueType, awst::SourceLocation const& loc)
{
	if (!base.key) return {};
	if (transparentMappingWrapper(&type))
		return base; // same represented fields and addressable data path
	return {StorageKey::member(std::move(base.key), type, name, loc),
		awst::makeFieldExpression(std::move(base.value), name, valueType, loc)};
}

StorageHolder element(eb::ContractContext& ctx, StorageHolder base, ArrayType const& type,
	std::shared_ptr<awst::Expression> index, awst::SourceLocation const& loc)
{
	if (!base.key || !base.value) return {};
	index = pin(ctx, TypeCoercion::checkedIndexToUint64(ctx.preEffects(), std::move(index), loc), loc);
	std::shared_ptr<awst::Expression> bound;
	if (type.isDynamicallySized())
		bound = awst::makeArrayLength(base.value, awst::WType::uint64Type(), loc);
	else
		bound = awst::makeIntegerConstant(checkedSize<uint64_t>(type.length(), "holder array bound"), loc);
	ctx.preEffects().push_back(awst::makeExpressionStatement(awst::makeAssert(
		awst::makeNumericCompare(index, awst::NumericComparison::Lt, std::move(bound), loc),
		loc, "array index out of bounds"), loc));
	return {StorageKey::arrayElement(std::move(base.key), index, loc),
		awst::makeIndexExpression(std::move(base.value), std::move(index),
			ctx.typeMapper.mapSolTypeToARC4(type.baseType()), loc)};
}
}

StorageHolder resolveBuiltStorageHolder(eb::ContractContext& ctx,
	std::shared_ptr<awst::Expression> const& value, awst::SourceLocation const& loc)
{
	if (!value) return {};
	if (auto box = std::dynamic_pointer_cast<awst::BoxValueExpression>(value))
	{
		auto copy = std::make_shared<awst::BoxValueExpression>(*box);
		copy->key = pin(ctx, box->key, loc);
		return {copy->key, copy};
	}
	if (auto cell = std::dynamic_pointer_cast<awst::AppStateExpression>(value))
	{
		auto copy = std::make_shared<awst::AppStateExpression>(*cell);
		copy->key = pin(ctx, cell->key, loc);
		return {copy->key, copy};
	}
	if (auto get = std::dynamic_pointer_cast<awst::StateGet>(value))
	{
		auto result = resolveBuiltStorageHolder(ctx, get->field, loc);
		if (result.key)
		{
			auto copy = std::make_shared<awst::StateGet>(*get);
			copy->field = std::move(result.value);
			result.value = std::move(copy);
		}
		return result;
	}
	if (auto cast = std::dynamic_pointer_cast<awst::ReinterpretCast>(value))
	{
		auto result = resolveBuiltStorageHolder(ctx, cast->expr, loc);
		if (result.key) result.value = awst::makeReinterpretCast(std::move(result.value), value->wtype, loc);
		return result;
	}
	if (auto field = std::dynamic_pointer_cast<awst::FieldExpression>(value))
	{
		auto const* type = dynamic_cast<StructType const*>(ctx.typeMapper.solcAggregateFor(field->base->wtype));
		if (!type) throw SizeError("storage holder alias lacks solc struct facts");
		return member(resolveBuiltStorageHolder(ctx, field->base, loc), *type, field->name, value->wtype, loc);
	}
	if (auto index = std::dynamic_pointer_cast<awst::IndexExpression>(value))
	{
		auto const* type = dynamic_cast<ArrayType const*>(ctx.typeMapper.solcAggregateFor(index->base->wtype));
		if (!type) throw SizeError("storage holder alias lacks solc array facts");
		return element(ctx, resolveBuiltStorageHolder(ctx, index->base, loc), *type, index->index, loc);
	}
	if (value->wtype == awst::WType::bytesType() || value->wtype == awst::WType::boxKeyType())
	{
		auto key = pin(ctx, value, loc);
		return {key, key};
	}
	return {};
}

StorageHolder resolveStorageHolder(eb::ContractContext& ctx, Context& scope,
	Expression const& expression, awst::SourceLocation const& loc)
{
	if (auto const* id = dynamic_cast<Identifier const*>(&expression))
	{
		auto const* declaration = id->annotation().referencedDeclaration;
		if (!declaration) return {};
		auto const& parameter = scope.findMappingKeyParam(declaration->id());
		if (!parameter.empty())
			return withValue(ctx, awst::makeVarExpression(parameter, awst::WType::bytesType(), loc),
				expression.annotation().type, loc);
		if (auto const* alias = scope.findStorageAlias(declaration->id()))
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
	if (auto const* field = dynamic_cast<MemberAccess const*>(&expression))
	{
		if (auto const* type = dynamic_cast<StructType const*>(field->expression().annotation().type))
			return member(resolveStorageHolder(ctx, scope, field->expression(), loc), *type,
				field->memberName(), ctx.typeMapper.mapSolTypeToARC4(expression.annotation().type), loc);
		// Qualified inherited declaration, e.g. Base.mappingVar.
		if (auto const* var = dynamic_cast<VariableDeclaration const*>(field->annotation().referencedDeclaration);
			var && var->isStateVariable())
		{
			auto binding = ctx.storageMapper.physicalBindingFor(*var);
			return withValue(ctx, awst::makeUtf8BytesConstant(binding.key, loc), var->type(), loc);
		}
	}
	if (auto const* index = dynamic_cast<IndexAccess const*>(&expression))
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
	if (dynamic_cast<FunctionCall const*>(&expression))
		return withValue(ctx, ctx.buildExpr(expression), expression.annotation().type, loc);
	if (dynamic_cast<Conditional const*>(&expression)
		&& dynamic_cast<MappingType const*>(expression.annotation().type))
		return resolveBuiltStorageHolder(ctx, ctx.buildExpr(expression), loc);
	if (auto const* tuple = dynamic_cast<TupleExpression const*>(&expression);
		tuple && !tuple->isInlineArray() && tuple->components().size() == 1 && tuple->components()[0])
		return resolveStorageHolder(ctx, scope, *tuple->components()[0], loc);
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
