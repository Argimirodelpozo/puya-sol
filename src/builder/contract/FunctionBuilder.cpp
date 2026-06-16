#include "builder/contract/ContractBuilder.h"
#include "awst/Termination.h"
#include "builder/AWSTBuilder.h"
#include "builder/NatSpecTags.h"
#include "builder/assembly/AssemblyBuilder.h"
#include "builder/contract/ParamABIValidator.h"
#include "builder/contract/ReturnRewriter.h"
#include "builder/sol-ast/stmts/SolBlock.h"
#include "builder/sol-ast/StorageRefPointer.h"
#include "builder/itxn/CallResolver.h"
#include "builder/sol-types/OverloadSuffix.h"
#include "builder/sol-types/Arc4Defaults.h"
#include "builder/sol-types/TypeCoercion.h"
#include "Logger.h"

#include <libsolidity/ast/ASTVisitor.h>

namespace puyasol::builder
{

using awst::blockAlwaysTerminates;

namespace {

/// Recursively visit each `SubroutineCallExpression` reachable from
/// `_expr`/`_stmt`. Used to retarget self-recursive callsubs after the
/// public method's biguint params have been remapped to ARC4 — see
/// `wrapRecursiveCallsubArgs` below for the rewrite.
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

awst::ContractMethod ContractBuilder::buildFunction(
	solidity::frontend::FunctionDefinition const& _func,
	std::string const& _contractName,
	std::string const& _nameOverride
)
{
	// Per-method scope state (funcPtrTargets, storageAliases, constantLocals)
	// lives on the flat ScopeState reached via the BlockContext that buildBlock
	// pushes for the function body — no manual snapshot/restore needed here.

	awst::ContractMethod method;
	method.sourceLocation = makeLoc(_func.location());
	method.cref = m_sourceFile + "." + _contractName;
	// Use name override if provided, otherwise disambiguate overloaded names
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
		// Function-pointer mapping (uint64 for Internal, bytes[12] for
		// External/DelegateCall) is handled inside `TypeMapper::map`'s
		// `Type::Category::Function` case — no override needed here.
		arg.wtype = m_typeMapper.map(param->type());
		// Memory aggregate param >4KB → passed as its uint64 base offset (blob
		// pointer model). The callee re-registers it as a blob aggregate
		// (setBlobAggParams below) so `p.field[i]` lowers to blob word access.
		if (param->referenceLocation() == solidity::frontend::VariableDeclaration::Location::Memory
			&& computeEncodedElementSize(arg.wtype) > AssemblyBuilder::SLOT_SIZE)
			arg.wtype = awst::WType::uint64Type();
		method.args.push_back(std::move(arg));
		paramIndex++;
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
		// Storage reference returns (from functions with .slot assembly):
		// return type is biguint (slot number), not the array type.
		if (returnParams[0]->referenceLocation() == solidity::frontend::VariableDeclaration::Location::Storage
			&& _func.isImplemented()
			&& std::any_of(_func.body().statements().begin(), _func.body().statements().end(),
				[](auto const& s) { return dynamic_cast<solidity::frontend::InlineAssembly const*>(s.get()); }))
			method.returnType = awst::WType::biguintType();
		// Storage-ref pointer function (`return <stateVar>[<idx>];`): the
		// function returns only the uint64 index; call sites reconstitute
		// the storage location as IndexExpression(<stateVar>, <call>).
		else if (storageRefPointerReturn(&_func))
			// Box-keyed mapping-of-struct storage ref (e.g. `return _pools[id]`
			// returning `Pool.State storage`): the function returns the bytes
			// box-key prefix. Array/slot refs still return the uint64 index.
			// Keyed off the HOLDER being a mapping (storageRefReturnIsBytesKeyed),
			// not the struct contents — so plain-struct mapping elements (e.g. V4
			// Position.State, no nested mappings) are still box-keyed.
			method.returnType = storageRefReturnIsBytesKeyed(&_func)
				? awst::WType::bytesType()
				: awst::WType::uint64Type();
		// For signed integer returns ≤64 bits, promote to biguint for proper
		// 256-bit two's complement ARC4 encoding.
		// Unwrap UserDefinedValueType/EnumType to find the underlying IntegerType.
		auto const* retSolType = returnParams[0]->type();
		if (auto const* udvt = dynamic_cast<solidity::frontend::UserDefinedValueType const*>(retSolType))
			retSolType = &udvt->underlyingType();
		auto const* intType = dynamic_cast<solidity::frontend::IntegerType const*>(retSolType);
		// Enums are uint8 in ABI — treat as unsigned 8-bit
		if (!intType)
			if (auto const* enumType = dynamic_cast<solidity::frontend::EnumType const*>(retSolType))
				intType = dynamic_cast<solidity::frontend::IntegerType const*>(
					enumType->encodingType());
		// Biguint promotion is only for ABI sign-extension (public/external).
		// Private/internal functions keep the native uint64 return so their
		// body's `return IntegerConstant(uint64, …)` matches the declared type.
		bool isAbiBoundary = _func.isPartOfExternalInterface();
		if (intType && intType->isSigned())
		{
			if (intType->numBits() <= 64 && isAbiBoundary)
				method.returnType = awst::WType::biguintType();
			if (isAbiBoundary)
				signedReturns.push_back({intType->numBits(), 0});
		}
		else if (intType && !intType->isSigned() && intType->numBits() < 64)
		{
			if (isAbiBoundary)
				unsignedMasks.push_back({intType->numBits(), 0});
		}
	}
	else
	{
		// Multiple return values → tuple
		std::vector<awst::WType const*> types;
		std::vector<std::string> names;
		bool hasNames = false;
		for (size_t ri = 0; ri < returnParams.size(); ++ri)
		{
			auto const& rp = returnParams[ri];
			auto* mappedType = m_typeMapper.map(rp->type());
			// Detect signed/narrow integer elements for sign-extension/masking
			auto const* retSolType = rp->type();
			if (auto const* udvt = dynamic_cast<solidity::frontend::UserDefinedValueType const*>(retSolType))
				retSolType = &udvt->underlyingType();
			auto const* intType = dynamic_cast<solidity::frontend::IntegerType const*>(retSolType);
			if (!intType)
				if (auto const* enumType = dynamic_cast<solidity::frontend::EnumType const*>(retSolType))
					intType = dynamic_cast<solidity::frontend::IntegerType const*>(
						enumType->encodingType());
			// Biguint promotion only at ABI boundary (public/external).
			bool isAbiBoundary = _func.isPartOfExternalInterface();
			if (intType)
			{
				if (intType->isSigned())
				{
					if (intType->numBits() <= 64 && isAbiBoundary)
						mappedType = awst::WType::biguintType();
					if (isAbiBoundary)
						signedReturns.push_back({intType->numBits(), ri});
				}
				else if (!intType->isSigned() && intType->numBits() < 64)
				{
					if (isAbiBoundary)
						unsignedMasks.push_back({intType->numBits(), ri});
				}
			}
			types.push_back(mappedType);
			names.push_back(rp->name());
			if (!rp->name().empty())
				hasNames = true;
		}
		if (hasNames)
		{
			// Use function name + "Return" as the struct name to avoid
			// ARC56 collision when multiple methods return different named tuples.
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

	// uros chunked methods must NEVER be inlined: an inlined copy at an
	// internal call site (e.g. verifySumcheck → relChunkMem) would bake the
	// body into the CALLER's chunk, defeating the split (the body is duplicated
	// instead of stubbed + reached via the staged inner-txn). Forcing a real
	// SubroutineCall lets the uros backend stub it in non-owning chunks. No
	// effect outside @custom:splitter contracts (chunk is empty there).
	if (method.arc4MethodConfig.has_value())
		if (auto* abiCfg = std::get_if<awst::ARC4ABIMethodConfig>(&*method.arc4MethodConfig))
			if (!abiCfg->chunk.empty())
				method.inlineOpt = false;

	// For ARC4 methods, convert array/tuple parameter types to ARC4 encoding
	// and prepare decode operations for the function body
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
	// Detect inline assembly early — needed to skip ARC4 param wrapping
	// which would break assembly variable references.
	bool funcHasInlineAssembly = false;
	if (_func.isImplemented())
	{
		for (auto const& stmt: _func.body().statements())
			if (dynamic_cast<solidity::frontend::InlineAssembly const*>(stmt.get()))
			{ funcHasInlineAssembly = true; break; }
	}

	// (Self-recursion no longer gates the remap — recursive callsubs are
	// rewritten post-translation to wrap their biguint args in ARC4Encode
	// so they match the now-arc4-typed param. See the wrap pass below.)

	if (method.arc4MethodConfig.has_value())
	{
		// Remap types to ARC4 encoding for ABI-exposed methods.
		// This ensures correct ABI signatures (e.g., uint256 not uint512 for biguint).
		auto const& solParams = _func.parameters();
		for (size_t pi = 0; pi < method.args.size(); ++pi)
		{
			auto& arg = method.args[pi];

			// Remap biguint args to ARC4UIntN with the original Solidity bit width.
			// Without this, puya maps biguint→uint512 (AVM max) instead of uint256,
			// breaking ABI selectors for callers that use the natural f(uint256)
			// signature. Signed integers use the same 256-bit two's complement
			// encoding — we keep ARC4UIntN(256) and let the test runner's
			// _abi_safe_type helper map int<N>→uint<N> so encode/decode line up.
			//
			// One skip condition:
			//   - inline assembly: body references the param by its original
			//     name; renaming would break Yul references.
			//
			// Self-recursive functions are also remapped: a post-translation
			// walk over `method.body` (further down in this method) wraps
			// each recursive callsub's biguint arg in ARC4Encode so it
			// matches the now-arc4-typed param.
			if (arg.wtype == awst::WType::biguintType() && pi < solParams.size()
				&& !funcHasInlineAssembly)
			{
				auto const* solType = solParams[pi]->annotation().type;
				auto const* intType = solType ? dynamic_cast<solidity::frontend::IntegerType const*>(solType) : nullptr;
				if (!intType && solType)
					if (auto const* udvt = dynamic_cast<solidity::frontend::UserDefinedValueType const*>(solType))
						intType = dynamic_cast<solidity::frontend::IntegerType const*>(&udvt->underlyingType());
				unsigned bits = intType ? intType->numBits() : 256;
				auto const* arc4Type = m_typeMapper.createType<awst::ARC4UIntN>(static_cast<int>(bits));
				// A signed sub-256 (64<N<256) param decodes to N-bit two's complement
				// (2^N - X for negatives, bit 255 clear); sign-extend to canonical
				// 256-bit after the decode so downstream 256-bit ops (signed compare,
				// negate, casts) read its sign correctly. int256 is already canonical;
				// <=64-bit signed is uint64-backed and handled by buildABIEntryChecks.
				unsigned signedBits =
					(intType && intType->isSigned() && bits > 64 && bits < 256) ? bits : 0;
				paramDecodes.push_back({arg.name, arg.wtype, arc4Type, arg.sourceLocation, 0, signedBits});
				arg.wtype = arc4Type;
				continue;
			}

			// Remap aggregate types (arrays, tuples) and external fn-ptr
			// bytes[12] to ARC4 encoding. General bytes/bytes[N] params
			// are NOT remapped — only fn-ptr-specific bytes[12].
			bool isAggregate = arg.wtype
				&& (arg.wtype->kind() == awst::WTypeKind::ReferenceArray
					|| arg.wtype->kind() == awst::WTypeKind::ARC4StaticArray
					|| arg.wtype->kind() == awst::WTypeKind::ARC4DynamicArray
					|| arg.wtype->kind() == awst::WTypeKind::WTuple);
			// External fn-ptr: bytes[12] needs ARC4 remapping to byte[12]
			if (!isAggregate && pi < solParams.size())
			{
				if (dynamic_cast<solidity::frontend::FunctionType const*>(solParams[pi]->type())
					&& arg.wtype && arg.wtype->kind() == awst::WTypeKind::Bytes)
					isAggregate = true;
			}
			// Skip the aggregate->ARC4 remap for inline-assembly bodies, matching
			// the biguint remap guard above: the decode is suppressed for asm
			// functions (see the `!hasInlineAssembly` guard at the decode-insert
			// site), so remapping the signature without a decode would leave the
			// body reading ARC4 bytes where it expects the native aggregate.
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

	// Function body
	if (_func.isImplemented())
	{
		// Set function context for inline assembly translation
		// Use the (possibly ARC4-remapped) types from the method args
		{
			std::vector<std::pair<std::string, awst::WType const*>> paramContext;
			std::map<std::string, unsigned> bitWidths;
			for (auto const& arg: method.args)
				paramContext.emplace_back(arg.name, arg.wtype);
			// Collect sub-64-bit widths from function params and return params
			for (auto const& p: _func.parameters())
			{
				auto const* solType = p->annotation().type;
				auto const* intType = solType ? dynamic_cast<solidity::frontend::IntegerType const*>(solType) : nullptr;
				if (!intType && solType)
					if (auto const* udvt = dynamic_cast<solidity::frontend::UserDefinedValueType const*>(solType))
						intType = dynamic_cast<solidity::frontend::IntegerType const*>(&udvt->underlyingType());
				if (intType && intType->numBits() < 64)
					bitWidths[p->name()] = intType->numBits();
			}
			for (auto const& rp: _func.returnParameters())
			{
				auto const* solType = rp->annotation().type;
				auto const* intType = solType ? dynamic_cast<solidity::frontend::IntegerType const*>(solType) : nullptr;
				if (!intType && solType)
					if (auto const* udvt = dynamic_cast<solidity::frontend::UserDefinedValueType const*>(solType))
						intType = dynamic_cast<solidity::frontend::IntegerType const*>(&udvt->underlyingType());
				if (intType && intType->numBits() < 64)
					bitWidths[rp->name()] = intType->numBits();
			}
			setFunctionContext(paramContext, method.returnType, bitWidths);
		}

		// Stash the named-return decls in m_currentNamedReturns for buildBlock,
		// which uses them to register large (>4KB) memory returns as blob-backed
		// aggregates. (No shadow-rename registration any more: local names are
		// mangled by decl id — see sol_ast::Context::awstVarName.)
		std::vector<solidity::frontend::VariableDeclaration const*> namedReturnDecls;
		for (auto const& rp: returnParams)
			if (!rp->name().empty())
				namedReturnDecls.push_back(rp.get());
		setNamedReturns(namedReturnDecls);

		// Register mapping-storage-ref params as mapping-key-params so that
		// `m[k]` inside the body resolves the dynamic box-key prefix from
		// the runtime bytes value of `m` (the holder name passed by the
		// caller). Both input params and return params need this binding:
		//
		//   function f(mapping(K=>V) storage m, ...) → m is bound to the
		//     bytes value passed by callers (state-var name).
		//   function f() returns (mapping(K=>V) storage r) → r is a local
		//     pointer assigned via `r = m1;` — same dynamic-prefix shape.
		//
		// AWSTBuilder's freestanding-subroutine path handles input-param
		// registration for library/free functions, but contract methods
		// reach here, so we must register them too. The actual setMappingKeyParam
		// calls happen inside `buildBlock` after the FunctionContext is
		// pushed — stash the decls in `m_currentMappingKeyParams` for it
		// to pick up.
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
			// Also register a box-keyed storage-ref NAMED return (e.g. V4
			// `returns (Position.State storage position)` with `position = self[k];`)
			// even when the struct has no nested mappings — so `position` resolves to
			// the element's runtime box-key and the synthesised `return position;`
			// hands back that key (storageRefReturnIsBytesKeyed gates the mapping-
			// holder case that isMappingStorageRef/containsMappingType misses).
			if (isMappingStorageRef(rp.get())
				|| (rp->referenceLocation()
						== solidity::frontend::VariableDeclaration::Location::Storage
					&& !rp->name().empty() && storageRefReturnIsBytesKeyed(&_func)))
				mappingKeyParamDecls.push_back(rp.get());
		setMappingKeyParams(mappingKeyParamDecls);

		// Blob-backed (>4KB) memory aggregate params: register so the body's
		// `p.field[i]` reads/writes route through the multi-slot blob at the
		// passed-in base offset (the param's value). Mirrors mappingKeyParamDecls.
		std::vector<solidity::frontend::VariableDeclaration const*> blobAggParamDecls;
		for (auto const& p: _func.parameters())
			if (p->referenceLocation() == solidity::frontend::VariableDeclaration::Location::Memory
				&& !p->name().empty()
				&& computeEncodedElementSize(m_typeMapper.map(p->type())) > AssemblyBuilder::SLOT_SIZE)
				blobAggParamDecls.push_back(p.get());
		setBlobAggParams(blobAggParamDecls);

		m_currentInConstructor = _func.isConstructor();
		m_currentFrameIsProgram =
			_func.visibility() == solidity::frontend::Visibility::Internal
			|| _func.visibility() == solidity::frontend::Visibility::Private;
		method.body = buildBlock(_func.body());
		m_currentInConstructor = false;
		m_currentFrameIsProgram = false;

		// Insert zero-initialization for named return variables
		// Solidity implicitly initializes named returns to their zero values.
		// This is critical for struct types where field-by-field assignment
		// reads other fields from the variable via copy-on-write pattern.
		//
		// Also: every memory-typed return parameter (named or unnamed) gets a
		// fresh allocation in EVM, advancing mload(0x40) at function entry.
		// We mirror this so tests that probe free-memory-pointer movement
		// across calls see the expected bumps.
		{
			auto const& retParams = _func.returnParameters();
			std::vector<std::shared_ptr<awst::Statement>> inits;
			for (auto const& rp: retParams)
			{
				if (rp->name().empty())
					continue;
				// Box-keyed storage-ref named returns hold a bytes box-key (a
				// mappingKeyParam), not a struct value — skip the struct zero-init.
				if (rp->referenceLocation() == solidity::frontend::VariableDeclaration::Location::Storage
					&& storageRefReturnIsBytesKeyed(&_func))
					continue;
				auto* rpType = m_typeMapper.map(rp->type());

				// Blob-backed (>4KB) memory returns are pre-zeroed in the preamble;
				// skip the (oversized) bzero zero-init — they use the pointer model.
				if (rp->referenceLocation() == solidity::frontend::VariableDeclaration::Location::Memory
					&& computeEncodedElementSize(rpType) > AssemblyBuilder::SLOT_SIZE)
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
				// Blob-backed (>4KB) memory return: bind its runtime base offset
				// (current FMP, before the bump) to __blobagg_off_<id>, matching the
				// blob-aggregate registration in ContractBuilder::buildBlock.
				if (sz > AssemblyBuilder::SLOT_SIZE)
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

		// Storage-ref pointer function: rewrite `return <stateVar>[<idx>];`
		// (translated to `return IndexExpression(base, index)`) to return
		// just the uint64 `index`. The location is reconstituted at call
		// sites — see storageRefPointerReturn / SolInternalCall. returnType
		// was already set to uint64 above.
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

		// Ensure all non-void functions end with a return statement.
		// For named return parameters, synthesize a return referencing the variables.
		// Otherwise append a default zero-value return.
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

			// Enum range validation for implicit return of named return variables
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

		// Skip ARC4 decode for functions with inline assembly blocks.
		// The assembly translator handles parameter data directly via
		// calldataload mapping using ARC4-encoded types.
		bool hasInlineAssembly = false;
		for (auto const& stmt: _func.body().statements())
		{
			if (dynamic_cast<solidity::frontend::InlineAssembly const*>(stmt.get()))
			{
				hasInlineAssembly = true;
				break;
			}
		}

		// Build ARC4 decode operations for aggregate / biguint parameters.
		// The method args were remapped to ARC4 types but the body refs the
		// native names. We rename each arg to `__arc4_<name>` and stash the
		// decode statements; insertion is DEFERRED until after modifier
		// inlining (below) so the decode lands at the top of the eventually
		// modifier-wrapped body. Otherwise modifier-inlined pre-code that
		// reads the param sees an uninitialised local — `inlineModifiers`
		// replaces `method.body` wholesale, burying anything we prepend
		// before the rewrap.
		std::vector<std::shared_ptr<awst::Statement>> deferredDecodes;
		if (!paramDecodes.empty() && !hasInlineAssembly)
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
						break;
					}
				}

				// Create: <name> = arc4_decode(__arc4_<name>)
				// For dynamic arrays (ReferenceArray with null array_size), use
				// IntrinsicCall("extract", [2, 0]) to strip the ARC4 length header
				// instead of ARC4Decode — works around a puya backend issue where
				// extract3(value, 2, 0) returns empty bytes instead of extracting
				// to end (see puya-possible-bug.md).
				auto arc4Var = awst::makeVarExpression(arc4Name, pd.arc4Type, pd.loc);

				std::shared_ptr<awst::Expression> decodeExpr;
				auto const* refArr = dynamic_cast<awst::ReferenceArray const*>(pd.nativeType);
				if (refArr && !refArr->arraySize().has_value())
				{
					// Dynamic array: use ConvertArray instead of ARC4Decode.
					// This works around a puya backend bug where ARC4Decode
					// uses extract3(value, 2, 0) to strip the length header,
					// but extract3 with length=0 returns empty bytes instead
					// of extracting to end (see puya-possible-bug.md).
					// ConvertArray uses len+substring3 which works correctly.
					auto convert = awst::makeConvertArray(std::move(arc4Var), pd.nativeType, pd.loc);
					decodeExpr = std::move(convert);
				}
				else
				{
					auto decode = awst::makeARC4Decode(std::move(arc4Var), pd.nativeType, pd.loc);
					// Canonicalise a signed sub-256 param to 256-bit two's complement —
					// the ARC4 decode yields the N-bit form (2^N - X for negatives), and
					// downstream 256-bit ops (the getAmount*Delta(int128) `liquidity<0`
					// branch + its `-x`) would otherwise misread the sign.
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
			// For modifier-less functions we could insert here, but to keep
			// a single insertion point we always defer to post-inline below.
		}

		// Mask / range-validate sub-64-bit and bool/enum parameters at
		// function entry — EVM cleans ABI-decoded values automatically;
		// AVM uint64 preserves the full value, so the guard is explicit.
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

		// Transient-storage blob init lives in the approval-program preamble
		// (scratch slot TRANSIENT_SLOT, bzero(SLOT_SIZE)). Per-method init
		// would reset the blob mid-dispatch, clobbering writes made by
		// earlier callsub frames in the same app call.


		// Modifier inlining strategy depends on codegen mode:
		// - Legacy (default): textual _ expansion, shared local variables
		// - Via IR: separate subroutines per modifier, fresh vars per _ invocation
		if (!_func.modifiers().empty())
		{
			if (m_viaIR)
				buildModifierChain(_func, method, _contractName);
			else
				inlineModifiers(_func, method.body);
		}

		// Insert ARC4 param-decode statements (built earlier, deferred so
		// that they land at the very top of the now-modifier-wrapped body).
		// `inlineModifiers` replaces `method.body` with a fresh wrapper, so
		// inserting earlier would have buried the decode inside the wrap.
		if (!deferredDecodes.empty())
		{
			method.body->body.insert(
				method.body->body.begin(),
				std::make_move_iterator(deferredDecodes.begin()),
				std::make_move_iterator(deferredDecodes.end())
			);

			// Recursive-callsub fix-up: any `f(...)` inside `f`'s body
			// will have been translated to a SubroutineCallExpression
			// targeting `InstanceMethodTarget{thisName}` with biguint
			// args. After the param remap above, the method now expects
			// ARC4-typed args, so the internal callsub needs to encode
			// each remapped arg before passing. Walk the post-modifier-
			// inline body and wrap matching arg positions in ARC4Encode.
			std::string thisName = eb::CallResolver::resolveMethodName(m_tr->contractCtx, _func);
			std::map<std::string, awst::WType const*> arc4ByOrig;
			for (auto const& pd: paramDecodes)
				arc4ByOrig[pd.name] = pd.arc4Type;

			SubroutineCallVisitor wrapper([&](awst::SubroutineCallExpression& _call) {
				auto const* tgt = std::get_if<awst::InstanceMethodTarget>(&_call.target);
				if (!tgt || tgt->memberName != thisName)
					return;
				// Position arg → method.args[i].name → original name
				// before rename was `__arc4_<orig>`. We use the order in
				// `paramDecodes` (which mirrors method.args order) to
				// know which positions need encoding.
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

		// Inject ensure_budget for opup budget padding
		// Check per-function map first, then global opup budget
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

		// Non-payable check: for public/external functions not marked `payable`,
		// assert that no preceding PaymentTxn in the group carries a non-zero
		// amount to this contract. Mirrors Solidity's `callvalue` check that
		// reverts non-payable calls receiving ether.
		//
		// Skipped for internal/private (not externally callable) and for the
		// receive() function (implicitly payable).
		if (!_func.isPayable() && !_func.isReceive())
			prependNonPayableCheck(method);
	}
	else
	{
		// Abstract function — empty body
		Logger::instance().debug("function '" + method.memberName + "' has no implementation", method.sourceLocation);
		method.body = awst::makeBlock(method.sourceLocation);
	}

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

	// Public/external functions get ARC4 ABI method configs
	awst::ARC4ABIMethodConfig config;
	config.sourceLocation = _loc;
	// Distinguish fallback from receive: both have empty Solidity names,
	// but need different ARC4 method names for routing.
	if (_func.isFallback())
		config.name = "__fallback";
	else if (_func.isReceive())
		config.name = "__receive";
	else
		config.name = _func.name();
	config.allowedCompletionTypes = {0}; // NoOp
	config.create = 3; // Disallow

	// View functions are readonly
	if (_func.stateMutability() == StateMutability::View ||
		_func.stateMutability() == StateMutability::Pure)
	{
		config.readonly = true;
	}

	// uros splitter chunk assignment: `@custom:uros-chunk <name>` on the method.
	if (_func.documentation())
		config.chunk = natSpecTagValue(*_func.documentation()->text(), "custom:uros-chunk");

	return awst::ARC4MethodConfig(config);
}


} // namespace puyasol::builder
