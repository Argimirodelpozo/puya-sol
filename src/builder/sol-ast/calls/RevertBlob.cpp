#include "builder/sol-ast/calls/RevertBlob.h"
#include "builder/sol-ast/CallOperands.h"
#include "builder/SelectorSemantics.h"
#include "builder/BuildArtifacts.h"
#include "builder/abi/AbiEncoderBuilder.h"
#include "builder/sol-types/ConversionPlan.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/sol-eb/ContractContext.h"

namespace puyasol::builder::sol_ast
{

RevertPayload::RevertPayload(
	eb::ContractContext& ctx, std::shared_ptr<awst::Expression> reason,
	awst::SourceLocation const& loc)
{
	if (auto const* literal = dynamic_cast<awst::StringConstant const*>(reason.get()))
		message = literal->value;

	// Keep only the message at each call site, not a complete padded ABI blob.
	// CallOperands already evaluates message expressions on the success path;
	// the caller places this payload expression exclusively on the failure path.
	std::string const id = "__puyasol_error_string";
	auto const* bytes = awst::WType::bytesType();
	auto call = awst::makeSubroutineCall(awst::SubroutineID{id}, bytes, loc);
	awst::pushCallArg(call->args, reason->wtype == bytes
		? std::move(reason) : awst::makeAsBytes(std::move(reason), loc));
	blob = std::move(call);
	auto& subs = ctx.typeMapper.artifacts().bufferSubroutines;
	if (subs.contains(id)) return;

	auto text = awst::makeVarExpression("message", bytes, loc);
	std::vector<uint8_t> head = {0x08, 0xc3, 0x79, 0xa0};
	appendRevertWord(head, 0x20);
	auto length = awst::makeLeftPad(
		awst::makeItob(awst::makeLen(text, loc), loc), 24, loc);
	auto payload = awst::makeConcat(
		awst::makeConcat(awst::makeBytesConstant(std::move(head), loc),
			std::move(length), loc), awst::makeRightPadTo32Multiple(text, loc), loc);
	auto body = awst::makeBlock(loc);
	body->body.push_back(awst::makeReturnStatement(std::move(payload), loc));
	auto sub = awst::makeSubroutine(id, id, {{"message", bytes, loc}},
		bytes, std::move(body), true, loc);
	sub->inlineOpt = false;
	subs.emplace(id, std::move(sub));
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
