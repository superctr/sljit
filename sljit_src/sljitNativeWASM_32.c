/*
 *    Stack-less Just-In-Time compiler
 *
 *    Copyright Zoltan Herczeg (hzmester@freemail.hu). All rights reserved.
 *    WebAssembly backend copyright 2026 Ian Karlsson.
 *
 * Redistribution and use in source and binary forms, with or without modification, are
 * permitted provided that the following conditions are met:
 *
 *   1. Redistributions of source code must retain the above copyright notice, this list of
 *      conditions and the following disclaimer.
 *
 *   2. Redistributions in binary form must reproduce the above copyright notice, this list
 *      of conditions and the following disclaimer in the documentation and/or other materials
 *      provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDER(S) AND CONTRIBUTORS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT
 * SHALL THE COPYRIGHT HOLDER(S) OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
 * TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
   WebAssembly (wasm32 memory, 64 bit machine word).

   The instructions are recorded as they are emitted, and sljit_generate_code
   builds one module of them.  The module imports the memory ("e" "m"), the
   function table ("e" "t") and the table index of its first slot ("e" "b"),
   and its element segment fills its slots: the entry, which has the C type
   given to sljit_emit_enter, then one thunk for each label.

   The body of the function takes the state (the label to start at) and every
   register, and returns a word; every function compiled has this shape, so a
   jump to the address of a label in another function is a tail call of that
   label's thunk, which passes the registers on.  Inside the body, a loop
   holds one block for each label, so that a label is where its block ends: a
   jump forward leaves the blocks up to its label, a jump back sets the state
   and starts the loop again, whose head dispatches on the state.

   The status flags are the operands and the result of the last operation
   which set them, kept in locals; a condition reads them as that operation
   defines it.

   Not supported: floating point, SIMD, atomics, locals on the stack
   (local_size and SLJIT_SP), fast calls, constants and rewritable jumps.
*/

SLJIT_API_FUNC_ATTRIBUTE const char* sljit_get_platform_name(void)
{
	return "WebAssembly" SLJIT_CPUINFO;
}

#define WASM_REGS SLJIT_NUMBER_OF_REGISTERS

/* Local indices of the body. */
#define L_STATE	0
#define L_FA	(WASM_REGS + 1)
#define L_FB	(WASM_REGS + 2)
#define L_FR	(WASM_REGS + 3)
#define L_T0	(WASM_REGS + 4)
#define L_T1	(WASM_REGS + 5)
#define L_A0	(WASM_REGS + 6)
#define L_A1	(WASM_REGS + 7)

/* Function and type indices of the module. */
#define F_BODY		0
#define F_ENTRY		1
#define F_THUNK0	2
#define T_BODY		0
#define T_THUNK		1
#define T_ENTRY		2
#define T_CALL0		3

/* Recorded instruction kinds. */
#define K_OP0		1
#define K_OP1		2
#define K_OP2		3
#define K_OP2R		4
#define K_OP_FLAGS	5
#define K_SELECT	6
#define K_JUMP		7
#define K_IJUMP		8
#define K_ICALL		9
#define K_RETURN	10
#define K_PREFETCH	11

struct wasm_ins {
	sljit_s32 kind;
	sljit_s32 op;
	sljit_s32 dst;
	sljit_s32 src1;
	sljit_s32 src2;
	/* The operation which set the flags this instruction reads. */
	sljit_s32 flags;
	/* A condition, or the argument types of a call. */
	sljit_s32 aux;
	sljit_sw dstw;
	sljit_sw src1w;
	sljit_sw src2w;
	struct sljit_jump *jump;
};

/* --------------------------------------------------------------------- */
/*  Recording                                                            */
/* --------------------------------------------------------------------- */

static sljit_s32 wasm_unsupported(struct sljit_compiler *compiler)
{
	compiler->error = SLJIT_ERR_UNSUPPORTED;
	return SLJIT_ERR_UNSUPPORTED;
}

static sljit_s32 wasm_bad_operand(sljit_s32 p)
{
	if (!(p & SLJIT_MEM))
		return p == SLJIT_SP || (p > WASM_REGS && p != SLJIT_IMM);
	return (p & REG_MASK) == SLJIT_SP || OFFS_REG(p) == SLJIT_SP
		|| (p & REG_MASK) > WASM_REGS || OFFS_REG(p) > WASM_REGS;
}

static struct wasm_ins* push_ins(struct sljit_compiler *compiler, sljit_s32 kind, sljit_s32 op,
	sljit_s32 dst, sljit_sw dstw,
	sljit_s32 src1, sljit_sw src1w,
	sljit_s32 src2, sljit_sw src2w)
{
	struct wasm_ins *ins;

	if (wasm_bad_operand(dst) || wasm_bad_operand(src1) || wasm_bad_operand(src2)) {
		wasm_unsupported(compiler);
		return NULL;
	}

	ins = (struct wasm_ins*)ensure_buf(compiler, sizeof(struct wasm_ins));
	if (!ins)
		return NULL;
	ins->kind = kind;
	ins->op = op;
	ins->dst = dst;
	ins->dstw = dstw;
	ins->src1 = src1;
	ins->src1w = src1w;
	ins->src2 = src2;
	ins->src2w = src2w;
	ins->flags = compiler->wasm_flags;
	ins->aux = 0;
	ins->jump = NULL;
	compiler->size++;
	return ins;
}

#define PUSH(kind, op, dst, dstw, src1, src1w, src2, src2w) \
	FAIL_IF(!push_ins(compiler, (kind), (op), (dst), (dstw), (src1), (src1w), (src2), (src2w)))

SLJIT_API_FUNC_ATTRIBUTE sljit_s32 sljit_has_cpu_feature(sljit_s32 feature_type)
{
	switch (feature_type) {
	case SLJIT_HAS_CLZ:
	case SLJIT_HAS_CTZ:
	case SLJIT_HAS_REV:
	case SLJIT_HAS_ROT:
	case SLJIT_HAS_CMOV:
		return 1;
	default:
		return 0;
	}
}

SLJIT_API_FUNC_ATTRIBUTE sljit_s32 sljit_cmp_info(sljit_s32 type)
{
	SLJIT_UNUSED_ARG(type);
	return 0;
}

SLJIT_API_FUNC_ATTRIBUTE sljit_s32 sljit_emit_enter(struct sljit_compiler *compiler,
	sljit_s32 options, sljit_s32 arg_types,
	sljit_s32 scratches, sljit_s32 saveds, sljit_s32 local_size)
{
	CHECK_ERROR();
	CHECK(check_sljit_emit_enter(compiler, options, arg_types, scratches, saveds, local_size));
	set_emit_enter(compiler, options, arg_types, scratches, saveds, local_size);

	compiler->wasm_args = arg_types;
	compiler->wasm_flags = 0;
	if (local_size > 0)
		return wasm_unsupported(compiler);
	return SLJIT_SUCCESS;
}

SLJIT_API_FUNC_ATTRIBUTE sljit_s32 sljit_set_context(struct sljit_compiler *compiler,
	sljit_s32 options, sljit_s32 arg_types,
	sljit_s32 scratches, sljit_s32 saveds, sljit_s32 local_size)
{
	CHECK_ERROR();
	CHECK(check_sljit_set_context(compiler, options, arg_types, scratches, saveds, local_size));
	set_emit_enter(compiler, options, arg_types, scratches, saveds, local_size);

	compiler->wasm_args = arg_types;
	if (local_size > 0)
		return wasm_unsupported(compiler);
	return SLJIT_SUCCESS;
}

SLJIT_API_FUNC_ATTRIBUTE sljit_s32 sljit_emit_return_void(struct sljit_compiler *compiler)
{
	CHECK_ERROR();
	CHECK(check_sljit_emit_return_void(compiler));

	PUSH(K_RETURN, 0, 0, 0, 0, 0, 0, 0);
	return SLJIT_SUCCESS;
}

SLJIT_API_FUNC_ATTRIBUTE sljit_s32 sljit_emit_return_to(struct sljit_compiler *compiler,
	sljit_s32 src, sljit_sw srcw)
{
	CHECK_ERROR();
	CHECK(check_sljit_emit_return_to(compiler, src, srcw));
	return wasm_unsupported(compiler);
}

SLJIT_API_FUNC_ATTRIBUTE sljit_s32 sljit_emit_op0(struct sljit_compiler *compiler, sljit_s32 op)
{
	CHECK_ERROR();
	CHECK(check_sljit_emit_op0(compiler, op));

	switch (GET_OPCODE(op)) {
	case SLJIT_DIVMOD_UW:
	case SLJIT_DIVMOD_SW:
	case SLJIT_DIV_UW:
	case SLJIT_DIV_SW:
		PUSH(K_OP0, op, 0, 0, 0, 0, 0, 0);
		return SLJIT_SUCCESS;
	case SLJIT_LMUL_UW:
	case SLJIT_LMUL_SW:
		return wasm_unsupported(compiler);
	default:
		return SLJIT_SUCCESS;
	}
}

SLJIT_API_FUNC_ATTRIBUTE sljit_s32 sljit_emit_op1(struct sljit_compiler *compiler, sljit_s32 op,
	sljit_s32 dst, sljit_sw dstw,
	sljit_s32 src, sljit_sw srcw)
{
	CHECK_ERROR();
	CHECK(check_sljit_emit_op1(compiler, op, dst, dstw, src, srcw));

	if (GET_OPCODE(op) >= SLJIT_MOV && GET_OPCODE(op) <= SLJIT_MOV_P && !dst)
		return SLJIT_SUCCESS;
	PUSH(K_OP1, op, dst, dstw, src, srcw, 0, 0);
	return SLJIT_SUCCESS;
}

SLJIT_API_FUNC_ATTRIBUTE sljit_s32 sljit_emit_op2(struct sljit_compiler *compiler, sljit_s32 op,
	sljit_s32 dst, sljit_sw dstw,
	sljit_s32 src1, sljit_sw src1w,
	sljit_s32 src2, sljit_sw src2w)
{
	sljit_s32 opcode = GET_OPCODE(op);

	CHECK_ERROR();
	CHECK(check_sljit_emit_op2(compiler, op, 0, dst, dstw, src1, src1w, src2, src2w));

	if (HAS_FLAGS(op) && (opcode == SLJIT_ADDC || opcode == SLJIT_SUBC
			|| (opcode == SLJIT_MUL && (op & VARIABLE_FLAG_MASK))))
		return wasm_unsupported(compiler);

	PUSH(K_OP2, op, dst, dstw, src1, src1w, src2, src2w);
	if (HAS_FLAGS(op))
		compiler->wasm_flags = op;
	return SLJIT_SUCCESS;
}

SLJIT_API_FUNC_ATTRIBUTE sljit_s32 sljit_emit_op2u(struct sljit_compiler *compiler, sljit_s32 op,
	sljit_s32 src1, sljit_sw src1w,
	sljit_s32 src2, sljit_sw src2w)
{
	CHECK_ERROR();
	CHECK(check_sljit_emit_op2(compiler, op, 1, 0, 0, src1, src1w, src2, src2w));

	SLJIT_SKIP_CHECKS(compiler);
	return sljit_emit_op2(compiler, op, 0, 0, src1, src1w, src2, src2w);
}

SLJIT_API_FUNC_ATTRIBUTE sljit_s32 sljit_emit_op2r(struct sljit_compiler *compiler, sljit_s32 op,
	sljit_s32 dst_reg,
	sljit_s32 src1, sljit_sw src1w,
	sljit_s32 src2, sljit_sw src2w)
{
	CHECK_ERROR();
	CHECK(check_sljit_emit_op2r(compiler, op, dst_reg, src1, src1w, src2, src2w));

	if (GET_OPCODE(op) != SLJIT_MULADD)
		return wasm_unsupported(compiler);
	PUSH(K_OP2R, op, dst_reg, 0, src1, src1w, src2, src2w);
	return SLJIT_SUCCESS;
}

SLJIT_API_FUNC_ATTRIBUTE sljit_s32 sljit_emit_op2_shift(struct sljit_compiler *compiler, sljit_s32 op,
	sljit_s32 dst, sljit_sw dstw,
	sljit_s32 src1, sljit_sw src1w,
	sljit_s32 src2, sljit_sw src2w,
	sljit_sw shift_arg)
{
	SLJIT_UNUSED_ARG(op);
	SLJIT_UNUSED_ARG(dst);
	SLJIT_UNUSED_ARG(dstw);
	SLJIT_UNUSED_ARG(src1);
	SLJIT_UNUSED_ARG(src1w);
	SLJIT_UNUSED_ARG(src2);
	SLJIT_UNUSED_ARG(src2w);
	SLJIT_UNUSED_ARG(shift_arg);
	CHECK_ERROR();
	return wasm_unsupported(compiler);
}

SLJIT_API_FUNC_ATTRIBUTE sljit_s32 sljit_emit_shift_into(struct sljit_compiler *compiler, sljit_s32 op,
	sljit_s32 dst_reg,
	sljit_s32 src1_reg,
	sljit_s32 src2_reg,
	sljit_s32 src3, sljit_sw src3w)
{
	CHECK_ERROR();
	CHECK(check_sljit_emit_shift_into(compiler, op, dst_reg, src1_reg, src2_reg, src3, src3w));
	return wasm_unsupported(compiler);
}

SLJIT_API_FUNC_ATTRIBUTE sljit_s32 sljit_emit_op_src(struct sljit_compiler *compiler, sljit_s32 op,
	sljit_s32 src, sljit_sw srcw)
{
	CHECK_ERROR();
	CHECK(check_sljit_emit_op_src(compiler, op, src, srcw));

	switch (op) {
	case SLJIT_PREFETCH_L1:
	case SLJIT_PREFETCH_L2:
	case SLJIT_PREFETCH_L3:
	case SLJIT_PREFETCH_ONCE:
		return SLJIT_SUCCESS;
	default:
		return wasm_unsupported(compiler);
	}
}

SLJIT_API_FUNC_ATTRIBUTE sljit_s32 sljit_emit_op_dst(struct sljit_compiler *compiler, sljit_s32 op,
	sljit_s32 dst, sljit_sw dstw)
{
	CHECK_ERROR();
	CHECK(check_sljit_emit_op_dst(compiler, op, dst, dstw));
	return wasm_unsupported(compiler);
}

SLJIT_API_FUNC_ATTRIBUTE sljit_s32 sljit_get_register_index(sljit_s32 type, sljit_s32 reg)
{
	CHECK_REG_INDEX(check_sljit_get_register_index(type, reg));
	return type == SLJIT_GP_REGISTER ? reg : -1;
}

SLJIT_API_FUNC_ATTRIBUTE sljit_s32 sljit_emit_op_custom(struct sljit_compiler *compiler,
	void *instruction, sljit_u32 size)
{
	CHECK_ERROR();
	CHECK(check_sljit_emit_op_custom(compiler, instruction, size));
	return wasm_unsupported(compiler);
}

SLJIT_API_FUNC_ATTRIBUTE sljit_s32 sljit_emit_fop1(struct sljit_compiler *compiler, sljit_s32 op,
	sljit_s32 dst, sljit_sw dstw,
	sljit_s32 src, sljit_sw srcw)
{
	SLJIT_UNUSED_ARG(op);
	SLJIT_UNUSED_ARG(dst);
	SLJIT_UNUSED_ARG(dstw);
	SLJIT_UNUSED_ARG(src);
	SLJIT_UNUSED_ARG(srcw);
	CHECK_ERROR();
	return wasm_unsupported(compiler);
}

SLJIT_API_FUNC_ATTRIBUTE sljit_s32 sljit_emit_fop2(struct sljit_compiler *compiler, sljit_s32 op,
	sljit_s32 dst, sljit_sw dstw,
	sljit_s32 src1, sljit_sw src1w,
	sljit_s32 src2, sljit_sw src2w)
{
	SLJIT_UNUSED_ARG(op);
	SLJIT_UNUSED_ARG(dst);
	SLJIT_UNUSED_ARG(dstw);
	SLJIT_UNUSED_ARG(src1);
	SLJIT_UNUSED_ARG(src1w);
	SLJIT_UNUSED_ARG(src2);
	SLJIT_UNUSED_ARG(src2w);
	CHECK_ERROR();
	return wasm_unsupported(compiler);
}

SLJIT_API_FUNC_ATTRIBUTE sljit_s32 sljit_emit_fset32(struct sljit_compiler *compiler,
	sljit_s32 freg, sljit_f32 value)
{
	SLJIT_UNUSED_ARG(freg);
	SLJIT_UNUSED_ARG(value);
	CHECK_ERROR();
	return wasm_unsupported(compiler);
}

SLJIT_API_FUNC_ATTRIBUTE sljit_s32 sljit_emit_fset64(struct sljit_compiler *compiler,
	sljit_s32 freg, sljit_f64 value)
{
	SLJIT_UNUSED_ARG(freg);
	SLJIT_UNUSED_ARG(value);
	CHECK_ERROR();
	return wasm_unsupported(compiler);
}

SLJIT_API_FUNC_ATTRIBUTE sljit_s32 sljit_emit_fcopy(struct sljit_compiler *compiler, sljit_s32 op,
	sljit_s32 freg, sljit_s32 reg)
{
	SLJIT_UNUSED_ARG(op);
	SLJIT_UNUSED_ARG(freg);
	SLJIT_UNUSED_ARG(reg);
	CHECK_ERROR();
	return wasm_unsupported(compiler);
}

SLJIT_API_FUNC_ATTRIBUTE sljit_s32 sljit_emit_fselect(struct sljit_compiler *compiler, sljit_s32 type,
	sljit_s32 dst_freg,
	sljit_s32 src1, sljit_sw src1w,
	sljit_s32 src2_freg)
{
	SLJIT_UNUSED_ARG(type);
	SLJIT_UNUSED_ARG(dst_freg);
	SLJIT_UNUSED_ARG(src1);
	SLJIT_UNUSED_ARG(src1w);
	SLJIT_UNUSED_ARG(src2_freg);
	CHECK_ERROR();
	return wasm_unsupported(compiler);
}

SLJIT_API_FUNC_ATTRIBUTE struct sljit_label* sljit_emit_label(struct sljit_compiler *compiler)
{
	struct sljit_label *label;

	CHECK_ERROR_PTR();
	CHECK_PTR(check_sljit_emit_label(compiler));

	if (compiler->last_label && compiler->last_label->size == compiler->size)
		return compiler->last_label;

	label = (struct sljit_label*)ensure_abuf(compiler, sizeof(struct sljit_label));
	PTR_FAIL_IF(!label);
	set_label(label, compiler);
	return label;
}

SLJIT_API_FUNC_ATTRIBUTE struct sljit_label* sljit_emit_aligned_label(struct sljit_compiler *compiler,
	sljit_s32 alignment, struct sljit_read_only_buffer *buffers)
{
	SLJIT_UNUSED_ARG(alignment);

	CHECK_ERROR_PTR();
	CHECK_PTR(check_sljit_emit_aligned_label(compiler, alignment, buffers));

	if (buffers) {
		wasm_unsupported(compiler);
		return NULL;
	}

	SLJIT_SKIP_CHECKS(compiler);
	return sljit_emit_label(compiler);
}

SLJIT_API_FUNC_ATTRIBUTE struct sljit_jump* sljit_emit_jump(struct sljit_compiler *compiler, sljit_s32 type)
{
	struct sljit_jump *jump;
	struct wasm_ins *ins;

	CHECK_ERROR_PTR();
	CHECK_PTR(check_sljit_emit_jump(compiler, type));

	if ((type & SLJIT_REWRITABLE_JUMP) || (type & 0xff) == SLJIT_FAST_CALL
			|| ((type & 0xff) > SLJIT_NOT_CARRY && (type & 0xff) < SLJIT_JUMP)) {
		wasm_unsupported(compiler);
		return NULL;
	}

	jump = (struct sljit_jump*)ensure_abuf(compiler, sizeof(struct sljit_jump));
	PTR_FAIL_IF(!jump);
	set_jump(jump, compiler, 0);
	jump->addr = compiler->size;

	ins = push_ins(compiler, K_JUMP, type, 0, 0, 0, 0, 0, 0);
	PTR_FAIL_IF(!ins);
	ins->jump = jump;
	return jump;
}

SLJIT_API_FUNC_ATTRIBUTE struct sljit_jump* sljit_emit_call(struct sljit_compiler *compiler, sljit_s32 type,
	sljit_s32 arg_types)
{
	struct sljit_jump *jump;
	struct wasm_ins *ins;

	CHECK_ERROR_PTR();
	CHECK_PTR(check_sljit_emit_call(compiler, type, arg_types));

	if (type & SLJIT_REWRITABLE_JUMP) {
		wasm_unsupported(compiler);
		return NULL;
	}

	jump = (struct sljit_jump*)ensure_abuf(compiler, sizeof(struct sljit_jump));
	PTR_FAIL_IF(!jump);
	set_jump(jump, compiler, 0);
	jump->addr = compiler->size;

	ins = push_ins(compiler, K_JUMP, type, 0, 0, 0, 0, 0, 0);
	PTR_FAIL_IF(!ins);
	ins->jump = jump;
	ins->aux = arg_types;
	compiler->wasm_flags = 0;
	return jump;
}

SLJIT_API_FUNC_ATTRIBUTE sljit_s32 sljit_emit_ijump(struct sljit_compiler *compiler, sljit_s32 type, sljit_s32 src, sljit_sw srcw)
{
	CHECK_ERROR();
	CHECK(check_sljit_emit_ijump(compiler, type, src, srcw));

	if (type != SLJIT_JUMP)
		return wasm_unsupported(compiler);
	PUSH(K_IJUMP, type, 0, 0, src, srcw, 0, 0);
	return SLJIT_SUCCESS;
}

SLJIT_API_FUNC_ATTRIBUTE sljit_s32 sljit_emit_icall(struct sljit_compiler *compiler, sljit_s32 type,
	sljit_s32 arg_types,
	sljit_s32 src, sljit_sw srcw)
{
	struct wasm_ins *ins;

	CHECK_ERROR();
	CHECK(check_sljit_emit_icall(compiler, type, arg_types, src, srcw));

	ins = push_ins(compiler, K_ICALL, type, 0, 0, src, srcw, 0, 0);
	FAIL_IF(!ins);
	ins->aux = arg_types;
	compiler->wasm_flags = 0;
	return SLJIT_SUCCESS;
}

SLJIT_API_FUNC_ATTRIBUTE sljit_s32 sljit_emit_op_flags(struct sljit_compiler *compiler, sljit_s32 op,
	sljit_s32 dst, sljit_sw dstw,
	sljit_s32 type)
{
	struct wasm_ins *ins;

	CHECK_ERROR();
	CHECK(check_sljit_emit_op_flags(compiler, op, dst, dstw, type));

	if ((type & 0xff) > SLJIT_NOT_CARRY)
		return wasm_unsupported(compiler);
	ins = push_ins(compiler, K_OP_FLAGS, op, dst, dstw, 0, 0, 0, 0);
	FAIL_IF(!ins);
	ins->aux = type;
	if (HAS_FLAGS(op))
		compiler->wasm_flags = op;
	return SLJIT_SUCCESS;
}

SLJIT_API_FUNC_ATTRIBUTE sljit_s32 sljit_emit_select(struct sljit_compiler *compiler, sljit_s32 type,
	sljit_s32 dst_reg,
	sljit_s32 src1, sljit_sw src1w,
	sljit_s32 src2_reg)
{
	CHECK_ERROR();
	CHECK(check_sljit_emit_select(compiler, type, dst_reg, src1, src1w, src2_reg));

	if ((type & 0xff) > SLJIT_NOT_CARRY)
		return wasm_unsupported(compiler);
	PUSH(K_SELECT, type, dst_reg, 0, src1, src1w, src2_reg, 0);
	return SLJIT_SUCCESS;
}

SLJIT_API_FUNC_ATTRIBUTE sljit_s32 sljit_emit_mem(struct sljit_compiler *compiler, sljit_s32 type,
	sljit_s32 reg,
	sljit_s32 mem, sljit_sw memw)
{
	CHECK_ERROR();
	CHECK(check_sljit_emit_mem(compiler, type, reg, mem, memw));
	return wasm_unsupported(compiler);
}

SLJIT_API_FUNC_ATTRIBUTE sljit_s32 sljit_emit_atomic_load(struct sljit_compiler *compiler, sljit_s32 op,
	sljit_s32 dst_reg,
	sljit_s32 mem_reg)
{
	SLJIT_UNUSED_ARG(op);
	SLJIT_UNUSED_ARG(dst_reg);
	SLJIT_UNUSED_ARG(mem_reg);
	CHECK_ERROR();
	return wasm_unsupported(compiler);
}

SLJIT_API_FUNC_ATTRIBUTE sljit_s32 sljit_emit_atomic_store(struct sljit_compiler *compiler, sljit_s32 op,
	sljit_s32 src_reg,
	sljit_s32 mem_reg,
	sljit_s32 temp_reg)
{
	SLJIT_UNUSED_ARG(op);
	SLJIT_UNUSED_ARG(src_reg);
	SLJIT_UNUSED_ARG(mem_reg);
	SLJIT_UNUSED_ARG(temp_reg);
	CHECK_ERROR();
	return wasm_unsupported(compiler);
}

SLJIT_API_FUNC_ATTRIBUTE struct sljit_const* sljit_emit_const(struct sljit_compiler *compiler, sljit_s32 op,
	sljit_s32 dst, sljit_sw dstw,
	sljit_sw init_value)
{
	SLJIT_UNUSED_ARG(op);
	SLJIT_UNUSED_ARG(dst);
	SLJIT_UNUSED_ARG(dstw);
	SLJIT_UNUSED_ARG(init_value);
	CHECK_ERROR_PTR();
	wasm_unsupported(compiler);
	return NULL;
}

SLJIT_API_FUNC_ATTRIBUTE struct sljit_jump* sljit_emit_op_addr(struct sljit_compiler *compiler, sljit_s32 op,
	sljit_s32 dst, sljit_sw dstw)
{
	SLJIT_UNUSED_ARG(op);
	SLJIT_UNUSED_ARG(dst);
	SLJIT_UNUSED_ARG(dstw);
	CHECK_ERROR_PTR();
	wasm_unsupported(compiler);
	return NULL;
}

SLJIT_API_FUNC_ATTRIBUTE void sljit_set_jump_addr(sljit_uw addr, sljit_uw new_target, sljit_sw executable_offset)
{
	SLJIT_UNUSED_ARG(addr);
	SLJIT_UNUSED_ARG(new_target);
	SLJIT_UNUSED_ARG(executable_offset);
	SLJIT_UNREACHABLE();
}

SLJIT_API_FUNC_ATTRIBUTE void sljit_set_const(sljit_uw addr, sljit_s32 op, sljit_sw new_constant, sljit_sw executable_offset)
{
	SLJIT_UNUSED_ARG(addr);
	SLJIT_UNUSED_ARG(op);
	SLJIT_UNUSED_ARG(new_constant);
	SLJIT_UNUSED_ARG(executable_offset);
	SLJIT_UNREACHABLE();
}

/* --------------------------------------------------------------------- */
/*  Loading                                                              */
/* --------------------------------------------------------------------- */

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

/* Compiles the module, reserves `slots` table slots for it and instantiates it
   there; returns the first slot, or 0. */
EM_JS(sljit_s32, sljit_wasm_load, (const sljit_u8 *code, sljit_u32 size, sljit_s32 slots), {
	var s = Module.sljitSlots || (Module.sljitSlots = { free: {}, size: {} });
	var table = wasmExports['__indirect_function_table'];
	var list = s.free[slots];
	var base = (list && list.length) ? list.pop() : table.grow(slots);
	try {
		var m = new WebAssembly.Module(HEAPU8.subarray(code, code + size));
		new WebAssembly.Instance(m, { e: { m: wasmExports['memory'], t: table,
			b: new WebAssembly.Global({ value: 'i32', mutable: false }, base) } });
	} catch (e) {
		err('sljit: ' + e);
		(s.free[slots] || (s.free[slots] = [])).push(base);
		return 0;
	}
	s.size[base] = slots;
	return base;
});

EM_JS(void, sljit_wasm_unload, (sljit_s32 base), {
	var s = Module.sljitSlots;
	var n = s && s.size[base];
	if (!n)
		return;
	delete s.size[base];
	for (var i = 0; i < n; i++)
		wasmExports['__indirect_function_table'].set(base + i, null);
	(s.free[n] || (s.free[n] = [])).push(base);
});
#else /* !__EMSCRIPTEN__ */
/* Supplied by the host. */
extern sljit_s32 sljit_wasm_load(const sljit_u8 *code, sljit_u32 size, sljit_s32 slots);
extern void sljit_wasm_unload(sljit_s32 base);
#endif /* __EMSCRIPTEN__ */

SLJIT_API_FUNC_ATTRIBUTE void sljit_wasm_free_code(void *code)
{
	sljit_wasm_unload((sljit_s32)(sljit_uw)code);
}

/* --------------------------------------------------------------------- */
/*  Encoding                                                             */
/* --------------------------------------------------------------------- */

struct wasm_buf {
	sljit_u8 *data;
	sljit_uw size;
	sljit_uw cap;
	sljit_s32 failed;
	void *allocator_data;
};

static void b_init(struct wasm_buf *b, void *allocator_data)
{
	b->data = NULL;
	b->size = 0;
	b->cap = 0;
	b->failed = 0;
	b->allocator_data = allocator_data;
}

static void b_free(struct wasm_buf *b)
{
	if (b->data)
		SLJIT_FREE(b->data, b->allocator_data);
	b->data = NULL;
	b->size = 0;
	b->cap = 0;
}

static sljit_u8* b_reserve(struct wasm_buf *b, sljit_uw n)
{
	sljit_u8 *p;
	sljit_uw cap;

	if (b->failed)
		return NULL;
	if (b->size + n > b->cap) {
		cap = b->cap ? b->cap * 2 : 4096;
		while (cap < b->size + n)
			cap *= 2;
		p = (sljit_u8*)SLJIT_MALLOC(cap, b->allocator_data);
		if (!p) {
			b->failed = 1;
			return NULL;
		}
		if (b->data) {
			SLJIT_MEMCPY(p, b->data, b->size);
			SLJIT_FREE(b->data, b->allocator_data);
		}
		b->data = p;
		b->cap = cap;
	}
	p = b->data + b->size;
	b->size += n;
	return p;
}

static void b_byte(struct wasm_buf *b, sljit_u8 v)
{
	sljit_u8 *p = b_reserve(b, 1);
	if (p)
		*p = v;
}

static void b_bytes(struct wasm_buf *b, const void *src, sljit_uw n)
{
	sljit_u8 *p = b_reserve(b, n);
	if (p && n)
		SLJIT_MEMCPY(p, src, n);
}

static void b_uleb(struct wasm_buf *b, sljit_uw v)
{
	do {
		sljit_u8 byte = (sljit_u8)(v & 0x7f);
		v >>= 7;
		b_byte(b, v ? (sljit_u8)(byte | 0x80) : byte);
	} while (v);
}

static void b_sleb(struct wasm_buf *b, sljit_sw v)
{
	for (;;) {
		sljit_u8 byte = (sljit_u8)(v & 0x7f);
		v >>= 7;
		if ((v == 0 && !(byte & 0x40)) || (v == -1 && (byte & 0x40))) {
			b_byte(b, byte);
			return;
		}
		b_byte(b, (sljit_u8)(byte | 0x80));
	}
}

static void b_name(struct wasm_buf *b, const char *s)
{
	sljit_uw n = 0;
	while (s[n])
		n++;
	b_uleb(b, n);
	b_bytes(b, s, n);
}

/* A section of `body`, which is freed. */
static void b_section(struct wasm_buf *b, sljit_u8 id, struct wasm_buf *body)
{
	b_byte(b, id);
	b_uleb(b, body->size);
	b_bytes(b, body->data, body->size);
	if (body->failed)
		b->failed = 1;
	b_free(body);
}

/* Opcodes. */
#define OP_UNREACHABLE	0x00
#define OP_BLOCK	0x02
#define OP_LOOP		0x03
#define OP_IF		0x04
#define OP_END		0x0b
#define OP_BR		0x0c
#define OP_BR_IF	0x0d
#define OP_BR_TABLE	0x0e
#define OP_RETURN	0x0f
#define OP_CALL		0x10
#define OP_CALL_INDIRECT	0x11
#define OP_RETURN_CALL	0x12
#define OP_RETURN_CALL_INDIRECT	0x13
#define OP_DROP		0x1a
#define OP_SELECT	0x1b
#define OP_LOCAL_GET	0x20
#define OP_LOCAL_SET	0x21
#define OP_LOCAL_TEE	0x22
#define OP_GLOBAL_GET	0x23
#define OP_I32_LOAD	0x28
#define OP_I64_LOAD	0x29
#define OP_I32_LOAD8_S	0x2c
#define OP_I32_LOAD8_U	0x2d
#define OP_I32_LOAD16_S	0x2e
#define OP_I32_LOAD16_U	0x2f
#define OP_I64_LOAD8_S	0x30
#define OP_I64_LOAD8_U	0x31
#define OP_I64_LOAD16_S	0x32
#define OP_I64_LOAD16_U	0x33
#define OP_I64_LOAD32_S	0x34
#define OP_I64_LOAD32_U	0x35
#define OP_I32_STORE	0x36
#define OP_I64_STORE	0x37
#define OP_I32_STORE8	0x3a
#define OP_I32_STORE16	0x3b
#define OP_I64_STORE8	0x3c
#define OP_I64_STORE16	0x3d
#define OP_I64_STORE32	0x3e
#define OP_I32_CONST	0x41
#define OP_I64_CONST	0x42
#define OP_I32_EQZ	0x45
#define OP_I64_EQZ	0x50

/* i32 and i64 forms of the integer operations: the i64 one is OP_64(x). */
#define OP_EQ		0x46
#define OP_NE		0x47
#define OP_LT_S		0x48
#define OP_LT_U		0x49
#define OP_GT_S		0x4a
#define OP_GT_U		0x4b
#define OP_LE_S		0x4c
#define OP_LE_U		0x4d
#define OP_GE_S		0x4e
#define OP_GE_U		0x4f
#define CMP_64(op)	((sljit_u8)((op) + 0x0b))

#define OP_CLZ		0x67
#define OP_CTZ		0x68
#define OP_ADD		0x6a
#define OP_SUB		0x6b
#define OP_MUL		0x6c
#define OP_DIV_S	0x6d
#define OP_DIV_U	0x6e
#define OP_REM_S	0x6f
#define OP_REM_U	0x70
#define OP_AND		0x71
#define OP_OR		0x72
#define OP_XOR		0x73
#define OP_SHL		0x74
#define OP_SHR_S	0x75
#define OP_SHR_U	0x76
#define OP_ROTL		0x77
#define OP_ROTR		0x78
#define ALU_64(op)	((sljit_u8)((op) + 0x12))

#define OP_I32_WRAP_I64		0xa7
#define OP_I64_EXTEND_I32_S	0xac
#define OP_I64_EXTEND_I32_U	0xad
#define OP_I32_EXTEND8_S	0xc0
#define OP_I32_EXTEND16_S	0xc1
#define OP_I64_EXTEND8_S	0xc2
#define OP_I64_EXTEND16_S	0xc3
#define OP_I64_EXTEND32_S	0xc4

#define TYPE_I32	0x7f
#define TYPE_I64	0x7e
#define TYPE_FUNC	0x60
#define TYPE_FUNCREF	0x70
#define BLOCK_EMPTY	0x40

/* --------------------------------------------------------------------- */
/*  Code generation                                                      */
/* --------------------------------------------------------------------- */

struct wasm_gen {
	struct sljit_compiler *compiler;
	struct wasm_buf *b;
	/* Labels, and the segment (the code after a label) being emitted. */
	sljit_s32 labels;
	sljit_s32 seg;
	/* Blocks opened inside the segment. */
	sljit_s32 inner;
	/* The signatures of the calls, after the fixed types. */
	sljit_s32 *calls;
	sljit_s32 call_count;
	sljit_s32 call_cap;
};

static void g_op(struct wasm_gen *g, sljit_u8 op)
{
	b_byte(g->b, op);
}

static void g_local(struct wasm_gen *g, sljit_u8 op, sljit_s32 idx)
{
	b_byte(g->b, op);
	b_uleb(g->b, (sljit_uw)idx);
}

static void g_i32(struct wasm_gen *g, sljit_s32 v)
{
	b_byte(g->b, OP_I32_CONST);
	b_sleb(g->b, v);
}

static void g_i64(struct wasm_gen *g, sljit_sw v)
{
	b_byte(g->b, OP_I64_CONST);
	b_sleb(g->b, v);
}

static void g_mem(struct wasm_gen *g, sljit_u8 op, sljit_uw offset)
{
	b_byte(g->b, op);
	b_byte(g->b, 0);
	b_uleb(g->b, offset);
}

static sljit_s32 g_call_type(struct wasm_gen *g, sljit_s32 arg_types)
{
	sljit_s32 i;
	sljit_s32 *p;

	for (i = 0; i < g->call_count; i++)
		if (g->calls[i] == arg_types)
			return T_CALL0 + i;

	if (g->call_count == g->call_cap) {
		g->call_cap = g->call_cap ? g->call_cap * 2 : 8;
		p = (sljit_s32*)SLJIT_MALLOC((sljit_uw)g->call_cap * sizeof(sljit_s32), g->compiler->allocator_data);
		if (!p) {
			g->b->failed = 1;
			return T_CALL0;
		}
		if (g->calls) {
			SLJIT_MEMCPY(p, g->calls, (sljit_uw)g->call_count * sizeof(sljit_s32));
			SLJIT_FREE(g->calls, g->compiler->allocator_data);
		}
		g->calls = p;
	}
	g->calls[g->call_count] = arg_types;
	return T_CALL0 + g->call_count++;
}

/* Pushes the address of a memory operand; returns the offset for the access. */
static sljit_uw g_addr(struct wasm_gen *g, sljit_s32 p, sljit_sw w)
{
	sljit_s32 base = p & REG_MASK;

	if (!base) {
		g_i32(g, (sljit_s32)w);
		return 0;
	}

	g_local(g, OP_LOCAL_GET, base);
	g_op(g, OP_I32_WRAP_I64);

	if (p & OFFS_REG_MASK) {
		g_local(g, OP_LOCAL_GET, OFFS_REG(p));
		g_op(g, OP_I32_WRAP_I64);
		if (w & 0x3) {
			g_i32(g, (sljit_s32)(w & 0x3));
			g_op(g, OP_SHL);
		}
		g_op(g, OP_ADD);
		return 0;
	}

	if (w >= 0 && w <= 0x7fffffff)
		return (sljit_uw)w;
	g_i32(g, (sljit_s32)w);
	g_op(g, OP_ADD);
	return 0;
}

static void g_load(struct wasm_gen *g, sljit_u8 op, sljit_s32 p, sljit_sw w)
{
	sljit_uw offset = g_addr(g, p, w);
	g_mem(g, op, offset);
}

/* Pushes an operand as an i64 (is32 == 0) or as an i32. */
static void g_src(struct wasm_gen *g, sljit_s32 p, sljit_sw w, sljit_s32 is32)
{
	if (p == SLJIT_IMM) {
		if (is32)
			g_i32(g, (sljit_s32)w);
		else
			g_i64(g, w);
		return;
	}
	if (p & SLJIT_MEM) {
		g_load(g, is32 ? OP_I32_LOAD : OP_I64_LOAD, p, w);
		return;
	}
	g_local(g, OP_LOCAL_GET, p);
	if (is32)
		g_op(g, OP_I32_WRAP_I64);
}

/* Stores the value on the stack (an i64, or an i32 if is32) into an operand;
   a register takes a 32 bit value zero extended.  store is the opcode of a
   memory store. */
static void g_dst(struct wasm_gen *g, sljit_s32 p, sljit_sw w, sljit_s32 is32, sljit_u8 store)
{
	sljit_uw offset;

	if (!p) {
		g_op(g, OP_DROP);
		return;
	}
	if (!(p & SLJIT_MEM)) {
		if (is32)
			g_op(g, OP_I64_EXTEND_I32_U);
		g_local(g, OP_LOCAL_SET, p);
		return;
	}
	g_local(g, OP_LOCAL_SET, is32 ? L_A0 : L_T0);
	offset = g_addr(g, p, w);
	g_local(g, OP_LOCAL_GET, is32 ? L_A0 : L_T0);
	g_mem(g, store, offset);
}

/* Pushes (as an i32, 0 or 1) a comparison of two i64 locals of the given width. */
static void g_compare(struct wasm_gen *g, sljit_s32 a, sljit_s32 b, sljit_s32 cond, sljit_s32 is32)
{
	sljit_u8 op;

	switch (cond & ~1) {
	case SLJIT_LESS: op = OP_LT_U; break;
	case SLJIT_GREATER: op = OP_GT_U; break;
	case SLJIT_SIG_LESS: op = OP_LT_S; break;
	case SLJIT_SIG_GREATER: op = OP_GT_S; break;
	default: op = OP_EQ; break;
	}

	g_local(g, OP_LOCAL_GET, a);
	if (is32)
		g_op(g, OP_I32_WRAP_I64);
	g_local(g, OP_LOCAL_GET, b);
	if (is32)
		g_op(g, OP_I32_WRAP_I64);
	g_op(g, is32 ? op : CMP_64(op));
	if (cond & 1)
		g_op(g, OP_I32_EQZ);
}

/* Pushes (as an i32, 0 or 1) a condition on the flags set by `flags`. */
static void g_cond(struct wasm_gen *g, sljit_s32 flags, sljit_s32 cond)
{
	sljit_s32 is32 = flags & SLJIT_32;
	sljit_s32 opcode = GET_OPCODE(flags);
	sljit_s32 add = (opcode == SLJIT_ADD || opcode == SLJIT_ADDC);

	cond &= 0xff;
	if (!flags) {
		g_i32(g, 0);
		return;
	}

	switch (cond & ~1) {
	case SLJIT_EQUAL:
		g_local(g, OP_LOCAL_GET, L_FR);
		if (is32) {
			g_op(g, OP_I32_WRAP_I64);
			g_op(g, OP_I32_EQZ);
		} else
			g_op(g, OP_I64_EQZ);
		if (cond & 1)
			g_op(g, OP_I32_EQZ);
		return;
	case SLJIT_OVERFLOW:
		/* add: (a ^ r) & (b ^ r) < 0; sub: (a ^ b) & (a ^ r) < 0 */
		g_local(g, OP_LOCAL_GET, add ? L_FR : L_FB);
		g_local(g, OP_LOCAL_GET, L_FA);
		g_op(g, ALU_64(OP_XOR));
		g_local(g, OP_LOCAL_GET, add ? L_FB : L_FA);
		g_local(g, OP_LOCAL_GET, L_FR);
		g_op(g, ALU_64(OP_XOR));
		g_op(g, ALU_64(OP_AND));
		if (is32) {
			g_op(g, OP_I32_WRAP_I64);
			g_i32(g, 0);
			g_op(g, OP_LT_S);
		} else {
			g_i64(g, 0);
			g_op(g, CMP_64(OP_LT_S));
		}
		if (cond & 1)
			g_op(g, OP_I32_EQZ);
		return;
	case SLJIT_CARRY:
		/* add: r < a (unsigned); sub: a < b (unsigned) */
		if (add)
			g_compare(g, L_FR, L_FA, SLJIT_LESS | (cond & 1), is32);
		else
			g_compare(g, L_FA, L_FB, SLJIT_LESS | (cond & 1), is32);
		return;
	default:
		g_compare(g, L_FA, L_FB, cond, is32);
		return;
	}
}

/* A jump to a table slot (a label's thunk): every register passed on. */
static void g_tail_call_slot(struct wasm_gen *g)
{
	sljit_s32 r;

	for (r = 1; r <= WASM_REGS; r++)
		g_local(g, OP_LOCAL_GET, r);
	g_local(g, OP_LOCAL_GET, L_A0);
	g_op(g, OP_RETURN_CALL_INDIRECT);
	b_uleb(g->b, T_THUNK);
	b_uleb(g->b, 0);
}

/* A jump to a label of this function; conditional on the i32 on the stack. */
static void g_branch(struct wasm_gen *g, sljit_s32 target, sljit_s32 conditional)
{
	if (target > g->seg) {
		g_op(g, conditional ? OP_BR_IF : OP_BR);
		b_uleb(g->b, (sljit_uw)(target - g->seg - 1 + g->inner));
		return;
	}
	if (conditional) {
		g_op(g, OP_IF);
		g_op(g, BLOCK_EMPTY);
		g->inner++;
	}
	g_i32(g, target);
	g_local(g, OP_LOCAL_SET, L_STATE);
	g_op(g, OP_BR);
	b_uleb(g->b, (sljit_uw)(g->labels - g->seg + g->inner));
	if (conditional) {
		g_op(g, OP_END);
		g->inner--;
	}
}

/* Pushes the table index of a code address operand. */
static void g_target(struct wasm_gen *g, sljit_s32 p, sljit_sw w)
{
	if (p == SLJIT_IMM)
		g_i32(g, (sljit_s32)w);
	else if (p & SLJIT_MEM)
		g_load(g, OP_I32_LOAD, p, w);
	else {
		g_local(g, OP_LOCAL_GET, p);
		g_op(g, OP_I32_WRAP_I64);
	}
}

static void g_call(struct wasm_gen *g, sljit_s32 type, sljit_s32 arg_types, sljit_s32 p, sljit_sw w)
{
	sljit_s32 i, t;
	sljit_s32 ret = arg_types & SLJIT_ARG_MASK;
	sljit_s32 args = arg_types >> SLJIT_ARG_SHIFT;

	for (i = 1; args; i++, args >>= SLJIT_ARG_SHIFT) {
		t = args & SLJIT_ARG_MASK;
		if (t != SLJIT_ARG_TYPE_W && t != SLJIT_ARG_TYPE_32 && t != SLJIT_ARG_TYPE_P) {
			g->compiler->error = SLJIT_ERR_UNSUPPORTED;
			return;
		}
		g_local(g, OP_LOCAL_GET, i);
		if (t != SLJIT_ARG_TYPE_W)
			g_op(g, OP_I32_WRAP_I64);
	}
	g_target(g, p, w);
	g_op(g, OP_CALL_INDIRECT);
	b_uleb(g->b, (sljit_uw)g_call_type(g, arg_types));
	b_uleb(g->b, 0);

	if (ret == SLJIT_ARG_TYPE_32 || ret == SLJIT_ARG_TYPE_P)
		g_op(g, OP_I64_EXTEND_I32_U);
	if (ret != SLJIT_ARG_TYPE_RET_VOID)
		g_local(g, OP_LOCAL_SET, SLJIT_R0);

	if (type & SLJIT_CALL_RETURN) {
		g_local(g, OP_LOCAL_GET, SLJIT_R0);
		g_op(g, OP_RETURN);
	}
}

/* x -> (byte swapped x), on the i32 on the stack (via L_A1). */
static void g_bswap32(struct wasm_gen *g)
{
	g_local(g, OP_LOCAL_TEE, L_A1);
	g_i32(g, 8);
	g_op(g, OP_ROTL);
	g_i32(g, 0x00ff00ff);
	g_op(g, OP_AND);
	g_local(g, OP_LOCAL_GET, L_A1);
	g_i32(g, 8);
	g_op(g, OP_ROTR);
	g_i32(g, (sljit_s32)0xff00ff00);
	g_op(g, OP_AND);
	g_op(g, OP_OR);
}

static void g_op1(struct wasm_gen *g, struct wasm_ins *ins)
{
	sljit_s32 op = GET_OPCODE(ins->op);
	sljit_s32 is32 = ins->op & SLJIT_32;
	sljit_s32 src = ins->src1;
	sljit_sw srcw = ins->src1w;
	sljit_u8 load, store;
	sljit_sw imm;

	switch (op) {
	case SLJIT_MOV:
		g_src(g, src, srcw, 0);
		g_dst(g, ins->dst, ins->dstw, 0, OP_I64_STORE);
		return;
	case SLJIT_MOV_U8:
	case SLJIT_MOV_S8:
	case SLJIT_MOV_U16:
	case SLJIT_MOV_S16:
	case SLJIT_MOV_U32:
	case SLJIT_MOV_S32:
	case SLJIT_MOV32:
	case SLJIT_MOV_P:
		switch (op) {
		case SLJIT_MOV_U8: load = OP_I64_LOAD8_U; store = OP_I64_STORE8; imm = (sljit_u8)srcw; break;
		case SLJIT_MOV_S8: load = OP_I64_LOAD8_S; store = OP_I64_STORE8; imm = (sljit_s8)srcw; break;
		case SLJIT_MOV_U16: load = OP_I64_LOAD16_U; store = OP_I64_STORE16; imm = (sljit_u16)srcw; break;
		case SLJIT_MOV_S16: load = OP_I64_LOAD16_S; store = OP_I64_STORE16; imm = (sljit_s16)srcw; break;
		case SLJIT_MOV_S32: load = OP_I64_LOAD32_S; store = OP_I64_STORE32; imm = (sljit_s32)srcw; break;
		default: load = OP_I64_LOAD32_U; store = OP_I64_STORE32; imm = (sljit_u32)srcw; break;
		}
		if (src == SLJIT_IMM)
			g_i64(g, imm);
		else if (src & SLJIT_MEM)
			g_load(g, load, src, srcw);
		else {
			g_local(g, OP_LOCAL_GET, src);
			switch (op) {
			case SLJIT_MOV_U8: g_i64(g, 0xff); g_op(g, ALU_64(OP_AND)); break;
			case SLJIT_MOV_S8: g_op(g, OP_I64_EXTEND8_S); break;
			case SLJIT_MOV_U16: g_i64(g, 0xffff); g_op(g, ALU_64(OP_AND)); break;
			case SLJIT_MOV_S16: g_op(g, OP_I64_EXTEND16_S); break;
			case SLJIT_MOV_S32: g_op(g, OP_I64_EXTEND32_S); break;
			default: g_op(g, OP_I32_WRAP_I64); g_op(g, OP_I64_EXTEND_I32_U); break;
			}
		}
		g_dst(g, ins->dst, ins->dstw, 0, store);
		return;
	case SLJIT_CLZ:
	case SLJIT_CTZ:
		g_src(g, src, srcw, is32);
		g_op(g, is32 ? (op == SLJIT_CLZ ? OP_CLZ : OP_CTZ) : ALU_64(op == SLJIT_CLZ ? OP_CLZ : OP_CTZ));
		g_dst(g, ins->dst, ins->dstw, is32, is32 ? OP_I32_STORE : OP_I64_STORE);
		return;
	case SLJIT_REV:
		if (is32) {
			g_src(g, src, srcw, 1);
			g_bswap32(g);
			g_dst(g, ins->dst, ins->dstw, 1, OP_I32_STORE);
			return;
		}
		g_src(g, src, srcw, 0);
		g_local(g, OP_LOCAL_TEE, L_T1);
		g_op(g, OP_I32_WRAP_I64);
		g_bswap32(g);
		g_op(g, OP_I64_EXTEND_I32_U);
		g_i64(g, 32);
		g_op(g, ALU_64(OP_SHL));
		g_local(g, OP_LOCAL_GET, L_T1);
		g_i64(g, 32);
		g_op(g, ALU_64(OP_SHR_U));
		g_op(g, OP_I32_WRAP_I64);
		g_bswap32(g);
		g_op(g, OP_I64_EXTEND_I32_U);
		g_op(g, ALU_64(OP_OR));
		g_dst(g, ins->dst, ins->dstw, 0, OP_I64_STORE);
		return;
	case SLJIT_REV_U16:
	case SLJIT_REV_S16:
		if (src & SLJIT_MEM)
			g_load(g, OP_I32_LOAD16_U, src, srcw);
		else
			g_src(g, src, srcw, 1);
		g_local(g, OP_LOCAL_TEE, L_A1);
		g_i32(g, 8);
		g_op(g, OP_SHR_U);
		g_i32(g, 0xff);
		g_op(g, OP_AND);
		g_local(g, OP_LOCAL_GET, L_A1);
		g_i32(g, 0xff);
		g_op(g, OP_AND);
		g_i32(g, 8);
		g_op(g, OP_SHL);
		g_op(g, OP_OR);
		if (op == SLJIT_REV_S16) {
			g_op(g, OP_I32_EXTEND16_S);
			if (!is32 && !(ins->dst & SLJIT_MEM)) {
				g_op(g, OP_I64_EXTEND_I32_S);
				g_dst(g, ins->dst, ins->dstw, 0, OP_I64_STORE16);
				return;
			}
		}
		g_dst(g, ins->dst, ins->dstw, 1, OP_I32_STORE16);
		return;
	case SLJIT_REV_U32:
	case SLJIT_REV_S32:
		g_src(g, src, srcw, 1);
		g_bswap32(g);
		if (op == SLJIT_REV_S32 && !(ins->dst & SLJIT_MEM)) {
			g_op(g, OP_I64_EXTEND_I32_S);
			g_dst(g, ins->dst, ins->dstw, 0, OP_I64_STORE32);
			return;
		}
		g_dst(g, ins->dst, ins->dstw, 1, OP_I32_STORE);
		return;
	default:
		g->compiler->error = SLJIT_ERR_UNSUPPORTED;
		return;
	}
}

static sljit_u8 g_alu(sljit_s32 opcode)
{
	switch (opcode) {
	case SLJIT_ADD:
	case SLJIT_ADDC: return OP_ADD;
	case SLJIT_SUB:
	case SLJIT_SUBC: return OP_SUB;
	case SLJIT_MUL: return OP_MUL;
	case SLJIT_AND: return OP_AND;
	case SLJIT_OR: return OP_OR;
	case SLJIT_XOR: return OP_XOR;
	case SLJIT_SHL:
	case SLJIT_MSHL: return OP_SHL;
	case SLJIT_LSHR:
	case SLJIT_MLSHR: return OP_SHR_U;
	case SLJIT_ASHR:
	case SLJIT_MASHR: return OP_SHR_S;
	case SLJIT_ROTL: return OP_ROTL;
	case SLJIT_ROTR: return OP_ROTR;
	default: return 0;
	}
}

static void g_op2(struct wasm_gen *g, struct wasm_ins *ins)
{
	sljit_s32 opcode = GET_OPCODE(ins->op);
	sljit_s32 is32 = ins->op & SLJIT_32;
	sljit_u8 alu = g_alu(opcode);
	sljit_u8 op = is32 ? alu : ALU_64(alu);

	if (!alu) {
		g->compiler->error = SLJIT_ERR_UNSUPPORTED;
		return;
	}

	if (opcode == SLJIT_ADDC || opcode == SLJIT_SUBC) {
		g_cond(g, ins->flags, SLJIT_CARRY);
		g_local(g, OP_LOCAL_SET, L_A1);
	}

	if (!HAS_FLAGS(ins->op)) {
		g_src(g, ins->src1, ins->src1w, is32);
		g_src(g, ins->src2, ins->src2w, is32);
		g_op(g, op);
	} else {
		g_src(g, ins->src1, ins->src1w, is32);
		if (is32)
			g_op(g, OP_I64_EXTEND_I32_U);
		g_local(g, OP_LOCAL_SET, L_FA);
		g_src(g, ins->src2, ins->src2w, is32);
		if (is32)
			g_op(g, OP_I64_EXTEND_I32_U);
		g_local(g, OP_LOCAL_SET, L_FB);
		g_local(g, OP_LOCAL_GET, L_FA);
		if (is32)
			g_op(g, OP_I32_WRAP_I64);
		g_local(g, OP_LOCAL_GET, L_FB);
		if (is32)
			g_op(g, OP_I32_WRAP_I64);
		g_op(g, op);
	}

	if (opcode == SLJIT_ADDC || opcode == SLJIT_SUBC) {
		g_local(g, OP_LOCAL_GET, L_A1);
		if (!is32)
			g_op(g, OP_I64_EXTEND_I32_U);
		g_op(g, is32 ? (opcode == SLJIT_ADDC ? OP_ADD : OP_SUB) : ALU_64(opcode == SLJIT_ADDC ? OP_ADD : OP_SUB));
	}

	if (HAS_FLAGS(ins->op)) {
		if (is32)
			g_op(g, OP_I64_EXTEND_I32_U);
		g_local(g, ins->dst ? OP_LOCAL_TEE : OP_LOCAL_SET, L_FR);
		if (!ins->dst)
			return;
		if (is32)
			g_op(g, OP_I32_WRAP_I64);
	}
	g_dst(g, ins->dst, ins->dstw, is32, is32 ? OP_I32_STORE : OP_I64_STORE);
}

static void g_op0(struct wasm_gen *g, struct wasm_ins *ins)
{
	sljit_s32 opcode = GET_OPCODE(ins->op);
	sljit_s32 is32 = ins->op & SLJIT_32;
	sljit_s32 sig = (opcode == SLJIT_DIVMOD_SW || opcode == SLJIT_DIV_SW);
	sljit_u8 div = sig ? OP_DIV_S : OP_DIV_U;
	sljit_u8 rem = sig ? OP_REM_S : OP_REM_U;

	if (opcode == SLJIT_DIVMOD_UW || opcode == SLJIT_DIVMOD_SW) {
		g_src(g, SLJIT_R0, 0, is32);
		g_src(g, SLJIT_R1, 0, is32);
		g_op(g, is32 ? rem : ALU_64(rem));
		if (is32)
			g_op(g, OP_I64_EXTEND_I32_U);
		g_local(g, OP_LOCAL_SET, L_T0);
	}
	g_src(g, SLJIT_R0, 0, is32);
	g_src(g, SLJIT_R1, 0, is32);
	g_op(g, is32 ? div : ALU_64(div));
	if (is32)
		g_op(g, OP_I64_EXTEND_I32_U);
	g_local(g, OP_LOCAL_SET, SLJIT_R0);
	if (opcode == SLJIT_DIVMOD_UW || opcode == SLJIT_DIVMOD_SW) {
		g_local(g, OP_LOCAL_GET, L_T0);
		g_local(g, OP_LOCAL_SET, SLJIT_R1);
	}
}

static void g_ins(struct wasm_gen *g, struct wasm_ins *ins)
{
	sljit_s32 is32, t, opcode;
	struct sljit_jump *jump;

	switch (ins->kind) {
	case K_OP0:
		g_op0(g, ins);
		return;

	case K_OP1:
		g_op1(g, ins);
		return;

	case K_OP2:
		g_op2(g, ins);
		return;

	case K_OP2R:
		is32 = ins->op & SLJIT_32;
		g_src(g, ins->dst, 0, is32);
		g_src(g, ins->src1, ins->src1w, is32);
		g_src(g, ins->src2, ins->src2w, is32);
		g_op(g, is32 ? OP_MUL : ALU_64(OP_MUL));
		g_op(g, is32 ? OP_ADD : ALU_64(OP_ADD));
		g_dst(g, ins->dst, 0, is32, 0);
		return;

	case K_OP_FLAGS:
		opcode = GET_OPCODE(ins->op);
		is32 = (ins->op & SLJIT_32) || opcode == SLJIT_MOV32;
		if (opcode == SLJIT_MOV || opcode == SLJIT_MOV32) {
			g_cond(g, ins->flags, ins->aux);
			g_op(g, OP_I64_EXTEND_I32_U);
			g_dst(g, ins->dst, ins->dstw, 0, is32 ? OP_I64_STORE32 : OP_I64_STORE);
			return;
		}
		g_cond(g, ins->flags, ins->aux);
		g_op(g, OP_I64_EXTEND_I32_U);
		g_local(g, OP_LOCAL_SET, L_FB);
		g_src(g, ins->dst, ins->dstw, 0);
		g_local(g, OP_LOCAL_TEE, L_FA);
		g_local(g, OP_LOCAL_GET, L_FB);
		g_op(g, ALU_64(g_alu(opcode)));
		if (is32) {
			g_op(g, OP_I32_WRAP_I64);
			g_op(g, OP_I64_EXTEND_I32_U);
		}
		if (HAS_FLAGS(ins->op))
			g_local(g, OP_LOCAL_TEE, L_FR);
		g_dst(g, ins->dst, ins->dstw, 0, is32 ? OP_I64_STORE32 : OP_I64_STORE);
		return;

	case K_SELECT:
		is32 = ins->op & SLJIT_32;
		g_src(g, ins->src1, ins->src1w, is32);
		if (is32)
			g_op(g, OP_I64_EXTEND_I32_U);
		g_local(g, OP_LOCAL_SET, L_T0);
		g_local(g, OP_LOCAL_GET, L_T0);
		g_local(g, OP_LOCAL_GET, ins->src2);
		if (ins->op & SLJIT_COMPARE_SELECT)
			g_compare(g, L_T0, ins->src2, ins->op & 0xff, is32);
		else
			g_cond(g, ins->flags, ins->op);
		g_op(g, OP_SELECT);
		if (is32) {
			g_op(g, OP_I32_WRAP_I64);
			g_op(g, OP_I64_EXTEND_I32_U);
		}
		g_local(g, OP_LOCAL_SET, ins->dst);
		return;

	case K_JUMP:
		jump = ins->jump;
		t = ins->op & 0xff;
		if (t == SLJIT_CALL || t == SLJIT_CALL_REG_ARG) {
			if (!(jump->flags & JUMP_ADDR)) {
				g->compiler->error = SLJIT_ERR_UNSUPPORTED;
				return;
			}
			g_call(g, ins->op, ins->aux, SLJIT_IMM, (sljit_sw)jump->u.target);
			return;
		}
		if (t != SLJIT_JUMP)
			g_cond(g, ins->flags, t);
		if (jump->flags & JUMP_ADDR) {
			if (t != SLJIT_JUMP) {
				g_op(g, OP_IF);
				g_op(g, BLOCK_EMPTY);
			}
			g_i32(g, (sljit_s32)jump->u.target);
			g_local(g, OP_LOCAL_SET, L_A0);
			g_tail_call_slot(g);
			if (t != SLJIT_JUMP)
				g_op(g, OP_END);
			return;
		}
		if (!jump->u.label) {
			if (t != SLJIT_JUMP)
				g_op(g, OP_DROP);
			g_op(g, OP_UNREACHABLE);
			return;
		}
		g_branch(g, (sljit_s32)jump->u.label->u.index + 1, t != SLJIT_JUMP);
		return;

	case K_IJUMP:
		g_target(g, ins->src1, ins->src1w);
		g_local(g, OP_LOCAL_SET, L_A0);
		g_tail_call_slot(g);
		return;

	case K_ICALL:
		g_call(g, ins->op, ins->aux, ins->src1, ins->src1w);
		return;

	case K_RETURN:
		g_local(g, OP_LOCAL_GET, SLJIT_R0);
		g_op(g, OP_RETURN);
		return;
	}
}

static void g_body(struct wasm_gen *g)
{
	struct sljit_compiler *compiler = g->compiler;
	struct sljit_memory_fragment *frag;
	struct sljit_label *label = compiler->labels;
	struct wasm_ins *ins, *end;
	sljit_uw index = 0;
	sljit_s32 i;

	/* locals: five i64, two i32 */
	b_uleb(g->b, 2);
	b_uleb(g->b, 5);
	b_byte(g->b, TYPE_I64);
	b_uleb(g->b, 2);
	b_byte(g->b, TYPE_I32);

	g_op(g, OP_LOOP);
	g_op(g, BLOCK_EMPTY);
	for (i = 0; i <= g->labels; i++) {
		g_op(g, OP_BLOCK);
		g_op(g, BLOCK_EMPTY);
	}
	g_local(g, OP_LOCAL_GET, L_STATE);
	g_op(g, OP_BR_TABLE);
	b_uleb(g->b, (sljit_uw)g->labels + 1);
	for (i = 0; i <= g->labels; i++)
		b_uleb(g->b, (sljit_uw)i);
	b_uleb(g->b, 0);
	g_op(g, OP_END);
	g->seg = 0;

	for (frag = compiler->buf; frag; frag = frag->next) {
		ins = (struct wasm_ins*)frag->memory;
		end = (struct wasm_ins*)(frag->memory + frag->used_size);
		for (; ins < end; ins++, index++) {
			while (label && label->size == index) {
				g_op(g, OP_END);
				g->seg++;
				label = label->next;
			}
			g_ins(g, ins);
		}
	}
	while (label) {
		g_op(g, OP_END);
		g->seg++;
		label = label->next;
	}

	g_op(g, OP_END);
	g_op(g, OP_UNREACHABLE);
	g_op(g, OP_END);
}

static void g_functype(struct wasm_buf *b, sljit_s32 arg_types)
{
	sljit_s32 ret = arg_types & SLJIT_ARG_MASK;
	sljit_s32 args = arg_types >> SLJIT_ARG_SHIFT;
	sljit_s32 n = 0, t;

	for (t = args; t; t >>= SLJIT_ARG_SHIFT)
		n++;
	b_byte(b, TYPE_FUNC);
	b_uleb(b, (sljit_uw)n);
	for (t = args; t; t >>= SLJIT_ARG_SHIFT)
		b_byte(b, (t & SLJIT_ARG_MASK) == SLJIT_ARG_TYPE_W ? TYPE_I64 : TYPE_I32);
	if (ret == SLJIT_ARG_TYPE_RET_VOID)
		b_uleb(b, 0);
	else {
		b_uleb(b, 1);
		b_byte(b, ret == SLJIT_ARG_TYPE_W ? TYPE_I64 : TYPE_I32);
	}
}

/* The entry: the arguments into their registers, the body from its start. */
static void g_entry(struct wasm_buf *b, struct sljit_compiler *compiler)
{
	sljit_s32 arg_types = compiler->wasm_args;
	sljit_s32 ret = arg_types & SLJIT_ARG_MASK;
	sljit_s32 reg_arg = compiler->options & SLJIT_ENTER_REG_ARG;
	sljit_s32 args[4], types[4], n = 0, r, i, t;

	for (t = arg_types >> SLJIT_ARG_SHIFT; t && n < 4; t >>= SLJIT_ARG_SHIFT, n++) {
		types[n] = t & SLJIT_ARG_MASK;
		args[n] = reg_arg ? SLJIT_R(n) : SLJIT_S(n);
	}

	b_uleb(b, 0);
	b_byte(b, OP_I32_CONST);
	b_sleb(b, 0);
	for (r = 1; r <= WASM_REGS; r++) {
		for (i = 0; i < n && args[i] != r; i++)
			;
		if (i < n) {
			b_byte(b, OP_LOCAL_GET);
			b_uleb(b, (sljit_uw)i);
			if (types[i] != SLJIT_ARG_TYPE_W)
				b_byte(b, OP_I64_EXTEND_I32_U);
		} else {
			b_byte(b, OP_I64_CONST);
			b_sleb(b, 0);
		}
	}
	b_byte(b, OP_CALL);
	b_uleb(b, F_BODY);
	if (ret == SLJIT_ARG_TYPE_RET_VOID)
		b_byte(b, OP_DROP);
	else if (ret != SLJIT_ARG_TYPE_W)
		b_byte(b, OP_I32_WRAP_I64);
	b_byte(b, OP_END);
}

static void g_thunk(struct wasm_buf *b, sljit_s32 state)
{
	sljit_s32 r;

	b_uleb(b, 0);
	b_byte(b, OP_I32_CONST);
	b_sleb(b, state);
	for (r = 0; r < WASM_REGS; r++) {
		b_byte(b, OP_LOCAL_GET);
		b_uleb(b, (sljit_uw)r);
	}
	b_byte(b, OP_RETURN_CALL);
	b_uleb(b, F_BODY);
	b_byte(b, OP_END);
}

static void g_function(struct wasm_buf *code, struct wasm_buf *f)
{
	b_uleb(code, f->size);
	b_bytes(code, f->data, f->size);
	if (f->failed)
		code->failed = 1;
	f->size = 0;
}

SLJIT_API_FUNC_ATTRIBUTE void* sljit_generate_code(struct sljit_compiler *compiler, sljit_s32 options, void *exec_allocator_data)
{
	struct wasm_gen g;
	struct wasm_buf body, module, sec, f;
	struct sljit_label *label;
	sljit_s32 labels = (sljit_s32)compiler->label_count;
	sljit_s32 i, r, base;

	SLJIT_UNUSED_ARG(options);
	SLJIT_UNUSED_ARG(exec_allocator_data);

	CHECK_ERROR_PTR();
	CHECK_PTR(check_sljit_generate_code(compiler, options));

	reverse_buf(compiler);

	b_init(&body, compiler->allocator_data);
	g.compiler = compiler;
	g.b = &body;
	g.labels = labels;
	g.seg = 0;
	g.inner = 0;
	g.calls = NULL;
	g.call_count = 0;
	g.call_cap = 0;
	g_body(&g);

	b_init(&module, compiler->allocator_data);
	b_init(&sec, compiler->allocator_data);
	b_init(&f, compiler->allocator_data);

	b_bytes(&module, "\0asm\1\0\0\0", 8);

	/* types */
	b_uleb(&sec, (sljit_uw)(T_CALL0 + g.call_count));
	b_byte(&sec, TYPE_FUNC);
	b_uleb(&sec, WASM_REGS + 1);
	b_byte(&sec, TYPE_I32);
	for (r = 0; r < WASM_REGS; r++)
		b_byte(&sec, TYPE_I64);
	b_uleb(&sec, 1);
	b_byte(&sec, TYPE_I64);
	b_byte(&sec, TYPE_FUNC);
	b_uleb(&sec, WASM_REGS);
	for (r = 0; r < WASM_REGS; r++)
		b_byte(&sec, TYPE_I64);
	b_uleb(&sec, 1);
	b_byte(&sec, TYPE_I64);
	g_functype(&sec, compiler->wasm_args);
	for (i = 0; i < g.call_count; i++)
		g_functype(&sec, g.calls[i]);
	b_section(&module, 1, &sec);

	/* imports: the memory, the table, the first slot */
	b_uleb(&sec, 3);
	b_name(&sec, "e");
	b_name(&sec, "m");
	b_byte(&sec, 0x02);
	b_byte(&sec, 0x00);
	b_uleb(&sec, 0);
	b_name(&sec, "e");
	b_name(&sec, "t");
	b_byte(&sec, 0x01);
	b_byte(&sec, TYPE_FUNCREF);
	b_byte(&sec, 0x00);
	b_uleb(&sec, 0);
	b_name(&sec, "e");
	b_name(&sec, "b");
	b_byte(&sec, 0x03);
	b_byte(&sec, TYPE_I32);
	b_byte(&sec, 0x00);
	b_section(&module, 2, &sec);

	/* functions: the body, the entry, a thunk a label */
	b_uleb(&sec, (sljit_uw)(2 + labels));
	b_uleb(&sec, T_BODY);
	b_uleb(&sec, T_ENTRY);
	for (i = 0; i < labels; i++)
		b_uleb(&sec, T_THUNK);
	b_section(&module, 3, &sec);

	/* elements: the entry and the thunks from the first slot */
	b_uleb(&sec, 1);
	b_byte(&sec, 0x00);
	b_byte(&sec, OP_GLOBAL_GET);
	b_uleb(&sec, 0);
	b_byte(&sec, OP_END);
	b_uleb(&sec, (sljit_uw)(1 + labels));
	for (i = 0; i <= labels; i++)
		b_uleb(&sec, (sljit_uw)(F_ENTRY + i));
	b_section(&module, 9, &sec);

	/* code */
	b_uleb(&sec, (sljit_uw)(2 + labels));
	g_function(&sec, &body);
	g_entry(&f, compiler);
	g_function(&sec, &f);
	for (i = 1; i <= labels; i++) {
		g_thunk(&f, i);
		g_function(&sec, &f);
	}
	b_section(&module, 10, &sec);

	b_free(&body);
	b_free(&f);
	if (g.calls)
		SLJIT_FREE(g.calls, compiler->allocator_data);

	if (compiler->error) {
		b_free(&module);
		return NULL;
	}
	if (module.failed) {
		b_free(&module);
		compiler->error = SLJIT_ERR_ALLOC_FAILED;
		return NULL;
	}

	base = sljit_wasm_load(module.data, (sljit_u32)module.size, labels + 1);
	compiler->executable_size = module.size;
	b_free(&module);
	if (!base) {
		compiler->error = SLJIT_ERR_ALLOC_FAILED;
		return NULL;
	}

	for (label = compiler->labels; label; label = label->next)
		label->u.addr = (sljit_uw)base + label->u.index + 1;

	compiler->error = SLJIT_ERR_COMPILED;
	compiler->executable_offset = 0;
	return (void*)(sljit_uw)base;
}
