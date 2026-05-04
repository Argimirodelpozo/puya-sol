#pragma once

/// @file UrosSplitter.h
///
/// **`--uros-splitter` technique** — split named functions out of an
/// over-large contract by producing a small `main` contract + N
/// **chunks** of bytecode that live in the orchestrator's box storage.
/// At runtime the orchestrator splices the right chunk into main per
/// call.
///
/// Layout:
///
///   `main`     — full contract surface, but split methods have STUB
///                bodies (the orc-guard asserts then `return default`).
///                Real bodies remain for every non-split method. Same
///                ABI selectors, state-var schema, constructor.
///
///   `chunk_i`  — bytecode-only artifact (NEVER DEPLOYED on its own).
///                Its `methods` list carries real bodies for the
///                methods in group `i`, plus a copy of every other
///                method as a (cheap) stub so puya still emits
///                identical state layout. Same state slots/keys as
///                main so storage continuity is preserved across swaps.
///                Stored in box `__codebox_chunk_<i>` on the orch.
///
///   `orchestrator` (separate algopy artifact, compiled via puyapy)
///                holds `__codebox_main` + `__codebox_chunk_0..N-1`
///                in box storage and a `__chunk_for_selector` mapping.
///                Its `dispatch()` submits an itxn group:
///                  1. read selector from gtxn[group_index-1]
///                  2. look up which chunk holds that selector
///                  3. UpdateApplication main with chunk_i's bytes
///                  4. NoOp call main with the user's selector + args
///                     (now executing chunk_i's body against main's
///                     storage)
///                  5. UpdateApplication main with main's bytes (restore)
///
/// User submits a group `[stub_call_to_main, dispatch_call_to_orch]`.
/// The stub no-ops at the surface but its args land in
/// gtxn[N-1].ApplicationArgs for the orch to forward.
///
/// Chunks **don't deploy**. They exist only as bytecode payloads in the
/// orch's box storage. The splitter's job is to produce N AWST root
/// sets — one per chunk — that compile through puya into the bytecode
/// that goes into each box.

#include "awst/Node.h"

#include <memory>
#include <set>
#include <string>
#include <vector>

namespace puyasol::splitter
{

class UrosSplitter
{
public:
	struct Chunk
	{
		/// AWST root set that puya compiles into this chunk's bytecode.
		std::vector<std::shared_ptr<awst::RootNode>> roots;
		/// Method names in this chunk (subset of the user's input
		/// for this group, intersected with what's present in the
		/// primary contract).
		std::vector<std::string> appliedNames;
	};

	struct Result
	{
		/// Roots that go through the normal puya backend pipeline to
		/// produce the live `MyContract.approval.bin`. Every method
		/// listed across ALL chunk groups is stubbed in main.
		std::vector<std::shared_ptr<awst::RootNode>> mainRoots;

		/// One chunk per requested group. The chunks are ordered to
		/// match the `_splitGroups` argument order — chunk_i carries
		/// the methods in group i.
		std::vector<Chunk> chunks;
	};

	/// Split `_roots` into a main set + N chunk sets.
	///
	/// `_splitGroups`: list of method-name groups. Each group becomes
	/// one chunk. Names not found in the primary contract are warned
	/// about and dropped. A name appearing in multiple groups is an
	/// error — every split method must belong to exactly one chunk so
	/// the orch's selector→chunk lookup is unambiguous.
	///
	/// Subroutines are duplicated into every chunk's root set
	/// unchanged — the splitter does no call-graph analysis. The size
	/// win is from method bodies, not subroutines.
	static Result split(
		std::vector<std::shared_ptr<awst::RootNode>> const& _roots,
		std::vector<std::set<std::string>> const& _splitGroups);

	/// Per-chunk artifact paths populated by `emitChunkAwsts`.
	struct ChunkPaths
	{
		std::string dir;          // <outputDir>/__uros_split/chunk_<i>/
		std::string awstPath;     // <dir>/awst.json
		std::string optionsPath;  // <dir>/options.json
		std::string contractName; // <PrimaryName>__chunk_<i>; binary file prefix
	};

	/// Write each chunk's awst.json + options.json. Doing this eagerly
	/// (before the main puya pass) lets `--no-puya` callers inspect the
	/// chunk AWSTs and run puya themselves. `_orchAppId` is baked into
	/// the orc-guards as TMPL_UROS_ORCH_APP_ID; pass `0` for first-pass
	/// builds where the orchestrator hasn't been deployed yet.
	///
	/// Returns one ChunkPaths per chunk in `_result.chunks`, in order.
	static std::vector<ChunkPaths> emitChunkAwsts(
		std::string const& _outputDir,
		Result const& _result,
		int _optimizationLevel,
		bool _outputIr,
		int64_t _orchAppId);

	/// Run puya on each chunk's awst.json + write `deploy.uros.json`
	/// (the canonical artifact for the runtime deploy harness — see
	/// `tests/uros-splitter/test_*_dance.py`). Schema:
	///   {
	///     "main_contract":     <bare name>,
	///     "main_approval_hex": "...",
	///     "main_clear_hex":    "...",
	///     "chunks": [
	///       { "name", "methods", "approval_hex", "clear_hex" }, ...
	///     ]
	///   }
	///
	/// Returns puya's exit code from the first chunk that fails, or
	/// 0 on success across the whole batch.
	static int compileChunksAndEmitDeployTemplate(
		std::string const& _outputDir,
		std::string const& _mainBareName,
		Result const& _result,
		std::vector<ChunkPaths> const& _chunkPaths,
		std::string const& _puyaPath,
		std::string const& _logLevel);
};

} // namespace puyasol::splitter
