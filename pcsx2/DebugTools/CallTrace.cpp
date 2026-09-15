// SPDX-FileCopyrightText: 2026 Caracu contributors
// SPDX-License-Identifier: GPL-3.0+

#include "DebugTools/CallTrace.h"
#include "R5900.h"

#include <algorithm>
#include <tuple>

namespace CallTrace
{
	bool g_recording = false;
	u32 g_emit_word = 0;
} // namespace CallTrace

namespace
{
	struct Slot
	{
		u32 from;
		u32 to;
		u32 word;
		u8 kind;
		bool used;
		u64 count;
	};

	constexpr size_t INITIAL_SLOTS = size_t(1) << 16;

	std::vector<Slot> s_slots;
	size_t s_used = 0;
	u64 s_events = 0;

	size_t Hash(u32 from, u32 to, u32 word, u8 kind)
	{
		u64 h = (static_cast<u64>(from) << 32 | to) * 0x9E3779B97F4A7C15ull;
		h ^= (static_cast<u64>(word) << 8 | kind) * 0xC2B2AE3D27D4EB4Full;
		return static_cast<size_t>(h ^ (h >> 31));
	}

	void Grow()
	{
		std::vector<Slot> old = std::move(s_slots);
		s_slots.assign(old.empty() ? INITIAL_SLOTS : old.size() * 2, Slot{});
		const size_t mask = s_slots.size() - 1;
		for (const Slot& slot : old)
		{
			if (!slot.used)
				continue;
			size_t index = Hash(slot.from, slot.to, slot.word, slot.kind) & mask;
			while (s_slots[index].used)
				index = (index + 1) & mask;
			s_slots[index] = slot;
		}
	}
} // namespace

void CallTrace::Start(bool clear)
{
	if (clear)
	{
		s_slots.clear();
		s_slots.shrink_to_fit();
		s_used = 0;
		s_events = 0;
	}
	g_recording = true;
}

void CallTrace::Stop()
{
	g_recording = false;
}

u64 CallTrace::Events()
{
	return s_events;
}

size_t CallTrace::EdgeCount()
{
	return s_used;
}

const char* CallTrace::KindName(Kind kind)
{
	switch (kind)
	{
		case Kind::Jal:
			return "jal";
		case Kind::Jalr:
			return "jalr";
		case Kind::J:
			return "j";
		case Kind::Jr:
			return "jr";
	}
	return "unknown";
}

void CallTrace::Record(Kind kind, u32 from, u32 to, u32 word)
{
	if (!g_recording)
		return;
	const u8 k = static_cast<u8>(kind);
	// Keep the load factor at or below one half so probing stays short.
	if ((s_used + 1) * 2 > s_slots.size())
		Grow();
	const size_t mask = s_slots.size() - 1;
	size_t index = Hash(from, to, word, k) & mask;
	for (;;)
	{
		Slot& slot = s_slots[index];
		if (!slot.used)
		{
			slot = Slot{from, to, word, k, true, 1};
			++s_used;
			++s_events;
			return;
		}
		if (slot.from == from && slot.to == to && slot.word == word && slot.kind == k)
		{
			++slot.count;
			++s_events;
			return;
		}
		index = (index + 1) & mask;
	}
}

std::vector<CallTrace::Edge> CallTrace::Snapshot()
{
	std::vector<Edge> edges;
	edges.reserve(s_used);
	for (const Slot& slot : s_slots)
	{
		if (slot.used)
			edges.push_back(Edge{slot.from, slot.to, slot.word, static_cast<Kind>(slot.kind), slot.count});
	}
	std::sort(edges.begin(), edges.end(), [](const Edge& a, const Edge& b) {
		return std::tie(a.from, a.to, a.kind, a.word) < std::tie(b.from, b.to, b.kind, b.word);
	});
	return edges;
}

void CallTrace::RecordJ(u32 from, u32 to)
{
	Record(Kind::J, from, to, g_emit_word);
}

void CallTrace::RecordJal(u32 from, u32 to)
{
	Record(Kind::Jal, from, to, g_emit_word);
}

void CallTrace::RecordJrFromPC(u32 from)
{
	Record(Kind::Jr, from, cpuRegs.pc, g_emit_word);
}

void CallTrace::RecordJalrFromPC(u32 from)
{
	Record(Kind::Jalr, from, cpuRegs.pc, g_emit_word);
}
