// Generate inline psxMemRead/Write or call them as-is
#define USE_DIRECT_MEM_ACCESS

#define OPCODE(insn, rt, rn, imm) \
	write32((insn) | ((rn) << 21) | ((rt) << 16) | ((imm) & 0xffff))

/* NOTE: psxM must be mmap'ed, not malloc'ed, otherwise segfault */
static int LoadFromConstAddr(u32 insn)
{
	if (IsConst(_Rs_)) {
		u32 addr = iRegs[_Rs_].r + ((s32)(s16)_Imm_);
		/* DEBUGF("known address 0x%x", addr); */
		if ((addr & 0x1fffffff) < 0x200000) {
			u32 rt = _Rt_;
			u32 rs = _Rs_;
			u32 r2 = regMipsToArm(rs, REG_LOAD, REG_REGISTER);
			u32 r1;
			if (rt != rs)
				r1 = regMipsToArm(rt, REG_FIND, REG_REGISTER);
			else
				r1 = r2;
			s32 imm16 = (s32)(s16)_Imm_;
			/* DEBUGF("r1 %d r2 %d imm16 %d", r1, r2, imm16); */
			if (addr < 0x200000) {
				LUI(TEMP_1, 0x1000);
			} else if (addr >= 0xa0000000) {
				LUI(TEMP_1, 0xb000);
			} else {
				LUI(TEMP_1, 0x9000);
			}

			XOR(TEMP_2, TEMP_1, r2);
			OPCODE(insn, r1, TEMP_2, imm16);
			regMipsChanged(rt);
			regBranchUnlock(r1);
			regBranchUnlock(r2);
			SetUndef(_Rt_);
			return 1;
		}
	}

	return 0;
}

static int StoreToConstAddr(u32 insn)
{
	if (IsConst(_Rs_)) {
		u32 addr = iRegs[_Rs_].r + ((s32)(s16)_Imm_);
		/* DEBUGF("known address 0x%x", addr); */
		if ((addr & 0x1fffffff) < 0x200000) {
			u32 rt = _Rt_;
			u32 rs = _Rs_;
			u32 r2 = regMipsToArm(rs, REG_LOAD, REG_REGISTER);
			u32 r1;
			if (rt != rs)
				r1 = regMipsToArm(rt, REG_LOAD, REG_REGISTER);
			else
				r1 = r2;
			s32 imm16 = (s32)(s16)_Imm_;
			/* DEBUGF("r1 %d r2 %d imm16 %d", r1, r2, imm16); */
			if (addr < 0x200000) {
				LUI(TEMP_1, 0x1000);
			} else if (addr >= 0xa0000000) {
				LUI(TEMP_1, 0xb000);
			} else {
				LUI(TEMP_1, 0x9000);
			}

			XOR(TEMP_2, TEMP_1, r2);
			OPCODE(insn, r1, TEMP_2, imm16);
			regBranchUnlock(r1);
			regBranchUnlock(r2);
			return 1;
		}
	}

	return 0;
}

#ifdef USE_DIRECT_MEM_ACCESS
static void LoadFromAddr(u32 insn)
{
	// Rt = [Rs + imm16]
	s32 imm16 = (s32)(s16)_Imm_;
	u32 rs = _Rs_;
	u32 rt = _Rt_;
	u32 *backpatch1, *backpatch2, *backpatch3, *backpatch5, *backpatch6;

	u32 r1 = regMipsToArm(rs, REG_LOAD, REG_REGISTER);
	u32 r2 = regMipsToArm(rt, REG_FIND, REG_REGISTER);
	ADDIU(MIPSREG_A0, r1, imm16);

	SRL(TEMP_1, MIPSREG_A0, 16);
	LI16(TEMP_2, 0x1f80);
	backpatch1 = (u32 *)recMem;
	BEQ(TEMP_1, TEMP_2, 0); // beq temp_1, temp_2, label_1f80
	NOP();

	LI32(TEMP_2, (u32)psxMemRLUT);
	SLL(TEMP_1, TEMP_1, 2);
	ADDU(TEMP_1, TEMP_2, TEMP_1);
	LW(TEMP_1, TEMP_1, 0);

	backpatch3 = (u32 *)recMem;
	BEQZ(TEMP_1, 0); // beqz temp_1, label_exit
	NOP();

	ANDI(TEMP_2, MIPSREG_A0, 0xffff);
	ADDU(TEMP_1, TEMP_1, TEMP_2);
	OPCODE(insn, r2, TEMP_1, 0);

	backpatch2 = (u32 *)recMem;
	B(0); // b label_exit
	NOP();

	// label_1f80:
	fixup_branch(backpatch1);
	ANDI(TEMP_2, MIPSREG_A0, 0xffff);
	SLTIU(TEMP_3, TEMP_2, 0x1000);
	backpatch5 = (u32 *)recMem;
	BEQZ(TEMP_3, 0); // beqz temp_3, label_call_hle
	NOP();

	LI32(TEMP_1, (u32)psxH);
	ADDU(TEMP_1, TEMP_1, TEMP_2);
	OPCODE(insn, r2, TEMP_1, 0);

	backpatch6 = (u32 *)recMem;
	B(0); // b label_exit
	NOP();

	// label_call_hle:
	fixup_branch(backpatch5);
	switch (insn) {
	case 0x80000000: CALLFunc((u32)psxHwRead8); SEB(r2, MIPSREG_V0); break; // LB
	case 0x90000000: CALLFunc((u32)psxHwRead8); MOV(r2, MIPSREG_V0); break; // LBU
	case 0x84000000: CALLFunc((u32)psxHwRead16); SEH(r2, MIPSREG_V0); break; // LH
	case 0x94000000: CALLFunc((u32)psxHwRead16); MOV(r2, MIPSREG_V0); break; // LHU
	case 0x8c000000: CALLFunc((u32)psxHwRead32); MOV(r2, MIPSREG_V0); break; // LW
	}

	// label_exit:
	fixup_branch(backpatch2);
	fixup_branch(backpatch3);
	fixup_branch(backpatch6);

	regMipsChanged(rt);
	regBranchUnlock(r1);
	regBranchUnlock(r2);
}

static void StoreToAddr(u32 insn)
{
	s32 imm16 = (s32)(s16)_Imm_;
	u32 rs = _Rs_;
	u32 rt = _Rt_;
	u32 *backpatch1, *backpatch2, *backpatch3, *backpatch4=0;
	u32 *backpatch5, *backpatch6;

	u32 r1 = regMipsToArm(rs, REG_LOAD, REG_REGISTER);
	u32 r2 = regMipsToArm(rt, REG_LOAD, REG_REGISTER);

	ADDIU(MIPSREG_A0, r1, imm16);
	MOV(MIPSREG_A1, r2);

	SRL(TEMP_1, MIPSREG_A0, 16);
	LI16(TEMP_2, 0x1f80);
	backpatch1 = (u32 *)recMem;
	BEQ(TEMP_1, TEMP_2, 0); // beq temp_1, temp_2, label_1f80
	NOP();

	LI32(TEMP_2, (u32)psxMemWLUT);
	SLL(TEMP_1, TEMP_1, 2);
	ADDU(TEMP_1, TEMP_2, TEMP_1);
	LW(TEMP_1, TEMP_1, 0);

	backpatch3 = (u32 *)recMem;
	BEQZ(TEMP_1, 0); // beqz temp_1, label_exit or label_error
	NOP();

	ANDI(TEMP_2, MIPSREG_A0, 0xffff);
	ADDU(TEMP_1, TEMP_1, TEMP_2);
	OPCODE(insn, r2, TEMP_1, 0);

	SRL(TEMP_1, MIPSREG_A0, 16);
	SLL(TEMP_1, TEMP_1, 2);
	LI32(TEMP_2, (u32)psxRecLUT);
	ADDU(TEMP_1, TEMP_1, TEMP_2);
	LW(TEMP_1, TEMP_1, 0);
	ANDI(TEMP_2, MIPSREG_A0, (~3));
	ADDU(TEMP_1, TEMP_1, TEMP_2);
	SW(0, TEMP_1, 0);

	backpatch2 = (u32 *)recMem;
	B(0); // b label_exit
	NOP();

	/* Generate psxMemWrite32_error() for SW only */
	/* This is needed for cache control in bios */
	// label_error:
	if (insn == 0xac000000) {
		fixup_branch(backpatch3);
		CALLFunc((u32)psxMemWrite32_error);
		backpatch4 = (u32 *)recMem;
		B(0); // b label_exit
		NOP();
	}

	// label_1f80:
	fixup_branch(backpatch1);
	ANDI(TEMP_2, MIPSREG_A0, 0xffff);
	SLTIU(TEMP_3, TEMP_2, 0x1000);
	backpatch5 = (u32 *)recMem;
	BEQZ(TEMP_3, 0); // beqz temp_3, label_call_hle
	NOP();

	LI32(TEMP_1, (u32)psxH);
	ADDU(TEMP_1, TEMP_1, TEMP_2);
	OPCODE(insn, r2, TEMP_1, 0);

	backpatch6 = (u32 *)recMem;
	B(0); // b label_exit
	NOP();

	// label_call_hle:
	fixup_branch(backpatch5);
	switch (insn) {
	case 0xa0000000: CALLFunc((u32)psxHwWrite8); break;
	case 0xa4000000: CALLFunc((u32)psxHwWrite16); break;
	case 0xac000000: CALLFunc((u32)psxHwWrite32); break;
	default: break;
	}

	// label2_exit
	fixup_branch(backpatch2);
	if (insn == 0xac000000)
		fixup_branch(backpatch4);
	else
		fixup_branch(backpatch3);
	fixup_branch(backpatch6);

	regBranchUnlock(r1);
	regBranchUnlock(r2);
}

#else

static void LoadFromAddr(u32 insn)
{
	// Rt = [Rs + imm16]
	s32 imm16 = (s32)(s16)_Imm_;
	u32 rs = _Rs_;
	u32 rt = _Rt_;

	u32 r1 = regMipsToArm(rs, REG_LOAD, REG_REGISTER);
	u32 r2 = regMipsToArm(rt, REG_FIND, REG_REGISTER);

	ADDIU(MIPSREG_A0, r1, imm16);

	switch (insn) {
	case 0x80000000: CALLFunc((u32)psxMemRead8); SEB(r2, MIPSREG_V0); break; // LB
	case 0x90000000: CALLFunc((u32)psxMemRead8); MOV(r2, MIPSREG_V0); break; // LBU
	case 0x84000000: CALLFunc((u32)psxMemRead16); SEH(r2, MIPSREG_V0); break; // LH
	case 0x94000000: CALLFunc((u32)psxMemRead16); MOV(r2, MIPSREG_V0); break; // LHU
	case 0x8c000000: CALLFunc((u32)psxMemRead32); MOV(r2, MIPSREG_V0); break; // LW
	}

	regMipsChanged(rt);
	regBranchUnlock(r1);
	regBranchUnlock(r2);
}

static void StoreToAddr(u32 insn)
{
	// mem[Rs + Im] = Rt
	s32 imm16 = (s32)(s16)_Imm_;
	u32 rs = _Rs_;
	u32 rt = _Rt_;

	u32 r1 = regMipsToArm(rs, REG_LOAD, REG_REGISTER);
	u32 r2 = regMipsToArm(rt, REG_LOAD, REG_REGISTER);

	ADDIU(MIPSREG_A0, r1, imm16);
	MOV(MIPSREG_A1, r2);

	switch (insn) {
	case 0xa0000000: CALLFunc((u32)psxMemWrite8); break;
	case 0xa4000000: CALLFunc((u32)psxMemWrite16); break;
	case 0xac000000: CALLFunc((u32)psxMemWrite32); break;
	default: break;
	}

	regBranchUnlock(r1);
	regBranchUnlock(r2);
}
#endif

static int calc_loads()
{
	int count = 1;
	u32 PC = pc;
	u32 opcode1 = psxRegs.code;
	u32 opcode2 = *(u32 *)((char *)PSXM(PC));

	/* Extra paranoid check if rt == rs */
	if (_fRt_(opcode1) == _fRs_(opcode1))
		return count;

	/* Allow LB, LBU, LH, LHU and LW */
	/* rs should be the same, imm and rt could be different */
	while ((((opcode2 >> 26) == 0x20) || ((opcode2 >> 26) == 0x24) ||
	        ((opcode2 >> 26) == 0x21) || ((opcode2 >> 26) == 0x25) ||
	        ((opcode2 >> 26) == 0x23)) &&
	       (((opcode1 >> 21) & 0x1f) == ((opcode2 >> 21) & 0x1f))) {

		PC += 4;
		count++;

		/* Extra paranoid check if rt == rs */
		if (_fRt_(opcode2) == _fRs_(opcode2))
			return count;

		opcode2 = *(u32 *)((char *)PSXM(PC));
	}

	//if (count > 1)
	//	printf("LOADS found %d at addr %08x\n", count, pc - 4);

	return count;
}

static int calc_stores()
{
	int count = 1;
	u32 PC = pc;
	u32 opcode1 = psxRegs.code;
	u32 opcode2 = *(u32 *)((char *)PSXM(PC));

	/* Allow SB, SH and SW */
	/* rs should be the same, imm and rt could be different */
	while ((((opcode2 >> 26) == 0x28) || ((opcode2 >> 26) == 0x29) ||
	        ((opcode2 >> 26) == 0x2b)) &&
	       (((opcode1 >> 21) & 0x1f) == ((opcode2 >> 21) & 0x1f))) {
		PC += 4;
		count++;
		opcode2 = *(u32 *)((char *)PSXM(PC));
	}

	//if (count > 1)
	//	printf("STORES found %d at addr %08x\n", count, pc - 4);

	return count;
}

static void recLB()
{
	int count = calc_loads();

	// Rt = mem[Rs + Im] (signed)
	if (LoadFromConstAddr(0x80000000))
		return;

	SetUndef(_Rt_);

	LoadFromAddr(0x80000000);
}

static void recLBU()
{
	int count = calc_loads();

	// Rt = mem[Rs + Im] (unsigned)
	if (LoadFromConstAddr(0x90000000))
		return;

	SetUndef(_Rt_);

	LoadFromAddr(0x90000000);
}

static void recLH()
{
	int count = calc_loads();

	// Rt = mem[Rs + Im] (signed)
	if (LoadFromConstAddr(0x84000000))
		return;

	SetUndef(_Rt_);

	LoadFromAddr(0x84000000);
}

static void recLHU()
{
	int count = calc_loads();

	// Rt = mem[Rs + Im] (unsigned)
	if (LoadFromConstAddr(0x94000000))
		return;

	SetUndef(_Rt_);

	LoadFromAddr(0x94000000);
}

static void recLW()
{
	int count = calc_loads();

	// Rt = mem[Rs + Im] (unsigned)
	if (LoadFromConstAddr(0x8c000000))
		return;

	SetUndef(_Rt_);

	LoadFromAddr(0x8c000000);
}

static void recSB()
{
	int count = calc_stores();

	// mem[Rs + Im] = Rt
	if (StoreToConstAddr(0xa0000000))
		return;

	StoreToAddr(0xa0000000);
}

static void recSH()
{
	int count = calc_stores();

	// mem[Rs + Im] = Rt
	if (StoreToConstAddr(0xa4000000))
		return;

	StoreToAddr(0xa4000000);
}

static void recSW()
{
	int count = calc_stores();

	// mem[Rs + Im] = Rt
	if (StoreToConstAddr(0xac000000))
		return;

	StoreToAddr(0xac000000);
}

static u32 LWL_MASK[4] = { 0xffffff, 0xffff, 0xff, 0 };
static u32 LWL_SHIFT[4] = { 24, 16, 8, 0 };
static u32 LWR_MASK[4] = { 0, 0xff000000, 0xffff0000, 0xffffff00 };
static u32 LWR_SHIFT[4] = { 0, 8, 16, 24 };

static void gen_LWL_LWR(int count)
{
	int icount = count;
	u32 rs = _Rs_;
	s32 imm16 = (s32)(s16)_Imm_;
	u32 r1 = regMipsToArm(rs, REG_LOAD, REG_REGISTER);
	u32 PC = pc - 4;
	u32 *backpatch1, *backpatch2;

	if (!count)
		printf("Sole LWL or LWR opcode at addr %08x\n", PC);

	#ifdef WITH_DISASM
	for (int i = 0; i < count*2-1; i++)
		DISASM_PSX(pc + i * 4);
	#endif

	ADDIU(MIPSREG_A0, r1, imm16);
	EXT(TEMP_1, MIPSREG_A0, 0, 0x1d); // and 0x1fffffff
	LUI(TEMP_2, 0x80);
	SLTU(TEMP_3, TEMP_1, TEMP_2);
	backpatch1 = (u32 *)recMem;
	BEQZ(TEMP_3, 0); // beqz temp_2, label_hle
	NOP();

	EXT(TEMP_1, r1, 0, 0x15); // and 0x1fffff

	if ((u32)psxM == 0x10000000)
		LUI(TEMP_2, 0x1000);
	else
		LW(TEMP_2, PERM_REG_1, off(psxM));

	ADDU(TEMP_2, TEMP_2, TEMP_1);

	do {
		u32 LWL = *(u32 *)((char *)PSXM(PC));
		u32 LWR = *(u32 *)((char *)PSXM(PC + 4));
		s32 imm_LWL = (s16)(LWL & 0xffff);
		s32 imm_LWR = (s16)(LWR & 0xffff);
		u32 rt = (LWL >> 16) & 0x1f;
		u32 r2 = regMipsToArm(rt, REG_FIND, REG_REGISTER);

		OPCODE((LWL & 0xfc000000), r2, TEMP_2, imm_LWL);
		if (icount)
			OPCODE((LWR & 0xfc000000), r2, TEMP_2, imm_LWR);

		SetUndef(rt);
		regMipsChanged(rt);
		regBranchUnlock(r2);

		PC += (icount ? 8 : 4);
		if (!icount) break;
	} while (--icount);

	backpatch2 = (u32 *)recMem;
	B(0); // b label_exit
	NOP();

	count *= 2;
	PC = pc - 4;

	// label_hle:
	fixup_branch(backpatch1);

	do {
		u32 opcode = *(u32 *)((char *)PSXM(PC));
		u32 insn = opcode & 0xfc000000;
		u32 rt = ((opcode >> 16) & 0x1f);
		u32 imm16 = (s32)(s16)(opcode & 0xffff);
		u32 r2 = regMipsToArm(rt, REG_FIND, REG_REGISTER);

		ADDIU(MIPSREG_A0, r1, imm16); // a0 = r1 & ~3
		SRL(MIPSREG_A0, MIPSREG_A0, 2);
		SLL(MIPSREG_A0, MIPSREG_A0, 2);
		CALLFunc((u32)psxMemRead32); // result in MIPSREG_V0

		ADDIU(TEMP_1, r1, imm16);
		ANDI(TEMP_1, TEMP_1, 3); // shift = addr & 3
		SLL(TEMP_1, TEMP_1, 2);

		if (insn == 0x88000000) // LWL
			LI32(TEMP_2, (u32)LWL_MASK);
		else			// LWR
			LI32(TEMP_2, (u32)LWR_MASK);

		ADDU(TEMP_2, TEMP_2, TEMP_1);
		LW(TEMP_2, TEMP_2, 0);
		AND(r2, r2, TEMP_2);

		if (insn == 0x88000000) // LWL
			LI32(TEMP_2, (u32)LWL_SHIFT);
		else			// LWR
			LI32(TEMP_2, (u32)LWR_SHIFT);

		ADDU(TEMP_2, TEMP_2, TEMP_1);
		LW(TEMP_2, TEMP_2, 0);

		if (insn == 0x88000000) // LWL
			SLLV(TEMP_3, MIPSREG_V0, TEMP_2);
		else			// LWR
			SRLV(TEMP_3, MIPSREG_V0, TEMP_2);

		OR(r2, r2, TEMP_3);

		SetUndef(rt);
		regMipsChanged(rt);
		regBranchUnlock(r2);

		PC += 4;
		if (!count) break;
	} while (--count);

	// label_exit:
	fixup_branch(backpatch2);

	pc = PC;
	regBranchUnlock(r1);
}

static u32 SWL_MASK[4] = { 0xffffff00, 0xffff0000, 0xff000000, 0 };
static u32 SWL_SHIFT[4] = { 24, 16, 8, 0 };
static u32 SWR_MASK[4] = { 0, 0xff, 0xffff, 0xffffff };
static u32 SWR_SHIFT[4] = { 0, 8, 16, 24 };

static void gen_SWL_SWR(int count)
{
	int icount = count;
	u32 rs = _Rs_;
	s32 imm16 = (s32)(s16)_Imm_;
	u32 r1 = regMipsToArm(rs, REG_LOAD, REG_REGISTER);
	u32 PC = pc - 4;
	u32 *backpatch1, *backpatch2, *backpatch3;

	if (!count)
		printf("Sole SWL or SWR opcode at addr %08x\n", PC);

	#ifdef WITH_DISASM
	for (int i = 0; i < count*2-1; i++)
		DISASM_PSX(pc + i * 4);
	#endif

	/* First check if memory is writable atm */
	LW(TEMP_1, PERM_REG_1, off(writeok));
	backpatch3 = (u32 *)recMem;
	BEQZ(TEMP_1, 0); // beqz temp_1, label_hle
	NOP();

	ADDIU(MIPSREG_A0, r1, imm16);
	EXT(TEMP_1, MIPSREG_A0, 0, 0x1d); // and 0x1fffffff
	LUI(TEMP_2, 0x80);
	SLTU(TEMP_3, TEMP_1, TEMP_2);
	backpatch1 = (u32 *)recMem;
	BEQZ(TEMP_3, 0); // beqz temp_2, label_hle
	NOP();

	EXT(TEMP_1, r1, 0, 0x15); // and 0x1fffff

	if ((u32)psxM == 0x10000000)
		LUI(TEMP_2, 0x1000);
	else
		LW(TEMP_2, PERM_REG_1, off(psxM));

	ADDU(TEMP_2, TEMP_2, TEMP_1);

	do {
		u32 SWL = *(u32 *)((char *)PSXM(PC));
		u32 SWR = *(u32 *)((char *)PSXM(PC + 4));
		s32 imm_SWL = (s16)(SWL & 0xffff);
		s32 imm_SWR = (s16)(SWR & 0xffff);
		u32 rt = (SWL >> 16) & 0x1f;
		u32 r2 = regMipsToArm(rt, REG_LOAD, REG_REGISTER);

		OPCODE((SWL & 0xfc000000), r2, TEMP_2, imm_SWL);
		if (icount)
			OPCODE((SWR & 0xfc000000), r2, TEMP_2, imm_SWR);

		regBranchUnlock(r2);

		PC += (icount ? 8 : 4);
		if (!icount) break;
	} while (--icount);

	backpatch2 = (u32 *)recMem;
	B(0); // b label_exit
	NOP();

	count *= 2;
	PC = pc - 4;

	// label_hle:
	fixup_branch(backpatch1);
	fixup_branch(backpatch3);

	do {
		u32 opcode = *(u32 *)((char *)PSXM(PC));
		u32 insn = opcode & 0xfc000000;
		u32 rt = ((opcode >> 16) & 0x1f);
		u32 imm16 = (s32)(s16)(opcode & 0xffff);
		u32 r2 = regMipsToArm(rt, REG_LOAD, REG_REGISTER);

		ADDIU(MIPSREG_A0, r1, imm16); // a0 = r1 & ~3
		SRL(MIPSREG_A0, MIPSREG_A0, 2);
		SLL(MIPSREG_A0, MIPSREG_A0, 2);
		CALLFunc((u32)psxMemRead32); // result in MIPSREG_V0

		ADDIU(MIPSREG_A0, r1, imm16); // a0 = r1 & ~3
		SRL(MIPSREG_A0, MIPSREG_A0, 2);
		SLL(MIPSREG_A0, MIPSREG_A0, 2);

		ADDIU(TEMP_1, r1, imm16);
		ANDI(TEMP_1, TEMP_1, 3); // shift = addr & 3
		SLL(TEMP_1, TEMP_1, 2);

		if (insn == 0xa8000000) // SWL
			LI32(TEMP_2, (u32)SWL_MASK);
		else			// SWR
			LI32(TEMP_2, (u32)SWR_MASK);

		ADDU(TEMP_2, TEMP_2, TEMP_1);
		LW(TEMP_2, TEMP_2, 0);
		AND(MIPSREG_A1, MIPSREG_V0, TEMP_2);

		if (insn == 0xa8000000) // SWL
			LI32(TEMP_2, (u32)SWL_SHIFT);
		else			// SWR
			LI32(TEMP_2, (u32)SWR_SHIFT);

		ADDU(TEMP_2, TEMP_2, TEMP_1);
		LW(TEMP_2, TEMP_2, 0);

		if (insn == 0xa8000000) // SWL
			SRLV(TEMP_3, r2, TEMP_2);
		else			// SWR
			SLLV(TEMP_3, r2, TEMP_2);

		OR(MIPSREG_A1, MIPSREG_A1, TEMP_3);

		CALLFunc((u32)psxMemWrite32);

		regBranchUnlock(r2);

		PC += 4;
		if (!count) break;
	} while (--count);

	// label_exit:
	fixup_branch(backpatch2);

	pc = PC;
	regBranchUnlock(r1);
}

/* Calculate LWL/LWR or SWL/SWR opcode pairs
   return number of pairs or 0 if single opcode is found */
static int calc_pairs()
{
	int count = 1;
	u32 PC = pc - 4;
	u32 LWL = *(u32 *)((char *)PSXM(PC));
	u32 LWR = *(u32 *)((char *)PSXM(PC + 4));
	u32 nextLWL = *(u32 *)((char *)PSXM(PC + 8));
	u32 nextLWR = *(u32 *)((char *)PSXM(PC + 12));

	/* Check if LWL/SWL is followed by LWR/SWR because the latter could be
	   placed in delay slot after jal/jalr/jr */
	if ((LWL >> 26) == 0x22 && (LWR >> 26) != 0x26) {
		printf("LWL is not followed by LWR at addr %08x\n", PC);
		return 0;
	}

	if ((LWL >> 26) == 0x2a && (LWR >> 26) != 0x2e) {
		printf("SWL is not followed by SWR at addr %08x\n", PC);
		return 0;
	}

	/* LWL/SWL has different rt than LWR/SWR, should never happen in fact */
	if (((LWL >> 16) & 0x1f) != ((LWR >> 16) & 0x1f)) {
		printf("LWL/SWL and LWR/SWR target reg don't match at addr %08x!\n", PC);
		return 0;
	}

	PC += 16;

	/* opcode and base must coincide, rt and imm could be any */
	while (((LWL & 0xffe00000) == (nextLWL & 0xffe00000)) &&
	       ((LWR & 0xffe00000) == (nextLWR & 0xffe00000))) {
		count++;
		nextLWL = *(u32 *)((char *)PSXM(PC));
		nextLWR = *(u32 *)((char *)PSXM(PC + 4));
		PC += 8;
	}

	return count;
}

static void recLWL()
{
	int count = calc_pairs();

	if (IsConst(_Rs_)) {
		u32 addr = iRegs[_Rs_].r + ((s32)(s16)_Imm_);
		if ((addr & 0x1fffffff) < 0x200000) {
			u32 rs = _Rs_;
			u32 r2 = regMipsToArm(rs, REG_LOAD, REG_REGISTER);
			u32 PC = pc - 4;

			#ifdef WITH_DISASM
			for (int i = 0; i < count*2-1; i++)
				DISASM_PSX(pc + i * 4);
			#endif

			if (addr < 0x200000) {
				LUI(TEMP_1, 0x1000);
			} else if (addr >= 0xa0000000) {
				LUI(TEMP_1, 0xb000);
			} else {
				LUI(TEMP_1, 0x9000);
			}

			XOR(TEMP_2, TEMP_1, r2);

			do {
				u32 LWL = *(u32 *)((char *)PSXM(PC));
				u32 LWR = *(u32 *)((char *)PSXM(PC + 4));
				s32 imm_LWL = (s16)(LWL & 0xffff);
				s32 imm_LWR = (s16)(LWR & 0xffff);
				u32 rt = (LWL >> 16) & 0x1f;
				u32 r1;

				if (rt != rs)
					r1 = regMipsToArm(rt, REG_FIND, REG_REGISTER);
				else
					r1 = r2;

				LWL(r1, TEMP_2, imm_LWL);
				if (count)
					LWR(r1, TEMP_2, imm_LWR);

				SetUndef(rt);
				regMipsChanged(rt);
				regBranchUnlock(r1);
				PC += (count ? 8 : 4);
				if (!count) break;
			} while (--count);

			pc = PC;
			regBranchUnlock(r2);
			return;
		}
	}

	/* FIXME: Weird bugs if count > 1 in Metal Slug X */
	if (count > 1) count = 1;

	gen_LWL_LWR(count);
}

static void recLWR()
{
	SetUndef(_Rt_);

	gen_LWL_LWR(0); // 0 - sole inst
}

static void recSWL()
{
	int count = calc_pairs();

	if (IsConst(_Rs_)) {
		u32 addr = iRegs[_Rs_].r + ((s32)(s16)_Imm_);
		if ((addr & 0x1fffffff) < 0x200000) {
			u32 rs = _Rs_;
			u32 r2 = regMipsToArm(rs, REG_LOAD, REG_REGISTER);
			u32 PC = pc - 4;

			#ifdef WITH_DISASM
			for (int i = 0; i < count*2-1; i++)
				DISASM_PSX(pc + i * 4);
			#endif

			if (addr < 0x200000) {
				LUI(TEMP_1, 0x1000);
			} else if (addr >= 0xa0000000) {
				LUI(TEMP_1, 0xb000);
			} else {
				LUI(TEMP_1, 0x9000);
			}

			XOR(TEMP_2, TEMP_1, r2);

			do {
				u32 SWL = *(u32 *)((char *)PSXM(PC));
				u32 SWR = *(u32 *)((char *)PSXM(PC + 4));
				s32 imm_SWL = (s16)(SWL & 0xffff);
				s32 imm_SWR = (s16)(SWR & 0xffff);
				u32 rt = (SWL >> 16) & 0x1f;
				u32 r1;

				if (rt != rs)
					r1 = regMipsToArm(rt, REG_LOAD, REG_REGISTER);
				else
					r1 = r2;

				SWL(r1, TEMP_2, imm_SWL);
				SWR(r1, TEMP_2, imm_SWR);

				regBranchUnlock(r1);
				PC += (count ? 8 : 4);
				if (!count) break;
			} while (--count);

			pc = PC;
			regBranchUnlock(r2);
			return;
		}
	}

	gen_SWL_SWR(count);
}

static void recSWR()
{
	gen_SWL_SWR(0); // 0 - sole inst
}
