// SPDX-FileCopyrightText: 2026 Caracu contributors
// SPDX-License-Identifier: GPL-3.0+
#pragma once
#include "common/Pcsx2Types.h"

struct EEDebugStepResult
{
	u32 dispatched_instructions;
	bool cancelled;
	bool event_exit;
};

// CPU thread only, outside Execute(), with VM paused and GS/VU work drained.
// Uses the real interpreter. A branch may dispatch its delay slot as well.
// Other emulated devices can advance at the interpreter's event boundary.
EEDebugStepResult ExecuteEEDebugStep();
