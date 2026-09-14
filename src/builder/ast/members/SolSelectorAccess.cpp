/// @file SolSelectorAccess.cpp
/// Static declaration selectors and runtime external-function projections.

#include "builder/ast/members/SolSelectorAccess.h"
#include "builder/codec/SelectorSemantics.h"
#include "builder/solc/SolcFacts.h"
#include "Logger.h"
#include "builder/types/TypeMapper.h"
#include "builder/lowering/itxn/InnerCallHandlers.h"

#include <libsolidity/ast/AST.h>
#include <stdexcept>

namespace puyasol::builder::sol_ast
{

using namespace solidity::frontend;

std::shared_ptr<awst::Expression> SolSelectorAccess::toAwst()
{
	return selectorOf(m_ctx, baseExpression(), m_wtype, m_loc);
}

std::shared_ptr<awst::Expression> SolSelectorAccess::selectorOf(
	eb::ContractContext& ctx, Expression const& source, awst::WType const* resultType,
	awst::SourceLocation const& loc, bool canonical)
{
	return projectFunctionValue(ctx, source, resultType, loc,
		[&](Expression const& source) -> std::shared_ptr<awst::Expression> {
			auto const* type = source.annotation().type;
			if (auto const* meta = dynamic_cast<TypeType const*>(type)) type = meta->actualType();
			auto const* function = dynamic_cast<FunctionType const*>(type);
			if (!function) throw std::logic_error("Selector receiver has no solc function type");

			if (function->hasDeclaration())
			{
				// Only value receivers execute. Contract/module/type names are metadata;
				// building the complete member would unnecessarily construct a pointer.
				if (auto const* member = dynamic_cast<MemberAccess const*>(&source))
				{
					auto const* receiverType = member->expression().annotation().type;
					if (receiverType->category() != Type::Category::TypeType
						&& receiverType->category() != Type::Category::Module)
					{
						auto receiver = ctx.lower(member->expression(), false);
						ctx.emitSequencedOperand(std::move(receiver.effects),
							std::move(receiver.value), true, loc);
					}
				}

				bool const evm = canonical || SelectorSemantics::enabled(ctx.typeMapper);
				auto const& declaration = function->declaration();
				if (auto const* event = dynamic_cast<EventDefinition const*>(&declaration))
					return SelectorSemantics::eventSelector(ctx,
						evm ? function->externalSignature() : SelectorSemantics::eventSignature(ctx, *event),
						resultType, loc);

				std::string signature;
				if (!evm)
				{
					if (auto const* definition = dynamic_cast<FunctionDefinition const*>(&declaration))
						signature = eb::InnerCallHandlers::buildMethodSelector(ctx, definition);
					else if (dynamic_cast<VariableDeclaration const*>(&declaration))
						signature = eb::InnerCallHandlers::buildMethodSelector(ctx, declaration.name(), *function);
					else
						signature = function->externalSignature(); // error selector
				}
				if (canonical)
				{
					auto const* external = function->asExternallyCallableFunction(false);
					if (!external) throw std::logic_error("Selector has no externally callable solc type");
					return awst::makeBytesConstant(SolcFacts::externalSelector(*external),
						loc, awst::BytesEncoding::Base16, resultType);
				}
				return awst::makeReinterpretCast(SelectorSemantics::functionSelector(
					ctx, *function, signature, loc), resultType, loc);
			}

			// Dynamic external pointers carry their public selector at bytes 8..12 in
			// both ABI profiles. Lower once, including any call-return write-backs.
			if (function->kind() != FunctionType::Kind::External)
				throw std::logic_error("Unresolved selector for a non-external function");
			if (canonical && !SelectorSemantics::enabled(ctx.typeMapper))
				Logger::instance().error(
					"abi.encodeCall with an opaque runtime external-function pointer requires --evm-selectors", loc);
			auto receiver = ctx.lower(source, false);
			auto value = ctx.emitSequencedOperand(
				std::move(receiver.effects), std::move(receiver.value), true, loc);
			return awst::makeReinterpretCast(awst::makeExtract(
				awst::makeAsBytes(std::move(value), loc), 8, 4, loc), resultType, loc);
		});
}

} // namespace puyasol::builder::sol_ast
