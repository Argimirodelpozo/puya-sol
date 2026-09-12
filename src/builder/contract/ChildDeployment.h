#pragma once

namespace puyasol::builder
{

/// Child schema comes from the backend ARC56 artifact, not solc storage slots.
/// Shared by template declarations, inner-create fields, and artifact binding.
struct ChildSchemaField
{
	char const* transactionField;
	char const* scope;
	char const* kind;
};
inline constexpr ChildSchemaField childSchemaFields[] = {
	{"GlobalNumUint", "global", "ints"},
	{"GlobalNumByteSlice", "global", "bytes"},
	{"LocalNumUint", "local", "ints"},
	{"LocalNumByteSlice", "local", "bytes"},
};

} // namespace puyasol::builder
