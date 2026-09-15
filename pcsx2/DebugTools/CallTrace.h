// SPDX-FileCopyrightText: 2026 Caracu contributors
// SPDX-License-Identifier: GPL-3.0+
#pragma once
#include "common/Pcsx2Types.h"

#include <vector>

// Aggregated dynamic EE control-transfer graph: (origin PC, target, kind, word) -> count.
// Off by default. While off, the recompiler emits nothing extra and the interpreter
// only tests one flag. Everything here runs on the EE/CPU thread; there is no locking.
namespace CallTrace
{
	enum class Kind : u8
	{
		Jal = 0,
		Jalr = 1,
		J = 2,
		Jr = 3, // JR with rs != ra only; returns (jr ra) are not recorded.
	};

	struct Edge
	{
		u32 from; // PC of the jump instruction itself (not of its delay slot)
		u32 to; // target PC
		u32 word; // instruction word that was executed/compiled at `from`
		Kind kind;
		u64 count;
	};

	// Read by the EE recompiler when a block is compiled, and by the interpreter per
	// executed jump. Change it only with the VM paused, then clear CPU execution caches.
	extern bool g_recording;
	// Scratch slot written by recompiled code immediately before a Record* call.
	extern u32 g_emit_word;

	void Start(bool clear);
	void Stop();
	u64 Events();
	size_t EdgeCount();
	std::vector<Edge> Snapshot(); // sorted by from, to, kind, word
	const char* KindName(Kind kind);

	void Record(Kind kind, u32 from, u32 to, u32 word);

	// Entry points for recompiled code; the word comes from g_emit_word.
	void RecordJ(u32 from, u32 to);
	void RecordJal(u32 from, u32 to);
	// Register jumps read the already-stored target from cpuRegs.pc.
	void RecordJrFromPC(u32 from);
	void RecordJalrFromPC(u32 from);
} // namespace CallTrace
