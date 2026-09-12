#pragma once

#include "builder/sol-ast/EvmSlotLowering.h"
#include "builder/storage/StorageMapper.h"
#include <variant>

namespace puyasol::builder::sol_ast
{

/// An already-evaluated assignment destination. Address/key/index evaluation
/// belongs to construction; reads and writes never lower the Solidity AST again.
/// Values use the solc type's native carrier, not the storage encoding.
class ResolvedLValue
{
public:
	using Expr = std::shared_ptr<awst::Expression>;
	/// Source-only classification: no operands run until construction. Retains
	/// an aggregate path so RHS-first assignment need not walk the LHS twice.
	class Resolution
	{
	public:
		Resolution() = default;
		bool isAddressed() const { return !std::holds_alternative<std::monostate>(m_kind); }
		bool isBoxedAggregate() const { return std::holds_alternative<AggregatePath>(m_kind); }
	private:
		friend class ResolvedLValue;
		struct Slot {};
		struct Blob {};
		struct AggregatePath
		{
			solidity::frontend::VariableDeclaration const* declaration;
			std::vector<solidity::frontend::Expression const*> steps;
			std::string key, offset;
			StorageMapper::PhysicalBinding binding;
			bool paged;
		};
		using Kind = std::variant<std::monostate, solidity::frontend::VariableDeclaration const*, Slot, Blob, AggregatePath>;
		explicit Resolution(Kind kind): m_kind(std::move(kind)) {}
		Kind m_kind;
	};
	static Resolution classify(eb::ContractContext& _ctx,
		solidity::frontend::Expression const& _source, bool _alreadyBuilt = false);

	ResolvedLValue(eb::ContractContext& _ctx,
		solidity::frontend::Expression const& _source,
		awst::SourceLocation const& _loc, Expr _built = nullptr);
	ResolvedLValue(eb::ContractContext& _ctx,
		solidity::frontend::Expression const& _source,
		awst::SourceLocation const& _loc, Resolution _resolution, Expr _built = nullptr);

	/// Freeze address/key/index operands without snapshotting container contents.
	static Expr freezeTarget(eb::ContractContext& ctx, Expr target, awst::SourceLocation const& loc);

	Expr read();
	/// Store a native value now and return the assigned scalar/value, or the
	/// destination slot handle for a storage-reference assignment result.
	Expr write(Expr _value);
	void clear();

private:
	struct Target { Expr value; };
	struct Slot { EvmSlotLowering::Addr address; Expr byteIndex; };
	struct Blob { Expr offset; bool packedByte = false; };
	struct Transient { solidity::frontend::VariableDeclaration const* declaration; };
	struct Aggregate
	{
		Expr target, root, initial, key, offset, box;
		std::shared_ptr<awst::Statement> ensure;
	};
	std::variant<Target, Slot, Blob, Transient, Aggregate> m_destination;
	Expr target() const;
	void writeTarget(Expr _target, Expr _value);
	Expr pin(Expr _value);
	static std::optional<Resolution::AggregatePath> aggregatePath(eb::ContractContext& _ctx,
		solidity::frontend::Expression const& _source);
	Aggregate resolveAggregate(Resolution::AggregatePath const& _path);
	void loadAggregate();

	eb::ContractContext& m_ctx;
	solidity::frontend::Type const* m_type;
	awst::WType const* m_native;
	awst::SourceLocation m_loc;
};

} // namespace puyasol::builder::sol_ast
