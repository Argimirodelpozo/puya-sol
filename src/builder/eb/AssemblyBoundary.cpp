#include "builder/eb/AssemblyBoundary.h"
#include "builder/eb/CalldataReference.h"
#include "builder/codec/EvmValueCodec.h"
#include "builder/context/ContractContext.h"
#include "builder/context/ProgramAnalysis.h"
#include "builder/solc/PreparedAssembly.h"
#include "builder/solc/SolcFacts.h"
#include "builder/types/TypeCoercion.h"
#include "builder/yul/AssemblyBuilder.h"
#include "builder/contract/SelectorRouter.h"

#include <algorithm>

#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/ASTVisitor.h>

namespace puyasol::builder
{
using namespace solidity::frontend;

namespace
{
awst::WTuple const rawWordType({awst::WType::biguintType()}, std::vector<std::string>{"__yul_word"});
}

bool isAssemblyScalarCopy(awst::WType const* type) { return type == &rawWordType; }

std::shared_ptr<awst::Expression> assemblyScalarCopy(eb::ContractContext& ctx,
	VariableDeclaration const& destination, Expression const& source, awst::SourceLocation const& loc)
{
	if (!ctx.scope().bindings.assemblyWords.find(destination.id())
		|| *source.annotation().type != *destination.type()) return nullptr;
	std::function<std::shared_ptr<awst::Expression>(Expression const&)> lower;
	lower = [&](Expression const& expression) -> std::shared_ptr<awst::Expression> {
		if (auto const* id = SolcFacts::expressionAs<Identifier>(&expression))
			if (auto const* declaration = dynamic_cast<VariableDeclaration const*>(id->annotation().referencedDeclaration))
				if (auto name = ctx.scope().bindings.assemblyWords.get(declaration->id()); !name.empty())
					return awst::makeVarExpression(name, awst::WType::biguintType(), loc);
		if (auto const* conditional = SolcFacts::expressionAs<Conditional>(&expression))
		{
			auto condition = ctx.pinIfWriteBacks(ctx.lower(conditional->condition(), false), loc);
			auto yes = ctx.lowerOperand([&] { return lower(conditional->trueExpression()); });
			auto no = ctx.lowerOperand([&] { return lower(conditional->falseExpression()); });
			return ctx.emitConditional(std::move(condition), std::move(yes), std::move(no), awst::WType::biguintType(), loc);
		}
		if (auto const* assignment = SolcFacts::expressionAs<Assignment>(&expression);
			assignment && assignment->assignmentOperator() == Token::Assign)
			if (auto const* id = SolcFacts::expressionAs<Identifier>(&assignment->leftHandSide()))
				if (auto const* declaration = dynamic_cast<VariableDeclaration const*>(id->annotation().referencedDeclaration))
					if (auto name = ctx.scope().bindings.assemblyWords.get(declaration->id()); !name.empty())
					{
						auto value = ctx.emitSequencedOperand({}, lower(assignment->rightHandSide()), true, loc);
						ctx.queuePreEffect(awst::makeAssignmentStatement(
							awst::makeVarExpression(name, awst::WType::biguintType(), loc), value, loc));
						return value;
					}
		return awst::makeAsBiguint(codec::valueToEvmWord(ctx.typeMapper, destination.type(), ctx.buildExpr(expression), loc), loc);
	};
	auto tuple = awst::makeTupleExpression(&rawWordType, loc);
	tuple->items.push_back(lower(source));
	return tuple;
}

std::shared_ptr<awst::Expression> readAssemblyScalar(
	sol_ast::Context const& scope, TypeMapper& mapper, VariableDeclaration const& declaration,
	awst::SourceLocation const& loc, std::vector<std::shared_ptr<awst::Statement>>& out)
{
	auto name = scope.bindings.assemblyWords.get(declaration.id());
	if (name.empty()) return nullptr;
	return codec::valueFromEvmWord(mapper, declaration.type(),
		awst::makeLeftPadToN(awst::makeAsBytes(
			awst::makeVarExpression(name, awst::WType::biguintType(), loc), loc), 32, loc),
		loc, out, codec::PaddingPolicy::Clean);
}

std::shared_ptr<awst::Statement> writeAssemblyScalar(
	sol_ast::Context const& scope, TypeMapper& mapper, VariableDeclaration const& declaration,
	std::shared_ptr<awst::Expression> value, awst::SourceLocation const& loc)
{
	auto name = scope.bindings.assemblyWords.get(declaration.id());
	if (name.empty()) return nullptr;
	if (isAssemblyScalarCopy(value->wtype))
		value = awst::makeTupleItem(std::move(value), 0, awst::WType::biguintType(), loc);
	else value = awst::makeAsBiguint(codec::valueToEvmWord(mapper, declaration.type(), std::move(value), loc), loc);
	return awst::makeAssignmentStatement(awst::makeVarExpression(name, awst::WType::biguintType(), loc), value, loc);
}

void prepareAssemblyBoundary(sol_ast::FunctionContext& function, Block const& body,
	std::vector<std::shared_ptr<awst::Statement>>& prelude)
{
	function.hasAssemblyCalldata |= function.tr.typeMapper.analysis().callablesWithCalldata.contains(function.callableId);
	struct Scanner: ASTConstVisitor
	{
		sol_ast::FunctionContext& fn;
		std::map<int64_t, VariableDeclaration const*> parameters;
		std::vector<std::pair<VariableDeclaration const*, VariableDeclaration const*>> copies;
		explicit Scanner(sol_ast::FunctionContext& function): fn(function) {}
		void copy(VariableDeclaration const* destination, Expression const* source)
		{
			if (!destination || !destination->isLocalVariable() || !source
				|| *destination->type() != *source->annotation().type) return;
			if (auto const* id = SolcFacts::expressionAs<Identifier>(source))
				if (auto const* declaration = dynamic_cast<VariableDeclaration const*>(id->annotation().referencedDeclaration);
					declaration && declaration->isLocalVariable()) copies.emplace_back(destination, declaration);
			if (auto const* conditional = SolcFacts::expressionAs<Conditional>(source))
			{
				copy(destination, &conditional->trueExpression());
				copy(destination, &conditional->falseExpression());
			}
			if (auto const* assignment = SolcFacts::expressionAs<Assignment>(source))
			{
				copy(destination, &assignment->leftHandSide());
				copy(destination, &assignment->rightHandSide());
			}
		}
		bool visit(VariableDeclarationStatement const& statement) override
		{
			auto const* tuple = SolcFacts::expressionAs<TupleExpression>(statement.initialValue());
			for (size_t i = 0; i < statement.declarations().size(); ++i)
				copy(statement.declarations()[i].get(), tuple && statement.declarations().size() > 1
					? tuple->components()[i].get() : statement.initialValue());
			return true;
		}
		bool visit(Assignment const& assignment) override
		{
			std::function<void(Expression const&, Expression const&)> bind = [&](auto const& lhs, auto const& rhs) {
				if (auto const* id = SolcFacts::expressionAs<Identifier>(&lhs))
					copy(dynamic_cast<VariableDeclaration const*>(id->annotation().referencedDeclaration), &rhs);
				else if (auto const* left = SolcFacts::expressionAs<TupleExpression>(&lhs))
					if (auto const* right = SolcFacts::expressionAs<TupleExpression>(&rhs))
						for (size_t i = 0; i < left->components().size(); ++i)
							if (left->components()[i] && right->components()[i]) bind(*left->components()[i], *right->components()[i]);
			};
			bind(assignment.leftHandSide(), assignment.rightHandSide());
			return true;
		}
		bool visit(VariableDeclaration const& declaration) override
		{
			if (declaration.referenceLocation() == VariableDeclaration::Location::CallData)
				fn.calldataDeclarations.insert(declaration.id());
			return true;
		}
		bool visit(InlineAssembly const& assembly) override
		{
			fn.hasAssemblyCalldata |= fn.tr.typeMapper.analysis().preparedAssemblies.at(assembly.id())->facts.usesCalldata;
			for (auto const& [identifier, info]: assembly.annotation().externalReferences)
			{
				auto const* declaration = dynamic_cast<VariableDeclaration const*>(info.declaration);
				if (declaration && declaration->referenceLocation() == VariableDeclaration::Location::CallData)
				{
					fn.hasAssemblyCalldata = true;
					fn.calldataDeclarations.insert(declaration->id());
				}
				if (!declaration || declaration->isStateVariable() || declaration->isConstant()
					|| !info.suffix.empty()) continue;
				auto const* type = codec::underlyingType(declaration->type());
				// Full-width integers already retain raw words. Addresses and function
				// pointers have their own target-specific transport conventions.
				if (auto const* integer = dynamic_cast<IntegerType const*>(type);
					integer && integer->numBits() == 256) continue;
				if (auto const* bytes = dynamic_cast<FixedBytesType const*>(type);
					bytes && bytes->numBytes() == 32) continue;
				if (!dynamic_cast<IntegerType const*>(type) && !dynamic_cast<BoolType const*>(type)
					&& !dynamic_cast<FixedBytesType const*>(type) && !dynamic_cast<EnumType const*>(type)) continue;
				fn.bindings.assemblyWords.set(declaration->id(), "__asmword_" + fn.scope.awstVarName(*declaration));
				if (declaration->isCallableOrCatchParameter() && !declaration->isTryCatchParameter())
					parameters.emplace(declaration->id(), declaration);
			}
			return false;
		}
	} scanner(function);
	body.accept(scanner);
	// Propagate only along same-type local copies connected to an actual Yul
	// external reference; unrelated narrow locals keep their native carriers.
	bool changed;
	do
	{
		changed = false;
		for (auto const& [left, right]: scanner.copies)
			if (function.bindings.assemblyWords.find(left->id()) || function.bindings.assemblyWords.find(right->id()))
				for (auto const* declaration: {left, right})
					if (!function.bindings.assemblyWords.find(declaration->id()))
					{
						function.bindings.assemblyWords.set(declaration->id(), "__asmword_" + function.scope.awstVarName(*declaration));
						if (declaration->isCallableOrCatchParameter() && !declaration->isTryCatchParameter())
							scanner.parameters.emplace(declaration->id(), declaration);
						changed = true;
					}
	} while (changed);
	if (function.hasAssemblyCalldata)
	{
		AssemblyBuilder builder(function.tr.typeMapper, function.tr.sourceFile, "calldata_entry", function.inConstructor);
		auto types = function.parameterSolTypes();
		if (function.sourceFunction)
			for (auto const& declaration: function.sourceFunction->parameters())
				if (declaration->type()->dataStoredIn(DataLocation::CallData))
					function.calldataDeclarations.insert(declaration->id());
		builder.setCalldataSolTypes(std::move(types));
		if (SelectorSemantics::enabled(function.tr.typeMapper))
			builder.setSelectorRoutes(SelectorSemantics::routes(function.tr.contractCtx));
		bool const incoming = std::any_of(function.params.begin(), function.params.end(),
			[](auto const& parameter) { return parameter.first == "__cd_blob"; });
		auto const* source = function.sourceFunction;
		if (function.inConstructor || (source && (source->isFallback() || source->isReceive())))
		{
			auto loc = function.tr.makeLoc(body.location());
			auto input = awst::makeVarExpression("__cd_blob", awst::WType::bytesType(), loc);
			std::shared_ptr<awst::Expression> data = function.inConstructor
				? std::shared_ptr<awst::Expression>(awst::makeBytesConstant({}, loc))
				: reconstructCalldata(CalldataTransport::Arc4Fallback, loc);
			prelude.push_back(awst::makeAssignmentStatement(input, std::move(data), loc));
			if (source && source->isFallback() && !source->parameters().empty())
			{
				auto const name = function.scope.awstVarName(*source->parameters().front());
				prelude.push_back(awst::makeAssignmentStatement(awst::makeVarExpression(
					"__cd_off_" + name, awst::WType::biguintType(), loc), awst::makeBiguintConstant("0", loc), loc));
				prelude.push_back(awst::makeAssignmentStatement(awst::makeVarExpression(
					"__cd_len_" + name, awst::WType::biguintType(), loc),
					TypeCoercion::coerceScalar(awst::makeLen(input, loc), awst::WType::biguintType(), loc), loc));
			}
		}
		else if (!incoming)
			builder.prepareCalldata(function.params, prelude, function.tr.makeLoc(body.location()),
				function.sourceFunction && function.sourceFunction->isPartOfExternalInterface());
	}
	for (auto const& [id, declaration]: scanner.parameters)
	{
		auto loc = function.tr.makeLoc(declaration->location());
		prelude.push_back(writeAssemblyScalar(function.scope, function.tr.typeMapper, *declaration,
			awst::makeVarExpression(function.scope.awstVarName(*declaration),
				function.tr.typeMapper.map(declaration->type()), loc), loc));
	}
}

void appendCalldataParameters(CallBoundaryPlan const& plan,
	std::vector<awst::SubroutineArgument>& args, awst::SourceLocation const& loc)
{
	if (!plan.calldataFrame) return;
	args.emplace_back("__cd_blob", awst::WType::bytesType(), loc);
	for (auto const& parameter: plan.parameters)
		if (parameter.declaration->type()->dataStoredIn(DataLocation::CallData))
		{
			args.emplace_back("__cd_off_" + parameter.name, awst::WType::biguintType(), loc);
			if (sol_ast::CalldataReference::hasLength(parameter.declaration->type()))
				args.emplace_back("__cd_len_" + parameter.name, awst::WType::biguintType(), loc);
		}
}

} // namespace puyasol::builder
