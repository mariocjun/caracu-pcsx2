// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"
#include "R5900OpcodeTables.h"
#include "DebugTools/CallTrace.h"
#include "x86/iR5900.h"

using namespace x86Emitter;

namespace R5900::Dynarec::OpcodeImpl
{

/*********************************************************
* Jump to target                                         *
* Format:  OP target                                     *
*********************************************************/
#ifndef JUMP_RECOMPILE

namespace Interp = R5900::Interpreter::OpcodeImpl;

REC_SYS(J);
REC_SYS_DEL(JAL, 31);
REC_SYS(JR);
REC_SYS_DEL(JALR, _Rd_);

#else

// Call graph recording (DebugTools/CallTrace.h). Emitted only while recording was
// enabled when the block was compiled, right after the delay slot and before the
// block-ending branch, so a delay-slot exception does not record a jump that did
// not happen. With recording off, the generated code is unchanged.
static void EmitCallTraceImm(void (*record)(u32, u32), u32 from, u32 word, u32 target)
{
	iFlushCall(FLUSH_EVERYTHING);
	xMOV(ptr32[&CallTrace::g_emit_word], word);
	xFastCall(record, from, target);
}

// Register jumps: the target is in eax, as SetBranchReg() expects. Store it in
// cpuRegs.pc first (SetBranchReg does the same), because flushing may use rax.
static void EmitCallTraceReg(void (*record)(u32), u32 from, u32 word)
{
	xMOV(ptr32[&cpuRegs.pc], eax);
	iFlushCall(FLUSH_EVERYTHING);
	xMOV(ptr32[&CallTrace::g_emit_word], word);
	xFastCall(record, from);
	xMOV(eax, ptr32[&cpuRegs.pc]);
}

////////////////////////////////////////////////////
void recJ()
{
	EE::Profiler.EmitOp(eeOpcode::J);
	// pc already points at the delay slot while the jump itself is recompiled.
	const u32 trace_from = pc - 4;
	const u32 trace_word = cpuRegs.code;

	// SET_FPUSTATE;
	u32 newpc = (_InstrucTarget_ << 2) + (pc & 0xf0000000);
	recompileNextInstruction(true, false);
	if (CallTrace::g_recording)
		EmitCallTraceImm(CallTrace::RecordJ, trace_from, trace_word, newpc);
	if (EmuConfig.Gamefixes.GoemonTlbHack)
		SetBranchImm(vtlb_V2P(newpc));
	else
		SetBranchImm(newpc);
}

////////////////////////////////////////////////////
void recJAL()
{
	EE::Profiler.EmitOp(eeOpcode::JAL);
	const u32 trace_from = pc - 4;
	const u32 trace_word = cpuRegs.code;

	u32 newpc = (_InstrucTarget_ << 2) + (pc & 0xf0000000);
	_deleteEEreg(31, 0);
	if (EE_CONST_PROP)
	{
		GPR_SET_CONST(31);
		g_cpuConstRegs[31].UL[0] = pc + 4;
		g_cpuConstRegs[31].UL[1] = 0;
	}
	else
	{
		xMOV(ptr32[&cpuRegs.GPR.r[31].UL[0]], pc + 4);
		xMOV(ptr32[&cpuRegs.GPR.r[31].UL[1]], 0);
	}

	recompileNextInstruction(true, false);
	if (CallTrace::g_recording)
		EmitCallTraceImm(CallTrace::RecordJal, trace_from, trace_word, newpc);
	if (EmuConfig.Gamefixes.GoemonTlbHack)
		SetBranchImm(vtlb_V2P(newpc));
	else
		SetBranchImm(newpc);
}

/*********************************************************
* Register jump                                          *
* Format:  OP rs, rd                                     *
*********************************************************/

////////////////////////////////////////////////////
void recJR()
{
	EE::Profiler.EmitOp(eeOpcode::JR);
	const u32 trace_from = pc - 4;
	const u32 trace_word = cpuRegs.code;
	// Returns (jr ra) are not recorded.
	const bool trace = CallTrace::g_recording && _Rs_ != 31;

	const bool swap = EmuConfig.Gamefixes.GoemonTlbHack ? false : TrySwapDelaySlot(_Rs_, 0, 0, true);
	if (!swap)
	{
		const int wbreg = _allocX86reg(X86TYPE_PCWRITEBACK, 0, MODE_WRITE | MODE_CALLEESAVED);
		_eeMoveGPRtoR(xRegister32(wbreg), _Rs_);

		if (EmuConfig.Gamefixes.GoemonTlbHack)
		{
			xMOV(ecx, xRegister32(wbreg));
			vtlb_DynV2P();
			xMOV(xRegister32(wbreg), eax);
		}

		recompileNextInstruction(true, false);

		// the next instruction may have flushed the register.. so reload it if so.
		if (x86regs[wbreg].inuse && x86regs[wbreg].type == X86TYPE_PCWRITEBACK)
		{
			xMOV(eax, xRegister32(wbreg));
			x86regs[wbreg].inuse = 0;
		}
		else
		{
			xMOV(eax, ptr[&cpuRegs.pcWriteback]);
		}
	}
	else
	{
		if (GPR_IS_DIRTY_CONST(_Rs_) || _hasX86reg(X86TYPE_GPR, _Rs_, 0))
		{
			const int x86reg = _allocX86reg(X86TYPE_GPR, _Rs_, MODE_READ);
			xMOV(eax, xRegister32(x86reg));
		}
		else
		{
			_eeMoveGPRtoR(eax, _Rs_);
		}
	}


	// Target passed in eax
	if (trace)
		EmitCallTraceReg(CallTrace::RecordJrFromPC, trace_from, trace_word);
	SetBranchReg();
}

////////////////////////////////////////////////////
void recJALR()
{
	EE::Profiler.EmitOp(eeOpcode::JALR);
	const u32 trace_from = pc - 4;
	const u32 trace_word = cpuRegs.code;
	const bool trace = CallTrace::g_recording;

	const u32 newpc = pc + 4;
	const bool swap = (EmuConfig.Gamefixes.GoemonTlbHack || _Rd_ == _Rs_) ? false : TrySwapDelaySlot(_Rs_, 0, _Rd_, true);


	int wbreg = -1;
	if (!swap)
	{
		wbreg = _allocX86reg(X86TYPE_PCWRITEBACK, 0, MODE_WRITE | MODE_CALLEESAVED);
		_eeMoveGPRtoR(xRegister32(wbreg), _Rs_);

		if (EmuConfig.Gamefixes.GoemonTlbHack)
		{
			xMOV(ecx, xRegister32(wbreg));
			vtlb_DynV2P();
			xMOV(xRegister32(wbreg), eax);
		}
	}

	if (_Rd_)
	{
		_deleteEEreg(_Rd_, 0);
		if (EE_CONST_PROP)
		{
			GPR_SET_CONST(_Rd_);
			g_cpuConstRegs[_Rd_].UD[0] = newpc;
		}
		else
		{
			xWriteImm64ToMem(&cpuRegs.GPR.r[_Rd_].UD[0], rax, newpc);
		}
	}

	if (!swap)
	{
		recompileNextInstruction(true, false);

		// the next instruction may have flushed the register.. so reload it if so.
		if (x86regs[wbreg].inuse && x86regs[wbreg].type == X86TYPE_PCWRITEBACK)
		{
			xMOV(eax, xRegister32(wbreg));
			x86regs[wbreg].inuse = 0;
		}
		else
		{
			xMOV(eax, ptr[&cpuRegs.pcWriteback]);
		}
	}
	else
	{
		if (GPR_IS_DIRTY_CONST(_Rs_) || _hasX86reg(X86TYPE_GPR, _Rs_, 0))
		{
			const int x86reg = _allocX86reg(X86TYPE_GPR, _Rs_, MODE_READ);
			xMOV(eax, xRegister32(x86reg));
		}
		else
		{
			_eeMoveGPRtoR(eax, _Rs_);
		}
	}

	// Target passed in eax
	if (trace)
		EmitCallTraceReg(CallTrace::RecordJalrFromPC, trace_from, trace_word);
	SetBranchReg();
}

#endif

} // namespace R5900::Dynarec::OpcodeImpl
