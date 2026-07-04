#include "builder/contract/ContractBuilder.h"
#include "awst/Termination.h"
#include "builder/AWSTBuilder.h"
#include "builder/NatSpecTags.h"
#include "builder/assembly/AssemblyBuilder.h"
#include "builder/contract/ParamABIValidator.h"
#include "builder/contract/ReturnRewriter.h"
#include "builder/sol-ast/stmts/SolBlock.h"
#include "builder/sol-ast/StorageRefPointer.h"
#include "builder/sol-ast/ParamMutationDetector.h"
#include "builder/itxn/CallResolver.h"
#include "builder/sol-types/OverloadSuffix.h"
#include "builder/sol-types/SolIntType.h"
#include "builder/sol-types/Arc4Defaults.h"
#include "builder/sol-types/TypeCoercion.h"
#include "Logger.h"

#include <libsolidity/ast/ASTVisitor.h>

namespace puyasol::builder
{

using awst::blockAlwaysTerminates;

namespace {

/// Walk all SubroutineCallExpression nodes in a body tree; used to
/// retarget self-recursive callsubs after biguint→ARC4 param remapping.
class SubroutineCallVisitor
{
public:
	using Callback = std::function<void(awst::SubroutineCallExpression&)>;

	explicit SubroutineCallVisitor(Callback _cb): m_cb(std::move(_cb)) {}

	void visitStmt(awst::Statement& _s)
	{
		if (auto* b = dynamic_cast<awst::Block*>(&_s))
			for (auto& sub: b->body) if (sub) visitStmt(*sub);
		else if (auto* es = dynamic_cast<awst::ExpressionStatement*>(&_s))
			{ if (es->expr) visitExpr(*es->expr); }
		else if (auto* rs = dynamic_cast<awst::ReturnStatement*>(&_s))
			{ if (rs->value) visitExpr(*rs->value); }
		else if (auto* ie = dynamic_cast<awst::IfElse*>(&_s))
		{
			if (ie->condition) visitExpr(*ie->condition);
			if (ie->ifBranch) visitStmt(*ie->ifBranch);
			if (ie->elseBranch) visitStmt(*ie->elseBranch);
		}
		else if (auto* wl = dynamic_cast<awst::WhileLoop*>(&_s))
		{
			if (wl->condition) visitExpr(*wl->condition);
			if (wl->loopBody) visitStmt(*wl->loopBody);
		}
		else if (auto* as = dynamic_cast<awst::AssignmentStatement*>(&_s))
		{
			if (as->target) visitExpr(*as->target);
			if (as->value) visitExpr(*as->value);
		}
		else if (auto* sw = dynamic_cast<awst::Switch*>(&_s))
		{
			if (sw->value) visitExpr(*sw->value);
			for (auto& c: sw->cases)
			{
				if (c.first) visitExpr(*c.first);
				if (c.second) visitStmt(*c.second);
			}
			if (sw->defaultCase) visitStmt(*sw->defaultCase);
		}
		else if (auto* fl = dynamic_cast<awst::ForInLoop*>(&_s))
		{
			if (fl->sequence) visitExpr(*fl->sequence);
			if (fl->items) visitExpr(*fl->items);
			if (fl->loopBody) visitStmt(*fl->loopBody);
		}
		else if (auto* aa = dynamic_cast<awst::UInt64AugmentedAssignment*>(&_s))
		{
			if (aa->target) visitExpr(*aa->target);
			if (aa->value) visitExpr(*aa->value);
		}
		else if (auto* aa = dynamic_cast<awst::BigUIntAugmentedAssignment*>(&_s))
		{
			if (aa->target) visitExpr(*aa->target);
			if (aa->value) visitExpr(*aa->value);
		}
		// Goto, LoopExit, LoopContinue have no children.
	}

	void visitExpr(awst::Expression& _e)
	{
		if (auto* call = dynamic_cast<awst::SubroutineCallExpression*>(&_e))
		{
			m_cb(*call);
			for (auto& a: call->args) if (a.value) visitExpr(*a.value);
			return;
		}
		if (auto* op = dynamic_cast<awst::UInt64BinaryOperation*>(&_e))
			{ if (op->left) visitExpr(*op->left); if (op->right) visitExpr(*op->right); }
		else if (auto* op = dynamic_cast<awst::BigUIntBinaryOperation*>(&_e))
			{ if (op->left) visitExpr(*op->left); if (op->right) visitExpr(*op->right); }
		else if (auto* op = dynamic_cast<awst::BytesBinaryOperation*>(&_e))
			{ if (op->left) visitExpr(*op->left); if (op->right) visitExpr(*op->right); }
		else if (auto* op = dynamic_cast<awst::BooleanBinaryOperation*>(&_e))
			{ if (op->left) visitExpr(*op->left); if (op->right) visitExpr(*op->right); }
		else if (auto* op = dynamic_cast<awst::BytesUnaryOperation*>(&_e))
			{ if (op->expr) visitExpr(*op->expr); }
		else if (auto* op = dynamic_cast<awst::Not*>(&_e))
			{ if (op->expr) visitExpr(*op->expr); }
		else if (auto* cmp = dynamic_cast<awst::NumericComparisonExpression*>(&_e))
			{ if (cmp->lhs) visitExpr(*cmp->lhs); if (cmp->rhs) visitExpr(*cmp->rhs); }
		else if (auto* cmp = dynamic_cast<awst::BytesComparisonExpression*>(&_e))
			{ if (cmp->lhs) visitExpr(*cmp->lhs); if (cmp->rhs) visitExpr(*cmp->rhs); }
		else if (auto* a = dynamic_cast<awst::AssertExpression*>(&_e))
			{ if (a->condition) visitExpr(*a->condition); }
		else if (auto* a = dynamic_cast<awst::AssignmentExpression*>(&_e))
			{ if (a->target) visitExpr(*a->target); if (a->value) visitExpr(*a->value); }
		else if (auto* c = dynamic_cast<awst::ConditionalExpression*>(&_e))
		{
			if (c->condition) visitExpr(*c->condition);
			if (c->trueExpr) visitExpr(*c->trueExpr);
			if (c->falseExpr) visitExpr(*c->falseExpr);
		}
		else if (auto* ic = dynamic_cast<awst::IntrinsicCall*>(&_e))
			for (auto& a: ic->stackArgs) if (a) visitExpr(*a);
		else if (auto* enc = dynamic_cast<awst::ARC4Encode*>(&_e))
			{ if (enc->value) visitExpr(*enc->value); }
		else if (auto* dec = dynamic_cast<awst::ARC4Decode*>(&_e))
			{ if (dec->value) visitExpr(*dec->value); }
		else if (auto* tup = dynamic_cast<awst::TupleExpression*>(&_e))
			for (auto& it: tup->items) if (it) visitExpr(*it);
		else if (auto* ti = dynamic_cast<awst::TupleItemExpression*>(&_e))
			{ if (ti->base) visitExpr(*ti->base); }
		else if (auto* fe = dynamic_cast<awst::FieldExpression*>(&_e))
			{ if (fe->base) visitExpr(*fe->base); }
		else if (auto* ix = dynamic_cast<awst::IndexExpression*>(&_e))
			{ if (ix->base) visitExpr(*ix->base); if (ix->index) visitExpr(*ix->index); }
		else if (auto* ca = dynamic_cast<awst::CommaExpression*>(&_e))
			for (auto& it: ca->expressions) if (it) visitExpr(*it);
		// Other expressions (constants, var refs, type-info nodes, etc.)
		// have no children we care about for recursive-call rewriting.
	}

private:
	Callback m_cb;
};

} // namespace

awst::ContractMethod ContractBuilder::buildClearProgram(
	solidity::frontend::ContractDefinition const& _contract,
	std::string const& _contractName
)
{
	awst::ContractMethod method;
	method.sourceLocation = makeLoc(_contract.location());
	method.returnType = awst::WType::boolType();
	method.cref = m_sourceFile + "." + _contractName;
	method.memberName = "clear_state_program";

	auto body = awst::makeBlock(method.sourceLocation);

	// return true
	auto ret = awst::makeReturnStatement(awst::makeTrue(method.sourceLocation), method.sourceLocation);

	body->body.push_back(ret);
	method.body = body;

	return method;
}

namespace {
// Handle-model copy+write-back for MEMORY-ref params of internal contract methods. Solidity passes
// memory by reference (callee mutations propagate to the caller); our value-translation copies, so
// a method that mutates a memory STRUCT param would lose it. (Arrays already write through via puya
// ReferenceArray; libraries/free fns already augment in buildFreestandingSubroutine — this brings
// contract methods in line.) Each mutated memory-ref param is appended to the method's return; the
// internal caller (SolInternalCall) writes it back. Storage refs use the box-key/offset handle, not
// this. Mirrors the freestanding logic + the caller's memoryRefParamIndices filter exactly.
void augmentMethodForMutatedMemoryParams(
	awst::ContractMethod& method,
	solidity::frontend::FunctionDefinition const& func,
	TypeMapper& /*tm*/)
{
	using namespace solidity::frontend;
	if (!func.isImplemented() || !method.body) return;
	// Internal only: Public/External are ABI entry points (augmenting their return breaks the
	// selector's return ABI); Private is threaded by puya. Internal methods are pure callsub
	// targets — the analogue of library/free fns, which buildFreestandingSubroutine augments.
	if (func.visibility() != Visibility::Internal) return;

	auto isMemRefType = [](Type const* t) {
		if (auto const* arr = dynamic_cast<ArrayType const*>(t)) return !arr->isByteArrayOrString();
		return dynamic_cast<StructType const*>(t) != nullptr;
	};
	ParamMutationDetector detector;
	for (auto const& p : func.parameters()) detector.paramIds.insert(p->id());
	func.body().accept(detector);

	std::vector<size_t> memIdx;
	for (size_t pi = 0; pi < func.parameters().size() && pi < method.args.size(); ++pi)
	{
		auto const& p = func.parameters()[pi];
		if (p->referenceLocation() != VariableDeclaration::Location::Memory) continue;
		if (!p->type() || !isMemRefType(p->type())) continue;
		if (!detector.mutated.count(p->id())) continue;
		memIdx.push_back(pi);
	}
	if (memIdx.empty()) return;

	auto const& loc = method.sourceLocation;

	// Augment the return type: original return value(s) (flattened), then each mem-param type.
	std::vector<awst::WType const*> types;
	bool origVoid = (method.returnType == awst::WType::voidType());
	auto const* origTuple = origVoid ? nullptr
		: dynamic_cast<awst::WTuple const*>(method.returnType);
	if (!origVoid)
	{
		if (origTuple) for (auto const* t : origTuple->types()) types.push_back(t);
		else types.push_back(method.returnType);
	}
	for (size_t idx : memIdx) types.push_back(method.args[idx].wtype);
	awst::WType const* newRetType =
		types.size() == 1 ? types[0] : new awst::WTuple(std::move(types));
	method.returnType = newRetType;
	bool newIsTuple = (dynamic_cast<awst::WTuple const*>(newRetType) != nullptr);

	// Walk existing returns; append the mem-param vars to match the new shape.
	std::function<void(awst::Block&)> walk = [&](awst::Block& block) {
		for (auto& stmt : block.body)
		{
			if (auto* ret = dynamic_cast<awst::ReturnStatement*>(stmt.get()))
			{
				if (!newIsTuple)
				{
					if (!ret->value && memIdx.size() == 1)
						ret->value = awst::makeVarExpression(
							method.args[memIdx[0]].name, method.args[memIdx[0]].wtype,
							ret->sourceLocation);
				}
				else
				{
					auto tuple = awst::makeTupleExpression(newRetType, ret->sourceLocation);
					if (ret->value)
					{
						if (auto* ot = dynamic_cast<awst::TupleExpression*>(ret->value.get()))
							for (auto& it : ot->items) tuple->items.push_back(it);
						else tuple->items.push_back(ret->value);
					}
					for (size_t idx : memIdx)
						tuple->items.push_back(awst::makeVarExpression(
							method.args[idx].name, method.args[idx].wtype, ret->sourceLocation));
					ret->value = std::move(tuple);
				}
			}
			if (auto* ie = dynamic_cast<awst::IfElse*>(stmt.get()))
			{
				if (ie->ifBranch) walk(*ie->ifBranch);
				if (ie->elseBranch) walk(*ie->elseBranch);
			}
		}
	};
	walk(*method.body);

	// Fall-through: only void methods reach here un-terminated (buildFunction already synthesised
	// a return for non-void fall-through, which the walk above augmented). Return the mem param(s).
	if (!awst::blockAlwaysTerminates(*method.body))
	{
		auto implicit = awst::makeReturnStatement(nullptr, loc);
		if (!newIsTuple && memIdx.size() == 1)
			implicit->value = awst::makeVarExpression(
				method.args[memIdx[0]].name, method.args[memIdx[0]].wtype, loc);
		else
		{
			auto tuple = awst::makeTupleExpression(newRetType, loc);
			for (size_t idx : memIdx)
				tuple->items.push_back(awst::makeVarExpression(
					method.args[idx].name, method.args[idx].wtype, loc));
			implicit->value = std::move(tuple);
		}
		method.body->body.push_back(std::move(implicit));
	}
}
} // namespace

awst::ContractMethod ContractBuilder::buildFunction(
	solidity::frontend::FunctionDefinition const& _func,
	std::string const& _contractName,
	std::string const& _nameOverride
)
{
	awst::ContractMethod method;
	method.sourceLocation = makeLoc(_func.location());
	method.cref = m_sourceFile + "." + _contractName;
	if (!_nameOverride.empty())
	{
		method.memberName = _nameOverride;
	}
	else
	{
		method.memberName = _func.name();
		if (m_overloadedNames.count(_func.name()))
			appendOverloadSuffix(method.memberName, _func);
	}

	// Documentation
	if (_func.documentation())
		method.documentation.description = *_func.documentation()->text();

	// Parameters
	int paramIndex = 0;
	for (auto const& param: _func.parameters())
	{
		awst::SubroutineArgument arg;
		if (param->name().empty())
			arg.name = "_param" + std::to_string(paramIndex);
		else
			arg.name = param->name();
		arg.sourceLocation = makeLoc(param->location());
		arg.wtype = m_typeMapper.map(param->type());
		// Memory aggregate >4KB: pass as uint64 base offset (blob pointer model).
		// Callee re-registers via setBlobAggParams so p.field[i] hits blob word access.
		if (param->referenceLocation() == solidity::frontend::VariableDeclaration::Location::Memory
			&& memoryUsesBlob(arg.wtype))
			arg.wtype = awst::WType::uint64Type();
		method.args.push_back(std::move(arg));
		paramIndex++;
	}

	// Handle-model dual handle: offset-convention struct-ref params (those that receive an
	// array-element ref `f(arr[i])` somewhere) get a companion uint64 OFFSET param, appended
	// after all regular params. The caller appends matching offset args in the same order; the
	// body's `s.field` writes target the element slice via box_replace(key, offset+fieldOff).
	for (auto const& param: _func.parameters())
		if (param->referenceLocation() == solidity::frontend::VariableDeclaration::Location::Storage
			&& !param->name().empty()
			&& structRefOffsetParamsRegistry().count(param->id()))
		{
			awst::SubroutineArgument offArg;
			offArg.name = param->name() + "__off";
			offArg.sourceLocation = makeLoc(param->location());
			offArg.wtype = awst::WType::uint64Type();
			method.args.push_back(std::move(offArg));
		}

	// Return type
	auto const& returnParams = _func.returnParameters();
	std::vector<SignedReturnInfo> signedReturns;
	std::vector<UnsignedMaskInfo> unsignedMasks;

	if (returnParams.empty())
		method.returnType = awst::WType::voidType();
	else if (returnParams.size() == 1)
	{
		method.returnType = m_typeMapper.map(returnParams[0]->type());
		// .slot assembly storage ref: return biguint (slot number).
		if (returnParams[0]->referenceLocation() == solidity::frontend::VariableDeclaration::Location::Storage
			&& _func.isImplemented()
			&& std::any_of(_func.body().statements().begin(), _func.body().statements().end(),
				[](auto const& s) { return dynamic_cast<solidity::frontend::InlineAssembly const*>(s.get()); }))
			method.returnType = awst::WType::biguintType();
		// Storage-ref pointer (`return _pools[id]`): return uint64 index or bytes box-key.
		// Box-keyed when the holder is a mapping (storageRefReturnIsBytesKeyed),
		// even for plain-struct elements with no nested mappings (V4 Position.State).
		else if (storageRefPointerReturn(&_func))
			method.returnType = storageRefReturnIsBytesKeyed(&_func)
				? awst::WType::bytesType()
				: awst::WType::uint64Type();
		// Signed ≤64-bit returns → biguint for proper 256-bit two's complement ARC4 encoding.
		auto intInfo = builder::SolIntType::fromSolOrEnum(returnParams[0]->type());
		// Biguint promotion only at ABI boundary; private/internal keep uint64
		// so `return IntegerConstant(uint64,…)` matches declared type.
		bool isAbiBoundary = _func.isPartOfExternalInterface();
		if (intInfo && intInfo->isSigned)
		{
			if (intInfo->bits <= 64 && isAbiBoundary)
				method.returnType = awst::WType::biguintType();
			if (isAbiBoundary)
				signedReturns.push_back({intInfo->bits, 0});
		}
		else if (intInfo && !intInfo->isSigned && intInfo->bits < 64)
		{
			if (isAbiBoundary)
				unsignedMasks.push_back({intInfo->bits, 0});
		}
	}
	else
	{
		// Multiple returns → tuple
		std::vector<awst::WType const*> types;
		std::vector<std::string> names;
		bool hasNames = false;
		for (size_t ri = 0; ri < returnParams.size(); ++ri)
		{
			auto const& rp = returnParams[ri];
			auto* mappedType = m_typeMapper.map(rp->type());
			auto intInfo = builder::SolIntType::fromSolOrEnum(rp->type());
			bool isAbiBoundary = _func.isPartOfExternalInterface();
			if (intInfo)
			{
				if (intInfo->isSigned)
				{
					if (intInfo->bits <= 64 && isAbiBoundary)
						mappedType = awst::WType::biguintType();
					if (isAbiBoundary)
						signedReturns.push_back({intInfo->bits, ri});
				}
				else if (!intInfo->isSigned && intInfo->bits < 64)
				{
					if (isAbiBoundary)
						unsignedMasks.push_back({intInfo->bits, ri});
				}
			}
			types.push_back(mappedType);
			names.push_back(rp->name());
			if (!rp->name().empty())
				hasNames = true;
		}
		if (hasNames)
		{
			// Suffix "Return" to avoid ARC56 collision across methods.
			std::string tupleName = _func.name() + "Return";
			method.returnType = new awst::WTuple(std::move(types), std::move(names), std::move(tupleName));
		}
		else
			method.returnType = new awst::WTuple(std::move(types));
	}

	// Pure/view
	method.pure = _func.stateMutability() == solidity::frontend::StateMutability::Pure;

	// ARC4 method config for public/external functions
	method.arc4MethodConfig = buildARC4Config(_func, method.sourceLocation);

	// uros: chunk-assigned methods must not be inlined (an inlined copy defeats
	// the split; the uros backend needs to stub it in non-owning chunks).
	if (method.arc4MethodConfig.has_value())
		if (auto* abiCfg = std::get_if<awst::ARC4ABIMethodConfig>(&*method.arc4MethodConfig))
			if (!abiCfg->chunk.empty())
				method.inlineOpt = false;

	// ARC4 methods: remap param types to ARC4; stash decode ops for deferred insertion.
	struct ParamDecode
	{
		std::string name;
		awst::WType const* nativeType;
		awst::WType const* arc4Type;
		awst::SourceLocation loc;
		unsigned maskBits = 0; // >0 for sub-64-bit unsigned types needing input masking
		unsigned signedBits = 0; // >0 for signed 64<N<256 int params: sign-extend to 256-bit after decode
	};
	std::vector<ParamDecode> paramDecodes;
	// Detect inline assembly: skip ARC4 param wrapping (would break asm var refs).
	bool funcHasInlineAssembly = false;
	if (_func.isImplemented())
	{
		for (auto const& stmt: _func.body().statements())
			if (dynamic_cast<solidity::frontend::InlineAssembly const*>(stmt.get()))
			{ funcHasInlineAssembly = true; break; }
	}

	// Self-recursive callsubs are rewritten post-translation to wrap biguint args
	// in ARC4Encode (see wrap pass below) — self-recursion no longer gates the remap.
	if (method.arc4MethodConfig.has_value())
	{
		auto const& solParams = _func.parameters();
		for (size_t pi = 0; pi < method.args.size(); ++pi)
		{
			auto& arg = method.args[pi];

			// Remap biguint → ARC4UIntN(N): without this puya uses uint512 (AVM max),
			// breaking ABI selectors. Skipped for asm bodies (would break Yul refs).
			if (arg.wtype == awst::WType::biguintType() && pi < solParams.size())
			{
				auto intInfo = builder::SolIntType::fromSol(solParams[pi]->annotation().type);
				unsigned bits = intInfo ? intInfo->bits : 256;
				auto const* arc4Type = m_typeMapper.createType<awst::ARC4UIntN>(static_cast<int>(bits));
				// Signed sub-256 (64<N<256) decodes to N-bit two's complement; sign-extend
				// to 256-bit so downstream ops (compare, negate) see the correct sign.
				// int256 is already canonical; ≤64-bit is uint64-backed (buildABIEntryChecks).
				unsigned signedBits =
					(intInfo && intInfo->isSigned && bits > 64 && bits < 256) ? bits : 0;
				paramDecodes.push_back({arg.name, arg.wtype, arc4Type, arg.sourceLocation, 0, signedBits});
				// Asm bodies are built (buildBlock) AFTER this loop; defer the ABI wtype change so the Yul
				// body builds against the native biguint type (set in the decode rename loop below).
				if (!funcHasInlineAssembly)
					arg.wtype = arc4Type;
				continue;
			}

			// Remap aggregate types and external fn-ptr bytes[12] to ARC4.
			// General bytes/bytes[N] params are NOT remapped.
			bool isAggregate = arg.wtype
				&& (arg.wtype->kind() == awst::WTypeKind::ReferenceArray
					|| arg.wtype->kind() == awst::WTypeKind::ARC4StaticArray
					|| arg.wtype->kind() == awst::WTypeKind::ARC4DynamicArray
					|| arg.wtype->kind() == awst::WTypeKind::WTuple);
			if (!isAggregate && pi < solParams.size()) // external fn-ptr bytes[12]
			{
				if (dynamic_cast<solidity::frontend::FunctionType const*>(solParams[pi]->type())
					&& arg.wtype && arg.wtype->kind() == awst::WTypeKind::Bytes)
					isAggregate = true;
			}
			// Skip remap for asm bodies: decode is also suppressed there, so remapping
			// without a decode would leave the body reading ARC4 where it expects native.
			if (!isAggregate || funcHasInlineAssembly)
				continue;

			awst::WType const* arc4Type = m_typeMapper.mapToARC4Type(arg.wtype);
			if (arc4Type != arg.wtype)
			{
				paramDecodes.push_back({arg.name, arg.wtype, arc4Type, arg.sourceLocation});
				arg.wtype = arc4Type;
			}
		}
	}

	if (_func.isImplemented())
	{
		// Use ARC4-remapped types from method.args for the assembly translation context.
		{
			std::vector<std::pair<std::string, awst::WType const*>> paramContext;
			std::map<std::string, unsigned> bitWidths;
			for (auto const& arg: method.args)
				paramContext.emplace_back(arg.name, arg.wtype);
			// Collect sub-64-bit widths from function params and return params
			for (auto const& p: _func.parameters())
			{
				if (auto it = builder::SolIntType::fromSol(p->annotation().type);
					it && it->bits < 64)
					bitWidths[p->name()] = it->bits;
			}
			for (auto const& rp: _func.returnParameters())
			{
				if (auto it = builder::SolIntType::fromSol(rp->annotation().type);
					it && it->bits < 64)
					bitWidths[rp->name()] = it->bits;
			}
			setFunctionContext(paramContext, method.returnType, bitWidths);
		}

		// Stash named-return decls for buildBlock (registers >4KB memory returns as blob-backed).
		std::vector<solidity::frontend::VariableDeclaration const*> namedReturnDecls;
		for (auto const& rp: returnParams)
			if (!rp->name().empty())
				namedReturnDecls.push_back(rp.get());
		setNamedReturns(namedReturnDecls);

		// Mapping-storage-ref params: stash for buildBlock to register as mapping-key-params
		// so `m[k]` resolves the dynamic box-key prefix from the runtime bytes value of m.
		// Covers both input params (storage m) and named returns (storage r assigned r=m1).
		std::vector<solidity::frontend::VariableDeclaration const*> mappingKeyParamDecls;
		auto isMappingStorageRef = [&](solidity::frontend::VariableDeclaration const* p) {
			return p->referenceLocation() == solidity::frontend::VariableDeclaration::Location::Storage
				&& isBoxKeyedStorageRef(p->type()) // widened: plain structs too
				&& !p->name().empty();
		};
		for (auto const& p: _func.parameters())
			if (isMappingStorageRef(p.get()))
				mappingKeyParamDecls.push_back(p.get());
		for (auto const& rp: returnParams)
			// Also register box-keyed storage-ref named returns (e.g. V4 Position.State
			// storage): storageRefReturnIsBytesKeyed catches the mapping-holder case
			// that containsMappingType misses for plain-struct elements.
			if (isMappingStorageRef(rp.get())
				|| (rp->referenceLocation()
						== solidity::frontend::VariableDeclaration::Location::Storage
					&& !rp->name().empty() && storageRefReturnIsBytesKeyed(&_func)))
				mappingKeyParamDecls.push_back(rp.get());
		setMappingKeyParams(mappingKeyParamDecls);

		// Blob-backed (>4KB) memory params: stash so body's p.field[i] routes to the blob.
		std::vector<solidity::frontend::VariableDeclaration const*> blobAggParamDecls;
		for (auto const& p: _func.parameters())
			if (p->referenceLocation() == solidity::frontend::VariableDeclaration::Location::Memory
				&& !p->name().empty()
				&& memoryUsesBlob(m_typeMapper.map(p->type())))
				blobAggParamDecls.push_back(p.get());
		setBlobAggParams(blobAggParamDecls);

		m_currentInConstructor = _func.isConstructor();
		m_currentFrameIsProgram =
			_func.visibility() == solidity::frontend::Visibility::Internal
			|| _func.visibility() == solidity::frontend::Visibility::Private;
		method.body = buildBlock(_func.body());
		m_currentInConstructor = false;
		m_currentFrameIsProgram = false;

		// Zero-init named return vars (Solidity implicit init); bump free-memory pointer
		// for every memory-typed return (EVM allocates at entry; tests probe FMP movement).
		{
			auto const& retParams = _func.returnParameters();
			std::vector<std::shared_ptr<awst::Statement>> inits;
			for (auto const& rp: retParams)
			{
				if (rp->name().empty())
					continue;
				// Box-keyed storage-ref named returns hold a bytes box-key, not a struct — skip zero-init.
				if (rp->referenceLocation() == solidity::frontend::VariableDeclaration::Location::Storage
					&& storageRefReturnIsBytesKeyed(&_func))
					continue;
				auto* rpType = m_typeMapper.map(rp->type());

				// >4KB memory returns: pre-zeroed in preamble; skip bzero (pointer model).
				if (rp->referenceLocation() == solidity::frontend::VariableDeclaration::Location::Memory
					&& memoryUsesBlob(rpType))
					continue;

				auto target = awst::makeVarExpression(rp->name(), rpType, method.sourceLocation);

				auto zeroVal = StorageMapper::makeDefaultValue(rpType, method.sourceLocation);

				auto assign = awst::makeAssignmentStatement(std::move(target), std::move(zeroVal), method.sourceLocation);
				inits.push_back(std::move(assign));
			}
			for (auto const& rp: retParams)
			{
				if (rp->referenceLocation()
					!= solidity::frontend::VariableDeclaration::Location::Memory)
					continue;
				auto* rpType = m_typeMapper.map(rp->type());
				int sz = computeEncodedElementSize(rpType);
				if (sz <= 0)
					continue;
				// Blob-backed memory return: bind FMP (before bump) to __blobagg_off_<id>
				// to match blob-aggregate registration in ContractBuilder::buildBlock.
				if (memoryUsesBlob(rpType))
				{
					std::string offN = "__blobagg_off_" + std::to_string(rp->id());
					auto blob = awst::makeLoadSlot(
						AssemblyBuilder::MEMORY_SLOT_FIRST, method.sourceLocation);
					auto base = awst::makeExtractUInt64(std::move(blob),
						awst::makeIntegerConstant("88", method.sourceLocation), method.sourceLocation);
					inits.push_back(awst::makeAssignmentStatement(
						awst::makeVarExpression(offN, awst::WType::uint64Type(), method.sourceLocation),
						std::move(base), method.sourceLocation));
				}
				for (auto& s: AssemblyBuilder::emitFreeMemoryBump(
						sz, method.sourceLocation, static_cast<int>(rp->id())))
					inits.push_back(std::move(s));
			}
			if (!inits.empty())
			{
				method.body->body.insert(
					method.body->body.begin(),
					std::make_move_iterator(inits.begin()),
					std::make_move_iterator(inits.end())
				);
			}
		}

		// Storage-ref pointer: rewrite `return stateVar[idx]` to return just the
		// uint64 index; call sites reconstitute the location (SolInternalCall).
		if (storageRefPointerReturn(&_func))
		{
			std::function<void(std::vector<std::shared_ptr<awst::Statement>>&)> rewriteRet;
			rewriteRet = [&](std::vector<std::shared_ptr<awst::Statement>>& stmts)
			{
				for (auto& stmt: stmts)
				{
					if (auto* ret = dynamic_cast<awst::ReturnStatement*>(stmt.get()))
					{
						if (auto* ix = dynamic_cast<awst::IndexExpression*>(ret->value.get()))
							ret->value = TypeCoercion::implicitNumericCast(
								ix->index, awst::WType::uint64Type(),
								ret->value->sourceLocation);
					}
					else if (auto* ifElse = dynamic_cast<awst::IfElse*>(stmt.get()))
					{
						if (ifElse->ifBranch) rewriteRet(ifElse->ifBranch->body);
						if (ifElse->elseBranch) rewriteRet(ifElse->elseBranch->body);
					}
					else if (auto* block = dynamic_cast<awst::Block*>(stmt.get()))
						rewriteRet(block->body);
				}
			};
			rewriteRet(method.body->body);
		}

		// Synthesize implicit return: named-return vars or default zero.
		if (method.returnType != awst::WType::voidType()
			&& !blockAlwaysTerminates(*method.body))
		{
			auto const& retParams = _func.returnParameters();
			bool hasNamedReturns = false;
			for (auto const& rp: retParams)
				if (!rp->name().empty())
					hasNamedReturns = true;

			auto retStmt = awst::makeReturnStatement(nullptr, method.sourceLocation);

			if (hasNamedReturns)
			{
				if (retParams.size() == 1)
				{
					auto var = awst::makeVarExpression(retParams[0]->name(), m_typeMapper.map(retParams[0]->type()), method.sourceLocation);
					retStmt->value = std::move(var);
				}
				else
				{
					auto tuple = awst::makeTupleExpression(nullptr, method.sourceLocation);
					for (auto const& rp: retParams)
					{
						auto var = awst::makeVarExpression(rp->name(), m_typeMapper.map(rp->type()), method.sourceLocation);
						tuple->items.push_back(std::move(var));
					}
					tuple->wtype = method.returnType;
					retStmt->value = std::move(tuple);
				}
			}
			else
			{
				retStmt->value = StorageMapper::makeDefaultValue(method.returnType, method.sourceLocation);
			}

			// Enum range check on implicit named-return.
			if (hasNamedReturns && retParams.size() == 1)
			{
				if (auto const* enumType = dynamic_cast<solidity::frontend::EnumType const*>(retParams[0]->type()))
				{
					unsigned numMembers = enumType->numberOfMembers();
					auto var = awst::makeVarExpression(retParams[0]->name(), awst::WType::uint64Type(), method.sourceLocation);

					auto assertStmt = awst::makeExpressionStatement(
						awst::makeEnumRangeAssert(std::move(var), numMembers, method.sourceLocation),
						method.sourceLocation);
					method.body->body.push_back(std::move(assertStmt));
				}
			}

			method.body->body.push_back(std::move(retStmt));
		}

		rewriteARC4Returns(method, _func, m_typeMapper, signedReturns, unsignedMasks, funcHasInlineAssembly);

		// Asm bodies handle param data directly via calldataload; skip ARC4 decode.
		bool hasInlineAssembly = false;
		for (auto const& stmt: _func.body().statements())
		{
			if (dynamic_cast<solidity::frontend::InlineAssembly const*>(stmt.get()))
			{
				hasInlineAssembly = true;
				break;
			}
		}

		// Decode ARC4-remapped params: rename arg to __arc4_<name> and stash decodes.
		// Deferred until after modifier inlining: inlineModifiers replaces method.body
		// wholesale, so prepending earlier would bury the decode inside the wrap.
		std::vector<std::shared_ptr<awst::Statement>> deferredDecodes;
		if (!paramDecodes.empty())
		{
			for (auto& pd: paramDecodes)
			{
				// Rename the method arg to __arc4_<name>
				std::string arc4Name = "__arc4_" + pd.name;
				for (auto& arg: method.args)
				{
					if (arg.name == pd.name)
					{
						arg.name = arc4Name;
						arg.wtype = pd.arc4Type; // deferred for asm fns; idempotent for the rest
						break;
					}
				}

				auto arc4Var = awst::makeVarExpression(arc4Name, pd.arc4Type, pd.loc);

				std::shared_ptr<awst::Expression> decodeExpr;
				auto const* refArr = dynamic_cast<awst::ReferenceArray const*>(pd.nativeType);
				if (refArr && !refArr->arraySize().has_value())
				{
					// Dynamic array: ConvertArray (len+substring3) not ARC4Decode,
					// because extract3(v,2,0) returns empty bytes in the puya backend.
					auto convert = awst::makeConvertArray(std::move(arc4Var), pd.nativeType, pd.loc);
					decodeExpr = std::move(convert);
				}
				else
				{
					auto decode = awst::makeARC4Decode(std::move(arc4Var), pd.nativeType, pd.loc);
					// Signed sub-256: ARC4 decode yields N-bit form; sign-extend to 256-bit
					// so ops like getAmount*Delta(int128) liquidity<0 branch read sign correctly.
					if (pd.signedBits > 0)
						decodeExpr = TypeCoercion::signExtendToUint256(
							std::move(decode), pd.signedBits, pd.loc);
					else
						decodeExpr = std::move(decode);
				}

				auto target = awst::makeVarExpression(pd.name, pd.nativeType, pd.loc);

				auto assign = awst::makeAssignmentStatement(std::move(target), std::move(decodeExpr), pd.loc);
				deferredDecodes.push_back(std::move(assign));
			}
		}

		// Sub-64-bit / bool / enum params: AVM uint64 doesn't auto-clean like EVM; guard explicitly.
		{
			bool useV2 = true; // default in 0.8+
			if (m_currentContract)
			{
				auto const& ann = m_currentContract->sourceUnit().annotation();
				if (ann.useABICoderV2.set())
					useV2 = *ann.useABICoderV2;
			}
			auto entryChecks = buildABIEntryChecks(_func, useV2, m_sourceFile);
			if (!entryChecks.empty())
			{
				method.body->body.insert(
					method.body->body.begin(),
					std::make_move_iterator(entryChecks.begin()),
					std::make_move_iterator(entryChecks.end())
				);
			}
		}

		// Transient blob init is in the approval-program preamble (TRANSIENT_SLOT);
		// per-method init would reset it mid-dispatch, clobbering earlier writes.

		// Legacy: textual _ expansion; via IR: separate subroutines per modifier.
		if (!_func.modifiers().empty())
		{
			if (m_viaIR)
				buildModifierChain(_func, method, _contractName);
			else
				inlineModifiers(_func, method.body);
		}

		// Insert deferred ARC4 decodes at top of the now-modifier-wrapped body.
		if (!deferredDecodes.empty())
		{
			method.body->body.insert(
				method.body->body.begin(),
				std::make_move_iterator(deferredDecodes.begin()),
				std::make_move_iterator(deferredDecodes.end())
			);

			// Self-recursive callsub fix-up: after param remap, internal f(...) calls
			// still pass biguint args; wrap each remapped position in ARC4Encode.
			std::string thisName = eb::CallResolver::resolveMethodName(m_tr->contractCtx, _func);
			std::map<std::string, awst::WType const*> arc4ByOrig;
			for (auto const& pd: paramDecodes)
				arc4ByOrig[pd.name] = pd.arc4Type;

			SubroutineCallVisitor wrapper([&](awst::SubroutineCallExpression& _call) {
				auto const* tgt = std::get_if<awst::InstanceMethodTarget>(&_call.target);
				if (!tgt || tgt->memberName != thisName)
					return;
				size_t argI = 0;
				for (auto const& pd: paramDecodes)
				{
					if (argI >= _call.args.size()) break;
					auto& a = _call.args[argI++];
					if (!a.value || a.value->wtype == pd.arc4Type)
						continue;
					if (a.value->wtype != awst::WType::biguintType())
						continue;
					auto enc = awst::makeARC4Encode(std::move(a.value), pd.arc4Type, a.value->sourceLocation);
					a.value = std::move(enc);
				}
			});
			wrapper.visitStmt(*method.body);
		}

		// ensure_budget: per-function map first, then global opup budget.
		uint64_t budgetForFunc = 0;
		if (auto it = m_ensureBudget.find(_func.name()); it != m_ensureBudget.end())
			budgetForFunc = it->second;
		else if (m_opupBudget > 0 && method.arc4MethodConfig.has_value())
			budgetForFunc = m_opupBudget;

		if (budgetForFunc > 0)
		{
			auto budgetVal = awst::makeIntegerConstant(budgetForFunc, method.sourceLocation);

			auto feeSource = awst::makeZero(method.sourceLocation);

			auto call = awst::makePuyaLibCall("ensure_budget",
				{
					awst::CallArg{std::string("required_budget"), budgetVal},
					awst::CallArg{std::string("fee_source"), feeSource}
				},
				awst::WType::voidType(), method.sourceLocation);

			auto stmt = awst::makeExpressionStatement(std::move(call), method.sourceLocation);

			method.body->body.insert(method.body->body.begin(), std::move(stmt));
		}

		// Non-payable check: assert no preceding PaymentTxn has non-zero amount.
		// Skipped for internal/private and receive() (implicitly payable).
		if (!_func.isPayable() && !_func.isReceive())
			prependNonPayableCheck(method);
	}
	else
	{
		// Abstract — empty body.
		Logger::instance().debug("function '" + method.memberName + "' has no implementation", method.sourceLocation);
		method.body = awst::makeBlock(method.sourceLocation);
	}

	// Write-back augmentation for mutated MEMORY-ref params (Solidity passes memory by ref).
	// No-op unless the method mutates a memory struct/array param; the internal caller writes back.
	augmentMethodForMutatedMemoryParams(method, _func, m_typeMapper);

	return method;
}


std::optional<awst::ARC4MethodConfig> ContractBuilder::buildARC4Config(
	solidity::frontend::FunctionDefinition const& _func,
	awst::SourceLocation const& _loc
)
{
	using namespace solidity::frontend;

	auto vis = _func.visibility();

	if (vis == Visibility::Private || vis == Visibility::Internal)
		return std::nullopt;

	awst::ARC4ABIMethodConfig config;
	config.sourceLocation = _loc;
	// Both fallback and receive have empty Solidity names; distinguish for routing.
	if (_func.isFallback())
		config.name = "__fallback";
	else if (_func.isReceive())
		config.name = "__receive";
	else
		config.name = _func.name();
	config.allowedCompletionTypes = {0}; // NoOp
	config.create = 3; // Disallow

	if (_func.stateMutability() == StateMutability::View ||
		_func.stateMutability() == StateMutability::Pure)
	{
		config.readonly = true;
	}

	// uros chunk: @custom:uros-chunk <name>
	if (_func.documentation())
		config.chunk = natSpecTagValue(*_func.documentation()->text(), "custom:uros-chunk");

	return awst::ARC4MethodConfig(config);
}


} // namespace puyasol::builder
