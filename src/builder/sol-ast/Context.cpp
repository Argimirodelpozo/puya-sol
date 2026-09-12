#include "builder/sol-ast/Context.h"
#include "builder/sol-eb/ContractContext.h"
#include "builder/sol-types/RefParamPassing.h"
#include "builder/sol-types/SolIntType.h"
#include <libsolidity/ast/AST.h>

namespace puyasol::builder::sol_ast
{

FunctionContext::FunctionContext(TranslationContext& _tr,
	solidity::frontend::FunctionDefinition const& function,
	std::vector<awst::SubroutineArgument> const& args,
	awst::WType const* _returnType)
	: FunctionContext(_tr, {}, _returnType, {})
{
	using solidity::frontend::VariableDeclaration;
	callableId = function.id();
	for (auto const& arg: args)
		params.emplace_back(arg.name, arg.wtype);
	auto& types = tr.typeMapper;
	auto const& plan = types.callBoundaryPlan(function, tr.contractCtx.currentContract);
	auto recordWidth = [&](VariableDeclaration const& declaration, std::string const& name) {
		if (auto integer = SolIntType::fromSol(declaration.type()); integer && integer->bits < 64)
			paramBitWidths[name] = integer->bits;
	};
	for (auto const& parameter: plan.parameters)
	{
		auto const& declaration = *parameter.declaration;
		paramSolTypes[parameter.name] = declaration.type();
		recordWidth(declaration, parameter.name);
		switch (parameter.passing)
		{
		case RefParamPassing::SlotHandle:
			scope.bindings.slotStorageRefs.set(declaration.id(), awst::makeVarExpression(
				parameter.name, parameter.type, tr.makeLoc(declaration.location())));
			break;
		case RefParamPassing::BoxKeyPrefix:
			scope.bindings.mappingKeyParams.set(declaration.id(), parameter.name);
			break;
		case RefParamPassing::BlobOffset:
			scope.bindings.blobAggregates.set(declaration.id(), parameter.name);
			break;
		case RefParamPassing::Value: break;
		}
	}
	for (auto pi: plan.offsetParams)
		scope.bindings.structRefOffsets.set(plan.parameters[pi].declaration->id(),
			plan.parameters[pi].offsetName());
	if (!types.profile().evmStorageLayout)
		for (auto pi: plan.asmSlotParams)
			boxKeyStructParams[plan.parameters[pi].name] =
				types.map(plan.parameters[pi].declaration->type());
	bool const slotReturns = types.profile().evmStorageLayout
		|| storageRefReturnUsesSlot(&function, types.analysis());
	for (auto const& result: function.returnParameters())
	{
		returnSolTypes.push_back(result->type());
		recordWidth(*result, result->name());
		if (result->name().empty()) continue;
		if (result->referenceLocation() == VariableDeclaration::Location::Storage)
		{
			if (slotReturns)
				scope.bindings.slotStorageRefs.set(result->id(), awst::makeVarExpression(
					result->name(), awst::WType::biguintType(), tr.makeLoc(result->location())));
			else if (types.isBoxKeyedStorageRef(result->type())
				|| types.analysis().asmSlotReferenceDeclarations.contains(result->id())
				|| storageRefReturnIsBytesKeyed(&function, types.analysis()))
				scope.bindings.mappingKeyParams.set(result->id(), result->name());
		}
		else if (result->referenceLocation() == VariableDeclaration::Location::Memory
			&& memoryUsesBlob(types.map(result->type())))
			scope.bindings.blobAggregates.set(result->id(), "__blobagg_off_" + std::to_string(result->id()));
	}
}

bool Context::isInConstructor() const
{
	return function && function->inConstructor;
}

std::set<std::string>* Context::liveCalldataPointers() const
{
	return function ? &function->seededCalldataPointers : nullptr;
}

int64_t Context::callableId() const
{
	return function ? function->callableId : 0;
}

std::string Context::awstVarName(solidity::frontend::VariableDeclaration const& _vd) const
{
	// Modifier-lowering remap wins (same modifier applied twice → unique per-instance
	// local names, keyed by decl id).
	if (auto const* remap = bindings.paramRemaps.find(_vd.id()))
		return remap->name;

	// Params/returns keep their bare name (unique in the fn, ABI-facing); locals and
	// catch params mangle to name__<declId> so shadows can't collide in the flat AWST
	// frame. Pure function of the decl — solc ids are globally unique.
	if (_vd.isCallableOrCatchParameter() && !_vd.isTryCatchParameter())
		return _vd.name();
	return _vd.name() + "__" + std::to_string(_vd.id());
}

} // namespace puyasol::builder::sol_ast
