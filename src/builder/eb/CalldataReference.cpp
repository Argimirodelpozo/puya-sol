#include "builder/eb/CalldataReference.h"
#include "builder/ast/calls/SolInternalCall.h"
#include "builder/codec/ByteSlice.h"
#include "builder/codec/EvmAbiDecode.h"
#include "builder/codec/EvmValueCodec.h"
#include "builder/context/ContractContext.h"
#include "builder/eb/CallOperands.h"
#include "builder/solc/SolcFacts.h"
#include "builder/types/TypeCoercion.h"

#include <libsolidity/ast/AST.h>

namespace puyasol::builder::sol_ast
{
using namespace solidity::frontend;
using Expr = CalldataReference::Expr;
namespace
{
Expr word(uint64_t value, awst::SourceLocation const& loc) { return awst::makeBiguintConstant(std::to_string(value), loc); }
Expr blob(awst::SourceLocation const& loc) { return awst::makeVarExpression("__cd_blob", awst::WType::bytesType(), loc); }
Expr add(Expr left, Expr right, awst::SourceLocation const& loc)
{
	return awst::makeBigUIntBinOp(awst::makeBigUIntBinOp(std::move(left), awst::BigUIntBinaryOperator::Add,
		std::move(right), loc), awst::BigUIntBinaryOperator::Mod, makePow256(loc), loc);
}
Expr load(eb::ContractContext& ctx, Expr data, Expr offset, awst::SourceLocation const& loc)
{
	return awst::makeAsBiguint(readPaddedBytes(ctx.typeMapper, std::move(data), std::move(offset),
		awst::makeIntegerConstant(32, loc), loc), loc);
}
ArrayType const* arrayType(Type const* type)
{
	if (auto const* slice = dynamic_cast<ArraySliceType const*>(type)) return &slice->arrayType();
	return dynamic_cast<ArrayType const*>(type);
}
void require(eb::ContractContext& ctx, Expr condition, awst::SourceLocation const& loc, char const* message)
{
	ctx.queuePreExpression(awst::makeAssert(std::move(condition), loc, message), loc);
}
CalldataReference atHead(eb::ContractContext& ctx, Type const* type, Expr data, Expr base, Expr head,
	awst::SourceLocation const& loc)
{
	if (!data) data = blob(loc);
	CalldataReference result{type, head, nullptr, data};
	if (type->isDynamicallyEncoded())
	{
		// solc's access_calldata_tail validates the tail before exposing its
		// pointer. Static scalar loads instead retain calldataload zero padding.
		auto relative = ctx.emitSequencedOperand({}, load(ctx, data, head, loc), true, loc);
		auto end = TypeCoercion::coerceScalar(awst::makeLen(data, loc), awst::WType::biguintType(), loc);
		auto absolute = awst::makeBigUIntBinOp(base, awst::BigUIntBinaryOperator::Add, relative, loc);
		require(ctx, awst::makeNumericCompare(awst::makeBigUIntBinOp(absolute,
			awst::BigUIntBinaryOperator::Add, word(type->calldataEncodedTailSize(), loc), loc),
			awst::NumericComparison::Lte, end, loc), loc, "invalid calldata tail");
		result.offset = ctx.emitSequencedOperand({}, absolute, true, loc);
		if (CalldataReference::hasLength(type))
		{
			result.length = ctx.emitSequencedOperand({}, load(ctx, data, result.offset, loc), true, loc);
			result.offset = add(result.offset, word(32, loc), loc);
			require(ctx, awst::makeNumericCompare(result.length, awst::NumericComparison::Lte,
				awst::makeBiguintConstant("18446744073709551615", loc), loc), loc, "invalid calldata length");
			auto extent = awst::makeBigUIntBinOp(result.length, awst::BigUIntBinaryOperator::Mult,
				word(arrayType(type)->calldataStride(), loc), loc);
			require(ctx, awst::makeNumericCompare(awst::makeBigUIntBinOp(result.offset,
				awst::BigUIntBinaryOperator::Add, extent, loc), awst::NumericComparison::Lte, end, loc),
				loc, "calldata tail too short");
		}
	}
	if (auto const* array = arrayType(type); array && !array->isDynamicallySized())
		result.length = awst::makeBiguintConstant(array->length().str(), loc);
	return result;
}
}

bool CalldataReference::hasLength(Type const* type)
{
	for (auto const& [name, part]: type->stackItems()) if (name == "length") return true;
	return false;
}

std::optional<CalldataReference> CalldataReference::local(Context const& scope,
	VariableDeclaration const& declaration, awst::SourceLocation const& loc)
{
	auto const name = scope.awstVarName(declaration);
	if (declaration.referenceLocation() != VariableDeclaration::Location::CallData
		|| !scope.function || !scope.function->hasAssemblyCalldata
		|| !scope.function->calldataDeclarations.contains(declaration.id())) return std::nullopt;
	CalldataReference result{declaration.type(),
		awst::makeVarExpression("__cd_off_" + name, awst::WType::biguintType(), loc), nullptr,
		awst::makeVarExpression("__cd_data_" + name, awst::WType::bytesType(), loc)};
	if (hasLength(result.type))
		result.length = awst::makeVarExpression("__cd_len_" + name, awst::WType::biguintType(), loc);
	else if (auto const* array = arrayType(result.type))
		result.length = awst::makeBiguintConstant(array->length().str(), loc);
	return result;
}

std::optional<CalldataReference> CalldataReference::resolve(eb::ContractContext& ctx,
	Expression const& source, awst::SourceLocation const& loc)
{
	if (!ctx.scope().function || !ctx.scope().function->hasAssemblyCalldata) return std::nullopt;
	auto const& node = SolcFacts::unparenthesized(source);
	if (auto const* call = SolcFacts::expressionAs<FunctionCall>(&node);
		call && node.annotation().type->dataStoredIn(DataLocation::CallData))
	{
		if (*call->annotation().kind == FunctionCallKind::TypeConversion && call->arguments().size() == 1)
			if (auto reference = resolve(ctx, *call->arguments().front(), loc))
			{
				reference->type = node.annotation().type;
				return reference;
			}
		if (auto const* function = dynamic_cast<FunctionType const*>(call->expression().annotation().type);
			function && function->kind() == FunctionType::Kind::Internal)
			return unpack(node.annotation().type, ctx.emitSequencedOperand({},
				SolInternalCall(ctx, *call).toReferenceAwst(), true, loc), loc);
	}
	if (auto const* id = SolcFacts::expressionAs<Identifier>(&node))
		if (auto const* declaration = dynamic_cast<VariableDeclaration const*>(id->annotation().referencedDeclaration))
			return local(ctx.scope(), *declaration, loc);
	if (auto const* assignment = SolcFacts::expressionAs<Assignment>(&node))
		if (auto const* id = SolcFacts::expressionAs<Identifier>(&assignment->leftHandSide()))
			if (auto const* declaration = dynamic_cast<VariableDeclaration const*>(id->annotation().referencedDeclaration);
				declaration && declaration->referenceLocation() == VariableDeclaration::Location::CallData)
				if (auto reference = resolve(ctx, assignment->rightHandSide(), loc))
				{
					reference->bind(ctx, *declaration, loc);
					return local(ctx.scope(), *declaration, loc);
				}
	if (auto const* conditional = SolcFacts::expressionAs<Conditional>(&node);
		conditional && node.annotation().type->dataStoredIn(DataLocation::CallData))
	{
		auto condition = ctx.pinIfWriteBacks(ctx.lower(conditional->condition(), false), loc);
		auto branch = [&](Expression const& expression) {
			return ctx.lowerOperand([&] {
				auto reference = resolve(ctx, expression, loc);
				if (!reference) throw std::runtime_error("Cannot resolve conditional calldata reference");
				return reference->pack(loc);
			});
		};
		auto yes = branch(conditional->trueExpression());
		auto no = branch(conditional->falseExpression());
		return unpack(node.annotation().type, ctx.emitConditional(std::move(condition),
			std::move(yes), std::move(no), calldataReferenceType(), loc), loc);
	}
	if (auto const* member = SolcFacts::expressionAs<MemberAccess>(&node))
	{
		if (auto const* magic = dynamic_cast<MagicType const*>(member->expression().annotation().type);
			magic && magic->kind() == MagicType::Kind::Message && member->memberName() == "data")
		{
			auto data = ctx.emitSequencedOperand({}, ctx.buildExpr(node), true, loc);
			return CalldataReference{node.annotation().type, word(0, loc),
				TypeCoercion::coerceScalar(awst::makeLen(data, loc), awst::WType::biguintType(), loc), data};
		}
		if (auto const* structure = dynamic_cast<StructType const*>(member->expression().annotation().type))
			if (auto base = resolve(ctx, member->expression(), loc))
				return atHead(ctx, node.annotation().type, base->data, base->offset,
					add(base->offset, word(structure->calldataOffsetOfMember(member->memberName()), loc), loc), loc);
	}
	if (auto const* index = SolcFacts::expressionAs<IndexAccess>(&node); index && index->indexExpression())
		if (auto base = resolve(ctx, index->baseExpression(), loc))
		{
			auto const* array = arrayType(base->type);
			if (!array) return std::nullopt;
			base->offset = ctx.emitSequencedOperand({}, base->offset, true, loc);
			base->length = ctx.emitSequencedOperand({}, base->length, true, loc);
			base->data = ctx.emitSequencedOperand({}, base->data, true, loc);
			auto i = TypeCoercion::coerceScalar(CallOperands::evaluate(ctx, *index->indexExpression(), loc),
				awst::WType::biguintType(), loc);
			require(ctx, awst::makeNumericCompare(i, awst::NumericComparison::Lt, base->length, loc), loc, "array index out of bounds");
			auto position = add(base->offset, awst::makeBigUIntBinOp(i, awst::BigUIntBinaryOperator::Mult,
				word(array->calldataStride(), loc), loc), loc);
			auto result = atHead(ctx, node.annotation().type, base->data, base->offset, position, loc);
			result.packedByte = array->isByteArrayOrString();
			return result;
		}
	if (auto const* slice = SolcFacts::expressionAs<IndexRangeAccess>(&node))
		if (auto base = resolve(ctx, slice->baseExpression(), loc))
		{
			base->offset = ctx.emitSequencedOperand({}, base->offset, true, loc);
			base->length = ctx.emitSequencedOperand({}, base->length, true, loc);
			base->data = ctx.emitSequencedOperand({}, base->data, true, loc);
			auto bound = [&](Expression const* expression, Expr fallback) {
				return expression ? TypeCoercion::coerceScalar(CallOperands::evaluate(ctx, *expression, loc),
					awst::WType::biguintType(), loc) : fallback;
			};
			auto start = bound(slice->startExpression(), word(0, loc));
			auto end = bound(slice->endExpression(), base->length);
			require(ctx, awst::makeNumericCompare(start, awst::NumericComparison::Lte, end, loc), loc, "slice starts after end");
			require(ctx, awst::makeNumericCompare(end, awst::NumericComparison::Lte, base->length, loc), loc, "slice exceeds length");
			base->offset = add(base->offset, awst::makeBigUIntBinOp(start, awst::BigUIntBinaryOperator::Mult,
				word(arrayType(base->type)->calldataStride(), loc), loc), loc);
			base->length = awst::makeBigUIntBinOp(end, awst::BigUIntBinaryOperator::Sub, start, loc);
			return base;
		}
	return std::nullopt;
}

CalldataReference::Expr CalldataReference::pack(awst::SourceLocation const& loc) const
{
	auto tuple = awst::makeTupleExpression(calldataReferenceType(), loc);
	tuple->items = {offset, length ? length : word(0, loc), data ? data : blob(loc)};
	return tuple;
}

std::optional<CalldataReference> CalldataReference::unpack(Type const* type, Expr value,
	awst::SourceLocation const& loc)
{
	if (value->wtype != calldataReferenceType()) return std::nullopt;
	CalldataReference result{type, awst::makeTupleItem(value, 0, awst::WType::biguintType(), loc),
		awst::makeTupleItem(value, 1, awst::WType::biguintType(), loc),
		awst::makeTupleItem(value, 2, awst::WType::bytesType(), loc)};
	if (auto const* array = arrayType(type); array && !array->isDynamicallySized())
		result.length = awst::makeBiguintConstant(array->length().str(), loc);
	return result;
}

void CalldataReference::bind(eb::ContractContext& ctx, VariableDeclaration const& declaration,
	awst::SourceLocation const& loc) const
{
	auto const name = ctx.scope().awstVarName(declaration);
	// Freeze both coordinates before changing either side of a rebinding.
	auto off = ctx.emitSequencedOperand({}, offset, true, loc);
	auto len = hasLength(declaration.type()) ? ctx.emitSequencedOperand({}, length, true, loc) : nullptr;
	auto bytes = ctx.emitSequencedOperand({}, data ? data : blob(loc), true, loc);
	ctx.queuePreEffect(awst::makeAssignmentStatement(
		awst::makeVarExpression("__cd_off_" + name, awst::WType::biguintType(), loc), off, loc));
	if (len) ctx.queuePreEffect(awst::makeAssignmentStatement(
		awst::makeVarExpression("__cd_len_" + name, awst::WType::biguintType(), loc), len, loc));
	ctx.queuePreEffect(awst::makeAssignmentStatement(
		awst::makeVarExpression("__cd_data_" + name, awst::WType::bytesType(), loc), bytes, loc));
	ctx.scope().function->calldataDeclarations.insert(declaration.id());
}

Expr CalldataReference::read(eb::ContractContext& ctx, awst::SourceLocation const& loc) const
{
	if (codec::isWordType(type))
	{
		auto bytes = readPaddedBytes(ctx.typeMapper, data ? data : blob(loc), offset,
			awst::makeIntegerConstant(packedByte ? 1 : 32, loc), loc);
		if (packedByte) bytes = awst::makeRightPad(std::move(bytes), 31, loc);
		return codec::valueFromEvmWord(ctx.typeMapper, type, std::move(bytes), loc,
			ctx.preEffects(), codec::PaddingPolicy::Validate);
	}
	return abi::readCalldataValue(ctx.typeMapper, data ? data : blob(loc), type, offset, length, loc, ctx.preEffects());
}

} // namespace puyasol::builder::sol_ast
