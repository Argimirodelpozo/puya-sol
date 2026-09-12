#include "builder/sol-ast/calls/RevertBlob.h"
#include "builder/sol-ast/CallOperands.h"
#include "builder/SelectorSemantics.h"
#include "builder/abi/AbiEncoderBuilder.h"
#include "builder/sol-types/ConversionPlan.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/sol-eb/ContractContext.h"

namespace puyasol::builder::sol_ast
{

RevertPayload::RevertPayload(
	std::shared_ptr<awst::Expression> reason, awst::SourceLocation const& loc)
{
	if (auto const* literal = dynamic_cast<awst::StringConstant const*>(reason.get()))
	{
		message = literal->value;
		blob = awst::makeBytesConstant(errorStringRevertBlobBytes(message), loc);
	}
	else
		blob = makeErrorStringRevertBlob(std::move(reason), loc);
}

RevertPayload::RevertPayload(eb::ContractContext& ctx,
	solidity::frontend::FunctionCall const& error, awst::SourceLocation const& loc)
{
	using namespace solidity::frontend;
	auto const* definition = dynamic_cast<ErrorDefinition const*>(
		ASTNode::referencedDeclaration(error.expression()));
	assert(definition);
	auto const* type = definition->functionType(true);
	message = definition->name();
	blob = SelectorSemantics::functionSelector(ctx, *type, type->externalSignature(), loc);
	auto values = CallOperands::build(ctx, error, loc, [&](Expression const& source, size_t i) {
		auto const* param = type->parameterTypes().at(i);
		return ConversionPlan{source.annotation().type, param, ctx.typeMapper.map(param),
			ConversionPlan::Context::AbiArgument}.emit(ctx.buildExpr(source), loc);
	});
	if (!values.empty())
		blob = awst::makeConcat(std::move(blob),
			eb::AbiEncoderBuilder::arc4EncodeValues(ctx, std::move(values), loc), loc);
}

} // namespace puyasol::builder::sol_ast
