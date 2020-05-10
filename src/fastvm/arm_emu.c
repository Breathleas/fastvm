﻿
#include "mcore/mcore.h"
#include "vm.h"
#include "arm_emu.h"
#include "minst.h"
#include <math.h>


#define ARM_SP_VAL(e)   e->regs[ARM_REG_SP]     
#define ARM_PC_VAL(e)   e->regs[ARM_REG_PC]
#define ARM_APSR_PTR(e)     ((struct arm_cpsr *)&e->regs[ARM_REG_APSR])

#define REG_SET_KNOWN(e, reg)       (e->known |= 1 << reg)
#define REG_SET_UNKNOWN(e, reg)     (e->knwwn &= ~(1 << reg))
#define REG_IS_KNOWN(e,reg)         (e->knwon & (1 << reg)

#define KB          (1024)
#define MB          (1024 * 1024)

#define PROCESS_STACK_SIZE          (256 * KB)
#define PROCESS_STACK_BASE          0x10000000
#define IN_PROCESS_STACK_RANGE(a)   (((a) < PROCESS_STACK_BASE) && (a >= (PROCESS_STACK_BASE - PROCESS_STACK_SIZE)))

#define MEM_ERROR           -1
#define MEM_CODE            0       // [0 - 64M)
#define MEM_STACK           1       // [254 - 256M)
#define MEM_HEAP            2       // ?

#define MEM_STACK_TOP(e)        (PROCESS_STACK_BASE - e->regs[ARM_REG_SP])
#define MEM_STACK_TOP1(e)       (PROCESS_STACK_BASE - e->prev_regs[ARM_REG_SP])

typedef int         reg_t;

#define BITS_GET(a,offset,len)   ((a >> (offset )) & ((1 << len) - 1))
#define BITS_GET_SHL(a, offset, len, sh)      (BITS_GET(a, offset, len) << sh)

#define ARM_UNPREDICT()   vm_error("arm unpredictable. %s:%d", __FILE__, __LINE__)

#define IDUMP_ADDR              0x01
#define IDUMP_BINCODE           0x02
#define IDUMP_STACK_HEIGHT      0x04    
#define IDUMP_STATUS            0x08
#define IDUMP_TRACE             0x10
#define IDUMP_LIVE              0x20
#define IDUMP_REACHING_DEFS     0x40
#define IDUMP_DEFAULT           -1

#define EMU_SEQ_MODE            0
#define EMU_CONST_MODE          1

/* arm指令中，某部分指令是上下文相关的 */
#define EMU_IS_SEQ_MODE(e)      (e->seq_mod == EMU_SEQ_MODE)
#define EMU_IS_CONST_MODE(e)    (e->seq_mod == EMU_CONST_MODE)

#define EMU_SET_SEQ_MODE(e)     (e->seq_mod = EMU_SEQ_MODE)
#define EMU_SET_CONST_MODE(e)   (e->seq_mod = EMU_CONST_MODE)

struct arm_inst_context {
    reg_t   ld;
    reg_t   lm;
    reg_t   ln;
    int     register_list;
    int     imm;
    int     m;
    int     setflags;
    int     cond;
    int     u;
};

struct emu_temp_var
{
    /* temp variable id */
    int     t;
    /* stack address */
    int     top;
};

struct arm_emu {
    struct {
        unsigned char*  data;
        int             len;
        int             pos;

        struct arm_inst_context ctx;
    } code;

    char inst_fmt[64];
    unsigned int prev_regs[32];
    unsigned int regs[32];
    unsigned int known;
    int seq_mod;

    int(*inst_func)(unsigned char *inst, int len,  char *inst_str, void *user_ctx);
    void *user_ctx;

    /* emulate stack */
    struct {
        /* 堆栈分配的地址 */
        unsigned char*      data;
        /* 我们分配的内存写的时候从低往高写，但是真实的程序堆栈是从高往低写，所以我们让base指向末尾 */
        unsigned char*      base;
        int                top;
        int                size;
    } stack;

    struct {
        unsigned nfa : 1;
        unsigned dfa :1;
        unsigned mblk : 1;
        unsigned cfg : 1;
    } dump;

    /* target machine内的函数起始位置 */
    int             baseaddr;
    int             thumb;
    int             meet_blx;
    int             decode_inst_flag;
    /* P26 */
    //struct arm_cpsr apsr;

    /* 生成IR时，会产生大量的临时变量，这个是临时变量计数器 */
    struct dynarray tvars;
    int             tvar_id;

    struct {
        int     inblock;
        int     num;
        int     cond;
        char    et[8];
    } it;

    struct minst_blk        mblk;
    struct minst            *prev_minst;
};

static const char *regstr[] = {
    "r0", "r1", "r2", "r3",
    "r4", "r5", "r6", "r7",
    "r8", "r9", "r10", "r11",
    "r12", "sp", "lr", "pc",
    "", "", "apsr", "",
    "", "", "", "",
};

static const char *condstr[] = {
    "eq", "ne", "cs", "cc",
    "mi", "pl", "vs", "vc",
    "hi", "ls", "ge", "lt",
    "gt", "le", "{al}"
};

#define FLAG_DISABLE_EMU            0x1            
#define IS_DISABLE_EMU(e)           (e->decode_inst_flag & FLAG_DISABLE_EMU)

typedef int(*arm_inst_func)    (struct arm_emu *emu, struct minst *minst, uint16_t *inst, int inst_len);

#include "arm_op.h"

char *it2str(int firstcond, int mask, char *buf)
{
    int i, j;
    int c0 = firstcond & 1;

    buf[0] = 0;
    i = RightMostBitPos(mask, 4);
    if (i == -1)
        ARM_UNPREDICT();

    strcat(buf, "t");

    for (j = 0; j < (3 - i); j++) {
        strcat(buf, ((mask >> (3 - j)) & 1) == c0 ? "t":"e");
    }

    return buf;
}

const char *cur_inst_it_cond(struct arm_emu *e)
{
    if (!e->it.inblock)
        return "";

    return (e->it.et[e->it.num - e->it.inblock] == 't') ? condstr[e->it.cond]:condstr[(e->it.cond & 1) ? (e->it.cond - 1):(e->it.cond + 1)];
}

static char* reglist2str(int reglist, char *buf)
{
    int i, start_reg = -1, len;
    buf[0] = 0;

    strcat(buf, "{");
    for (i = 0; i < 16; ) {
        if (reglist & (1 << i)) {
            strcat(buf, regstr[i]);

            start_reg = i++;
            while (reglist & (1 << i)) i++;

            if (i != (start_reg + 1)) {
                strcat(buf, "-");
                strcat(buf, regstr[i - 1]);
            }

            strcat(buf, ",");
        }
        else
            i++;
    }
    len = strlen(buf);
    if (buf[len - 1] == ',')  buf[len - 1] = 0;
    strcat(buf, "}");

    return buf;
}

static void arm_prepare_dump(struct arm_emu *emu, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(emu->inst_fmt, sizeof (emu->inst_fmt), fmt, ap);
    va_end(ap);
}

static int arm_dump_bitset(const char *desc, struct bitset *v, char *obuf)
{
    char *o = obuf;
    int olen = 0, pos = -1;
    /* dump liveness calculate result */
    olen = sprintf(o += olen, "[%s: ", desc);
    while ((pos = bitset_next_bit_pos(v, pos + 1)) >= 0) {
        olen = sprintf(o += olen, "%d  ", pos);
    }
    if (o[olen - 1] == ' ') olen--;
    olen = sprintf(o += olen, "] ");

    return (o + olen) - obuf;
}

static int arm_dump_temp_reglist(const char *desc, struct bitset *v, char *obuf)
{
    char *o = obuf;
    int i, j, olen = 0;
    /* dump liveness calculate result */
    olen = sprintf(o += olen, "[%s: ", desc);
    if (v->len4) {
        for (i = 0; i < 32; i++) {
            if (v->data[0] & (1 << i))
                olen = sprintf(o += olen, "%s ", regstr[i]);
        }

        for (i = 1; i < v->len4; i++) {
            for (j = 0; j < 32; j++) {
                if (v->data[i] & (1 << j))
                    olen = sprintf(o += olen, "t%d ", i * 32 + j);
            }
        }
    }
    if (o[olen - 1] == ' ') olen--;
    olen = sprintf(o += olen, "] ");

    return (o + olen) - obuf;
}

static int arm_inst_print_format(struct arm_emu *emu, struct minst *minst, unsigned int flag, char *obuf)
{
    int i, len, olen = 0;
    char *buf, *param, *o = obuf;

    if (!minst->len) {
        obuf[0] = 0;
        return 0;
    }

    buf = emu->inst_fmt;
    len = strlen(buf);
    for (i = 0; i < len; i++) {
        buf[i] = toupper(buf[i]);
    }

    param = strchr(buf, ' ');
    if (param) {
        *param++ = 0;
        while (isblank(*param)) param++;
    }

    olen = sprintf(o += olen, "%04d ", minst->id);

    if (flag & IDUMP_ADDR)
        olen = sprintf(o += olen, "%08x ", emu->baseaddr + (minst->addr - emu->code.data));

    if (flag & IDUMP_STACK_HEIGHT)
        olen = sprintf(o += olen, "%03x ", MEM_STACK_TOP1(emu));

    if (flag & IDUMP_BINCODE) {
        for (i = 0; i < minst->len; i++) {
            olen = sprintf(o += olen, "%02x ", (unsigned char)minst->addr[i]);
        }
    }

    for (; i < 6; i++) {
        olen = sprintf(o += olen, "   ");
    }

    olen = sprintf(o += olen, "%-10s %-18s  ", buf, param);

    if (flag & IDUMP_STATUS) {
        olen = sprintf(o += olen, "[");
        olen = sprintf(o += olen, "%c", minst->flag.dead_code ? 'D':' ');
        olen = sprintf(o += olen, "]");
    }

    if (flag & IDUMP_TRACE) {
        if (emu->code.ctx.ld != -1) {
            olen = sprintf(o += olen, "[%-3s=%08x", regstr[emu->code.ctx.ld], minst->ld_imm);
        }
        else {
            olen = sprintf(o += olen, "[");
        }

        olen = sprintf(o += olen, ",APSR=%c%c%c%c]", ARM_APSR_PTR(emu)->z ? 'Z':' ', 
            ARM_APSR_PTR(emu)->c ? 'C':' ', ARM_APSR_PTR(emu)->n ? 'N':' ', ARM_APSR_PTR(emu)->v ? 'V':' ');
    }

    /* dump liveness calculate result */
    if (flag & IDUMP_LIVE) {
        olen = arm_dump_temp_reglist("def", &minst->def, o += olen);
        olen = arm_dump_temp_reglist("use", &minst->use, o += olen);
        //olen = arm_dump_temp_reglist("in", &minst->in, o += olen);
        //olen = arm_dump_temp_reglist("out", &minst->out, o+= olen);
    }

    if (flag & IDUMP_REACHING_DEFS) {
        olen = arm_dump_bitset("r_in", &minst->rd_in, o += olen);
        olen = arm_dump_bitset("r_out", &minst->rd_out, o+= olen);
        olen = arm_dump_bitset("kills", &minst->kills, o += olen);
    }

    o += olen;
    emu->inst_fmt[0] = 0;

    return o - obuf;
}

static struct emu_temp_var *emu_alloc_temp_var(struct arm_emu *emu, unsigned long addr)
{
    struct emu_temp_var *var;

    /* 所有对进程堆栈的操作，都要转换成临时变量，假如这个地址不是堆栈内，直接报错 */
    if (!IN_PROCESS_STACK_RANGE(addr))
        vm_error("%ul not in process range", addr);

    var = calloc(1, sizeof (var[0]));
    if (!var)
        vm_error("stack_var calloc failure");

    var->top = addr;
    var->t = SYS_REG_NUM + emu->tvar_id++;

    dynarray_add(&emu->tvars, var);

    return var;
}

static struct emu_temp_var *emu_find_temp_var(struct arm_emu *emu, unsigned long addr)
{
    struct emu_temp_var *tvar;
    int i;

    for (i = emu->tvars.len - 1; i >= 0; i--) {
        tvar = emu->tvars.ptab[i];
        if (tvar->top == addr)
            return tvar;
    }

    return NULL;
}

static struct emu_temp_var* emu_stack_push(struct arm_emu *emu, int val)
{

    int top = MEM_STACK_TOP(emu);;

    if (top >= emu->stack.size)
        vm_error("arm emulator stack size overflow");

    emu->regs[ARM_REG_SP] -= 4;

    *(int *)(emu->stack.base - MEM_STACK_TOP(emu)) = val;

    return emu_alloc_temp_var(emu, ARM_SP_VAL(emu));
}

static struct emu_temp_var* emu_stack_top(struct arm_emu *emu)
{
    int top = MEM_STACK_TOP(emu);
    
    if ((top == 0) || !emu->tvars.len)
        vm_error("arm emulator stack empty");

    return emu->tvars.ptab[emu->tvars.len - 1];
}

static void emu_stack_pop(struct arm_emu *emu)
{
    int top = MEM_STACK_TOP(emu);

    if ((top == 0) || !emu->tvars.len)
        vm_error("arm emulator stack empty");

    free(emu->tvars.ptab[emu->tvars.len - 1]);
    emu->tvars.len--;

    emu->regs[ARM_REG_SP] += 4;
}

static int t1_inst_lsl(struct arm_emu *emu, struct minst *minst, uint16_t *code, int len)
{
    return 0;
}

static int t1_inst_lsr(struct arm_emu *emu, struct minst *minst, uint16_t *code, int len)
{
    return 0;
}

static int t1_inst_asr(struct arm_emu *emu, struct minst *minst, uint16_t *code, int len)
{
    return 0;
}

static int thumb_inst_push(struct arm_emu *emu, struct minst *minst, uint16_t *code, int len)
{
    char buf[32];
    int i, reglist;

    if (len == 1) {
        reglist = emu->code.ctx.register_list | (emu->code.ctx.m << ARM_REG_LR);
        arm_prepare_dump(emu, "push %s", reglist2str(reglist, buf));
    } else {
        reglist = emu->code.ctx.register_list | (emu->code.ctx.m << ARM_REG_LR);
        arm_prepare_dump(emu, "push.w %s", reglist2str(emu->code.ctx.register_list, buf));
    }

    for (i = 0; i < 16; i++) {
        if (reglist & (1 << i))
            live_use_set(&emu->mblk, i);
    }

#if 0
    struct emu_temp_var *var;
    for (i = 0; i < 16; i++) {
        if (reglist & (1 << i)) {
            var = emu_stack_push(emu, emu->regs[i]);
            live_def_set(&emu->mblk, var->t);
        }
    }
#endif

    return 0;
}

static int thumb_inst_pop(struct arm_emu *emu, struct minst *minst, uint16_t *code, int len)
{
    char buf[32];
    int reglist, i;

    if (len == 2) {
        reglist = code[1] & 0xdfff;
        arm_prepare_dump(emu, "pop%s.w %s", cur_inst_it_cond(emu), reglist2str(reglist, buf));
    }
    else {
        reglist = emu->code.ctx.register_list | (emu->code.ctx.m << ARM_REG_PC);
        arm_prepare_dump(emu, "pop%s %s", cur_inst_it_cond(emu), reglist2str(reglist, buf));
    }

    for (i = 0; i < 16; i++) {
        if (reglist & (1 << i)) {
            live_def_set(&emu->mblk, i);
        }
    }

#if 0
    struct emu_temp_var *var;
    if (ConditionPassed(emu)) {
        for (i = 0; i < 16; i++) {
            if (reglist && (1 << i)) {
                /* FIXME: */
                var = emu_stack_top(emu);
                live_use_set(&emu->mblk, var->t);
                emu_stack_pop(emu);
            }
        }
    }
#endif

    return 0;
}

static int t1_inst_pop(struct arm_emu *emu, struct minst *minst, uint16_t *code, int len)
{
    return 0;
}

static int t1_inst_add1(struct arm_emu *emu, struct minst *minst, uint16_t *code, int len)
{
    return 0;
}

static int t1_inst_add(struct arm_emu *emu, struct minst *minst, uint16_t *code, int len)
{
    int lm, imm32;
    if (((code[0] >> 12) & 0xf) == 0b1010) {
        arm_prepare_dump(emu, "add %s, sp, #0x%x", regstr[emu->code.ctx.ld], imm32 = emu->code.ctx.imm * 4);
        liveness_set2(&emu->mblk, emu->code.ctx.ld, emu->code.ctx.ld, ARM_REG_SP);
        lm = ARM_REG_SP;
    }
    else {
        arm_prepare_dump(emu, "add %s, %s", regstr[emu->code.ctx.ld], regstr[lm = emu->code.ctx.lm]);
        liveness_set2(&emu->mblk, emu->code.ctx.ld, emu->code.ctx.ld, emu->code.ctx.lm);
    }

    if (ConditionPassed(emu) && emu->code.ctx.setflags) {
        live_def_set(&emu->mblk, ARM_REG_APSR);
    }

    if (IS_DISABLE_EMU(emu))
        return 0;

    /*add sp */
    if (((code[0] >> 12) & 0xf) == 0b1010) {
        if (ConditionPassed(emu)) {
            struct bits b = AddWithCarry(ARM_SP_VAL(emu), imm32, 0);
            emu->regs[emu->code.ctx.ld] = b.v;

            if (emu->code.ctx.setflags) {
                ARM_APSR_PTR(emu)->n = INT_TOPMOSTBIT(b.v);
                ARM_APSR_PTR(emu)->z = IsZeroBit(b.v);
                ARM_APSR_PTR(emu)->c = b.carry_out;
                ARM_APSR_PTR(emu)->v = b.overflow;
            }
        }
    }
    else {
    }

    return 0;
}

static int t1_inst_sub(struct arm_emu *emu, struct minst *minst, uint16_t *code, int len)
{
    return 0;
}

/* P454 */
static int thumb_inst_sub(struct arm_emu *emu, struct minst *minst, uint16_t *code, int len)
{
    arm_prepare_dump(emu, "sub%s sp, sp, #0x%x", cur_inst_it_cond(emu), emu->code.ctx.imm * 4);

    emu->code.ctx.ld = ARM_REG_SP;
    liveness_set(&emu->mblk, emu->code.ctx.ld, emu->code.ctx.ld);
    if (emu->code.ctx.setflags)
        live_def_set(&emu->mblk, ARM_REG_APSR);

    if (IS_DISABLE_EMU(emu))
        return 0;

    if (ConditionPassed(emu)) {
        struct bits bits = AddWithCarry(ARM_SP_VAL(emu), NOT(emu->code.ctx.imm * 4), 1);

        emu->regs[emu->code.ctx.ld] = bits.v;

        if (emu->code.ctx.setflags) {
            ARM_APSR_PTR(emu)->n = INT_TOPMOSTBIT(bits.v);
            ARM_APSR_PTR(emu)->z = IsZeroBit(bits.v);
            ARM_APSR_PTR(emu)->c = bits.carry_out;
            ARM_APSR_PTR(emu)->v = bits.overflow;
        }
    }

    REG_SET_KNOWN(emu, emu->code.ctx.ld);

    return 0;
}

static int thumb_inst_sub_reg(struct arm_emu *emu, struct minst *minst, uint16_t *code, int len)
{
    arm_prepare_dump(emu, "sub%s %s, %s, %s", InITBlock(emu) ? "c":cur_inst_it_cond(emu), 
        regstr[emu->code.ctx.ld], regstr[emu->code.ctx.ln], regstr[emu->code.ctx.lm]);

    liveness_set2(&emu->mblk, emu->code.ctx.ld, emu->code.ctx.ln, emu->code.ctx.lm);

    emu->code.ctx.setflags = !InITBlock(emu);
    if (emu->code.ctx.setflags)
        live_def_set(&emu->mblk, ARM_REG_APSR);

    return 0;
}

static int t1_inst_mov(struct arm_emu *emu, struct minst *minst, uint16_t *code, int len)
{
    return 0;
}

static int t1_inst_mov_0100(struct arm_emu *emu, struct minst *minst, uint16_t *code, int len)
{
    arm_prepare_dump(emu, "mov%s %s, %s", cur_inst_it_cond(emu), regstr[emu->code.ctx.ld], regstr[emu->code.ctx.lm]);

    if (EMU_IS_CONST_MODE(emu)) {
        struct minst *const_minst = minst_get_last_const_definition(&emu->mblk, minst, emu->code.ctx.lm);
        if (const_minst) {
            minst_set_const(minst, const_minst->ld_imm);
        }
    }

    return 0;
}

static int t1_inst_mov1(struct arm_emu *emu, struct minst *minst, uint16_t *code, int len)
{
    arm_prepare_dump(emu, "mov%s %s, #0x%x",  InITBlock(emu)? "c":cur_inst_it_cond(emu), regstr[emu->code.ctx.ld], emu->code.ctx.imm);

    if (!minst->flag.in_it_block)
        minst_set_const(minst, emu->code.ctx.imm);


    return 0;
}

static int t1_inst_cmp_imm(struct arm_emu *emu, struct minst *minst, uint16_t *code, int len)
{
    arm_prepare_dump(emu, "cmp %s, 0x%x", regstr[emu->code.ctx.lm], emu->code.ctx.imm);

    liveness_set(&emu->mblk, ARM_REG_APSR, emu->code.ctx.lm);

    return 0;
}

static int thumb_inst_cmp(struct arm_emu *emu, struct minst *minst, uint16_t *code, int len)
{
    arm_prepare_dump(emu, "cmp %s, %s", regstr[emu->code.ctx.ln], regstr[emu->code.ctx.lm]);

    live_def_set(&emu->mblk, ARM_REG_APSR);

    if (EMU_IS_CONST_MODE(emu)) {
        struct minst *ln_minst = minst_get_last_const_definition(&emu->mblk, minst, emu->code.ctx.ln);
        struct minst *lm_minst = minst_get_last_const_definition(&emu->mblk, minst, emu->code.ctx.lm);

        if (!ln_minst || !ln_minst->flag.is_const || !lm_minst || !lm_minst->flag.is_const)
            return 0;

        struct bits bs = AddWithCarry(ln_minst->ld_imm, ~lm_minst->ld_imm, 1);

        minst->apsr.n = INT_TOPMOSTBIT(bs.v);
        minst->apsr.z = IsZeroBit(bs.v);
        minst->apsr.c = bs.carry_out;
        minst->apsr.v = bs.overflow;
        minst->flag.is_const = 1;
    }

    return 0;
}

static int t1_inst_and(struct arm_emu *emu, struct minst *minst, uint16_t *code, int len)
{
    return 0;
}

static int thumb_inst_it(struct arm_emu *emu, struct minst *minst, uint16_t *code, int len)
{
    int firstcond = emu->code.ctx.cond;
    int mask = emu->code.ctx.imm;

    if (0 == mask) ARM_UNPREDICT();
    if (0xf == firstcond) ARM_UNPREDICT();
    if ((0xe == firstcond) && (BitCount(mask) != 1)) ARM_UNPREDICT();
    if (InITBlock(emu)) ARM_UNPREDICT();

    arm_prepare_dump(emu, "i%s %s", it2str(firstcond, mask, emu->it.et), condstr[firstcond]);

    emu->it.cond = firstcond;
    emu->it.inblock = 5 - RightMostBitPos(mask, 4);
    emu->it.num = emu->it.inblock - 1;

    live_use_set(&emu->mblk, ARM_REG_APSR);

    return 0;
}

static int t1_inst_eor(struct arm_emu *emu, struct minst *minst, uint16_t *code, int len)
{
    return 0;
}

static int t1_inst_adc(struct arm_emu *emu, struct minst *minst, uint16_t *code, int len)
{
    return 0;
}

static int t1_inst_sbc(struct arm_emu *emu, struct minst *minst, uint16_t *code, int len)
{
    return 0;
}

static int t1_inst_ror(struct arm_emu *emu, struct minst *minst, uint16_t *code, int len)
{
    return 0;
}

static int t1_inst_tst(struct arm_emu *emu, struct minst *minst, uint16_t *code, int len)
{
    return 0;
}

static int t1_inst_neg(struct arm_emu *emu, struct minst *minst, uint16_t *code, int len)
{
    return 0;
}

static int t1_inst_cmn(struct arm_emu *emu, struct minst *minst, uint16_t *code, int len)
{
    return 0;
}

static int t1_inst_orr(struct arm_emu *emu, struct minst *minst, uint16_t *code, int len)
{
    return 0;
}

static int t1_inst_mul(struct arm_emu *emu, struct minst *minst, uint16_t *code, int len)
{
    return 0;
}

static int t1_inst_bic(struct arm_emu *emu, struct minst *minst, uint16_t *code, int len)
{
    return 0;
}

static int t1_inst_mvn(struct arm_emu *emu, struct minst *minst, uint16_t *code, int len)
{
    return 0;
}

static int t1_inst_cpy(struct arm_emu *emu, struct minst *minst, uint16_t *code, int len)
{
    return 0;
}

/* */
static unsigned long emu_addr_target2host(struct arm_emu *emu, unsigned long addr)
{
    return 0;
}

static unsigned long emu_addr_host2target(struct arm_emu *emu, unsigned long addr)
{
    addr -= emu->baseaddr;

    return (unsigned long)emu->code.data + addr;
}

static int thumb_inst_ldr(struct arm_emu *emu, struct minst *minst, uint16_t *code, int len)
{
    int sigcode = (code[0] >> 12) & 0xf, lm;
    unsigned long addr = 0;

    if (len == 1) {
        if (sigcode == 0b0100) {
            arm_prepare_dump(emu, "ldr %s, [pc, #0x%x] ", regstr[emu->code.ctx.ld], emu->code.ctx.imm * 4);

            liveness_set(&emu->mblk, emu->code.ctx.ld, ARM_REG_PC);
        }
        else if (sigcode == 0b1001) {
            if (emu->code.ctx.imm)
                arm_prepare_dump(emu, "ldr %s, [sp,#0x%x]", regstr[emu->code.ctx.ld], emu->code.ctx.imm * 4);
            else
                arm_prepare_dump(emu, "ldr %s, [sp]");

            addr = ARM_SP_VAL(emu) + emu->code.ctx.imm * 4;
            lm = ARM_REG_SP;
        }
        else {
            if (emu->code.ctx.imm)
                arm_prepare_dump(emu, "ldr %s, [%s, #0x%x] ", regstr[emu->code.ctx.ld], regstr[emu->code.ctx.lm], emu->code.ctx.imm * 4);
            else
                arm_prepare_dump(emu, "ldr %s, [%s] ", regstr[emu->code.ctx.ld], regstr[emu->code.ctx.lm]);

            addr = emu->regs[emu->code.ctx.lm] + emu->code.ctx.imm * 4;
            lm = emu->code.ctx.lm;
        }
    }
    else { 
        if (emu->code.ctx.lm == 15) {
            arm_prepare_dump(emu, "ldr.w %s, [pc, #0x%c%x]", regstr[emu->code.ctx.ld], emu->code.ctx.u ? '+':'-', emu->code.ctx.imm);
            liveness_set(&emu->mblk, emu->code.ctx.ld, ARM_REG_PC);
        }
        else {
            arm_prepare_dump(emu, "ldr.w %s, [%s,0x%x]", regstr[emu->code.ctx.ld], regstr[emu->code.ctx.lm], emu->code.ctx.imm);

            addr = emu->regs[emu->code.ctx.lm] + emu->code.ctx.imm;
            lm = emu->code.ctx.lm;
        }
    }

    if (IS_DISABLE_EMU(emu))
        return 0;
    
#if 0
    if (addr) {
        struct emu_temp_var *var;
        if ((var = emu_find_temp_var(emu, addr))) 
            liveness_set(&emu->mblk, emu->code.ctx.ld, var->t);
        else 
            liveness_set(&emu->mblk, emu->code.ctx.ld, lm);
    }
#endif

    return 0;
}

static int t1_inst_str_01101(struct arm_emu *emu, struct minst *minst, uint16_t *code, int len)
{
    return 0;
}

static int t1_inst_str_10010(struct arm_emu *emu, struct minst *minst, uint16_t *code, int len)
{
    if (emu->code.ctx.imm) 
        arm_prepare_dump(emu, "str %s, [sp,#0x%x]", regstr[emu->code.ctx.lm], emu->code.ctx.imm * 4);
    else
        arm_prepare_dump(emu, "str %s, [sp]", regstr[emu->code.ctx.lm]);

    if (IS_DISABLE_EMU(emu))
        return 0;

#if 0
    unsigned long addr;
    struct emu_temp_var *var;
    addr = ARM_SP_VAL(emu) + emu->code.ctx.imm * 4;
    if ((var = emu_find_temp_var(emu, addr)))
        liveness_set(&emu->mblk, var->t, emu->code.ctx.lm);
    else if (IN_PROCESS_STACK_RANGE(addr)) {
        var = emu_alloc_temp_var(emu, addr);
        liveness_set(&emu->mblk, var->t, emu->code.ctx.lm);
    }
    else
        liveness_set(&emu->mblk, -1, emu->code.ctx.lm);
#endif

    return 0;
}

static int thumb_inst_mov(struct arm_emu *emu, struct minst *minst, uint16_t *inst, int inst_len)
{
    int imm = BITS_GET_SHL(inst[0], 10, 1, 11) + BITS_GET_SHL(inst[0], 0, 4, 12) + BITS_GET_SHL(inst[1], 12, 3, 8) + BITS_GET_SHL(inst[1], 0, 8, 0);
    int is_movt = 0;

    if ((inst[0] >> 7) & 1) {
        arm_prepare_dump(emu, "movt%s %s, #0x%x", cur_inst_it_cond(emu), regstr[emu->code.ctx.ld], imm);
        liveness_set(&emu->mblk, emu->code.ctx.ld, emu->code.ctx.ld);

        if (EMU_IS_CONST_MODE(emu) && !minst->flag.in_it_block) {
            struct minst *cminst = minst_get_last_const_definition(&emu->mblk, minst, emu->code.ctx.ld);
            if (cminst)
                minst_set_const(minst, imm << 16 | (cminst->ld_imm & 0xffff));
        }
    }
    else {
        arm_prepare_dump(emu, "mov%sw %s, #0x%x", cur_inst_it_cond(emu), regstr[emu->code.ctx.ld], imm);

        if (!minst->flag.in_it_block) 
            minst_set_const(minst, imm);
    }

    return 0;
}

static int t1_inst_mov_w(struct arm_emu *emu, struct minst *minst, uint16_t *inst, int inst_len)
{
    int imm = BITS_GET_SHL(inst[0], 10, 1, 11) + BITS_GET_SHL(inst[1], 12, 3, 8) + BITS_GET_SHL(inst[1], 0, 8, 0), imm1;

    arm_prepare_dump(emu, "mov%s.w %s, #0x%x", cur_inst_it_cond(emu), regstr[emu->code.ctx.ld], imm1 = ThumbExpandImmWithC(emu, imm));

    if (!minst->flag.in_it_block)
        minst_set_const(minst, imm1);

    return 0;
}

static int t1_inst_bx_0100(struct arm_emu *emu, struct minst *minst, uint16_t *code, int len)
{
    arm_prepare_dump(emu, "bx %s", regstr[emu->code.ctx.lm]);

    return 0;
}

static int t1_inst_blx_0100(struct arm_emu *emu, struct minst *minst, uint16_t *code, int len)
{
    int i;
    arm_prepare_dump(emu, "blx %s", regstr[emu->code.ctx.lm]);

    live_def_set(&emu->mblk, ARM_REG_R0);
    for (i = 0; i < 4; i++)
        live_use_set(&emu->mblk, ARM_REG_R0 + i);

    return 0;
}

static int t1_inst_b(struct arm_emu *emu, struct minst *minst, uint16_t *code, int len)
{
    arm_prepare_dump(emu, "b 0x%x", minst->host_addr = (ARM_PC_VAL(emu) + SignExtend(emu->code.ctx.imm, 11) * 2));

    if (IS_DISABLE_EMU(emu))
        return 0;

    minst->flag.b = 1;
    minst->flag.b_al = 1;
    if (!minst->flag.b_need_fixed)
        minst->flag.b_need_fixed = 1;

    return 0;
}

static int thumb_inst_add_sp_imm(struct arm_emu *emu, struct minst *minst, uint16_t *code, int len)
{
    arm_prepare_dump(emu, "add%s sp, #0x%x", cur_inst_it_cond(emu), emu->code.ctx.imm * 4);

    liveness_set(&emu->mblk, ARM_REG_SP, ARM_REG_SP);

    return 0;
}

static int t_swi(struct arm_emu *emu, struct minst *minst, uint16_t *code, int len)
{
    return 0;
}

static int thumb_inst_b(struct arm_emu *emu, struct minst *minst, uint16_t *code, int len)
{
    if (emu->code.ctx.cond == 0xe)
        ARM_UNPREDICT();
    else if (emu->code.ctx.cond == 0xf)
        return t_swi(emu, minst, code, len);

    minst->host_addr = (ARM_PC_VAL(emu) + SignExtend(emu->code.ctx.imm, 8) * 2);
    arm_prepare_dump(emu, "b%s 0x%x", minst->flag.is_const ? "":condstr[emu->code.ctx.cond], minst->flag.b_cond_passed ? minst->host_addr:0);

    if (InITBlock(emu))
        ARM_UNPREDICT();

    live_use_set(&emu->mblk, ARM_REG_APSR);

    minst->flag.b = 1;
    minst->flag.b_al = (emu->code.ctx.cond == ARM_COND_AL);
    if (!minst->flag.b_need_fixed)
        minst->flag.b_need_fixed = 1;

    if (EMU_IS_CONST_MODE(emu)) {
        struct minst *cminst = minst_get_last_const_definition(&emu->mblk, minst, ARM_REG_APSR);
        struct minst *tminst;

        if (cminst->flag.is_const) {
            if (minst->flag.b_cond_passed = _ConditionPassed(&minst->apsr, emu->code.ctx.cond)) {
                tminst = emu->mblk.allinst.ptab[minst->id + 1];
            }
            else {
                tminst = minst_blk_find(&emu->mblk, emu_addr_host2target(emu, minst->host_addr));
                if (!tminst)
                    vm_error("inst cmp not found host addr");
            }

            minst_succ_del(minst, tminst);
            minst_pred_del(tminst, minst);

            minst->flag.is_const = 1;
        }
    }

    return 0;
}

static int thumb_inst_ldr_reg(struct arm_emu *emu, struct minst *minst, uint16_t *code, int len)
{
    return 0;
}

static int thumb_inst_blx(struct arm_emu *emu, struct minst *minst, uint16_t *code, int len)
{
    if (InITBlock(emu) && !LastInITBlock(emu)) ARM_UNPREDICT();

    arm_prepare_dump(emu, "blx");

    emu->meet_blx = 1;

    return 0;
}

/*

definition about inst reg rule
o[num]:      operate 
i[num]:      immediate
m[num]:      link register
rl[num]:     register list
lm[num]:     src register
hm[num]:     high register
*/
struct arm_inst_desc {
    const char *regexp;
    arm_inst_func    funclist[4];
    const char        *desc[4];
} desclist[]= {
    {"0000    o1    i5  lm3 ld3",           {t1_inst_lsl, t1_inst_lsr}, {"lsl", "lsr"}},
    {"0001    0     i5  lm3 ld3",           {t1_inst_asr}, {"asr"}},
    {"0001    10 o1 lm3 ln3 ld3",           {t1_inst_add, thumb_inst_sub_reg}, {"add", "sub"}},
    {"0001    11 o1 i3  ln3 ld3",           {t1_inst_add, t1_inst_sub}, {"add", "sub"}},
    {"0010    0 ld3    i8",                 {t1_inst_mov1}, {"mov"}},
    {"0010    1 lm3    i8",                 {t1_inst_cmp_imm}, {"cmp"}},
    {"0011    o1 ld3    i8",                {t1_inst_add, t1_inst_sub}, {"add", "sub"}},
    {"0100    0000 o2 lm3 ld3",             {t1_inst_and, t1_inst_eor, t1_inst_lsl, t1_inst_lsr}, {"and", "eor", "lsl2", "lsr2"}},
    {"0100    0001 o2 lm3 ld3",             {t1_inst_asr, t1_inst_adc, t1_inst_sbc, t1_inst_ror}, {"asr", "adc", "sbc", "ror"}},
    {"0100    0010 0o1 lm3 ld3",            {t1_inst_tst, t1_inst_neg}, {"tst", "neg"}},
    {"0100    0010 1o1 lm3 ln3",            {thumb_inst_cmp, t1_inst_cmn}, {"cmp", "cmn"}},
    {"0100    0011 o2 lm3 ld3",             {t1_inst_orr, t1_inst_mul, t1_inst_bic, t1_inst_mvn}, {"orr", "mul", "bic", "mvn"}},
    {"0100    0110 00 lm3 ld3",             {t1_inst_mov_0100}, {"mov"}},
    {"0100    01 o1 0 01 hm3 ld3",          {t1_inst_add, t1_inst_mov_0100}, {"add", "mov"}},
    {"0100    01 o1 0 10 lm3 hd3",          {t1_inst_add, t1_inst_mov_0100}, {"add", "mov"}},
    {"0100    01 o1 0 11 hm3 hd3",          {t1_inst_add, t1_inst_mov_0100}, {"add", "mov"}},
    {"0100    0101 01 lm3 hn3 ",            {thumb_inst_cmp}, {"cmp"}},
    {"0100    0101 10 hm3 ln3 ",            {thumb_inst_cmp}, {"cmp"}},
    {"0100    0101 11 hm3 hn3 ",            {thumb_inst_cmp}, {"cmp"}},
    {"0100    1 ld3 i8",                    {thumb_inst_ldr}, {"ldr"}},
    {"0100    0111 o1 lm4 000",             {t1_inst_bx_0100, t1_inst_blx_0100}, {"bx", "blx"}},
    {"0110    o1 i5 lm3 ld3",               {t1_inst_str_01101, thumb_inst_ldr},      {"str", "ldr"}},
    {"1001    0 lm3 i8",                    {t1_inst_str_10010}, {"str"}},
    {"1001    1 ld3 i8",                    {thumb_inst_ldr}, {"ldr"}},
    {"1010    o1 ld3 i8",                   {t1_inst_add1, t1_inst_add}, {"add", "add"}},
    {"1011    o1 10 m1 rl8",                {thumb_inst_push, thumb_inst_pop}, {"push", "pop"}},
    {"1011    0000 o1 i7",                  {thumb_inst_add_sp_imm, thumb_inst_sub}, {"add", "sub"}},
    {"1011    1111 c4 i4",                  {thumb_inst_it}, {"it"}},
    {"1101    c4 i8",                       {thumb_inst_b}, {"b<cond>"}},
    {"1110    0 i11",                       {t1_inst_b}, {"b"}},

    {"1110 1001 0010 1101 0 m1 0 rl13",     {thumb_inst_push}, "push.w"},
    {"1110 1000 1011 1101 i1 m1 0 rl13",    {thumb_inst_pop}, "pop.w"},

    {"1111 0i1 i10 11 i1 0 i1 i10 0",       {thumb_inst_blx}, "blx"},

    {"1111 0i1 00010 s1 11110 i3 ld4 i8",   {t1_inst_mov_w}, "mov.w"},
    {"1111 0i1 101100 i4 0 i3 ld4 i8",      {thumb_inst_mov}, "movt"},
    {"1111 0i1 100100 i4 0 i3 ld4 i8",      {thumb_inst_mov}, "movw"},
    {"1111 1000 u1 101 lm4 ld4 i12",        {thumb_inst_ldr}, {"ldr.w"}},
};

static int init_inst_map = 0;

struct reg_node {
    int id;
    struct reg_node *parent;
    struct reg_node *childs[3];

    arm_inst_func func;
    struct dynarray set;
    char *exp;
    char *desc;
};

struct reg_tree {
    int counts;
    struct dynarray arr;
    struct reg_node root;
};

struct arm_inst_engine {
    struct reg_tree    enfa;
    struct reg_tree    dfa;

    int width;
    int height;
    int *trans2d;
};

struct reg_node *reg_node_new(struct reg_tree *tree, struct reg_node *parent)
{
    struct reg_node *node = calloc(1, sizeof (node[0]));

    if (!node)
        vm_error("reg_node_new() failed calloc");

    node->parent = parent;
    node->id = ++tree->counts;
    dynarray_add(&tree->arr, node);

    return node;
}

void reg_node_copy(struct reg_node *dst, struct reg_node *src)
{
    dst->func = src->func;
    dst->exp = strdup(src->exp);
    dst->desc = strdup(src->desc);
}

int reg_node_height(struct reg_node *node)
{
    int i = 0;
    while (node->parent) {
        i++;
        node = node->parent;
    }

    return i;
}

void    reg_node_delete(struct reg_node *root)
{
    int i;
    if (!root)
        return;

    for (i = 0; i < count_of_array(root->childs); i++) {
        reg_node_delete(root->childs[i]);
    }

    if (root->exp)    free(root->exp);
    if (root->desc) free(root->desc);

    free(root);
}

void reg_node__dump_dot(FILE *fp, struct reg_tree *tree)
{
    struct reg_node *node, *root;
    int i, j;

    for (j = 0; j < tree->arr.len; j++) {
        root = (struct reg_node *)tree->arr.ptab[j];

        for (i = 0; i < count_of_array(root->childs); i++) {
            if (!root->childs[i])
                continue;

            fprintf(fp, "%d -> %d [label = \"%d\"]\n", root->id, root->childs[i]->id, i);
        }

        if (root->set.len) {
            fprintf(fp, "%d [label=\"%d (", root->id, root->id);
            for (i = 0; i < root->set.len; i++) {
                node = (struct reg_node *)root->set.ptab[i];
                fprintf(fp, "%d ", node->id);
            }
            fprintf(fp, ")\"]\n");
        }


        if (root->func) {
            fprintf(fp, "%d [shape=Msquare, label=\"%s\"];\n", root->id, root->desc);
        }
    }
}

void reg_node_dump_dot(const char *filename, struct reg_tree *tree)
{
    FILE *fp = fopen(filename, "w");

    fprintf(fp, "digraph G {\n");

    reg_node__dump_dot(fp, tree);

    fprintf(fp, "%d [shape=Mdiamond];\n", tree->root.id);

    fprintf(fp, "}\n");

    fclose(fp);
}

struct arm_inst_engine *inst_engine_create()
{
    struct arm_inst_engine *engine = NULL;

    engine = calloc(1, sizeof(engine[0]));
    if (!engine)
        vm_error("failed calloc()");

    return engine;
}

int arm_insteng_add_exp(struct arm_inst_engine *en, const char *exp, const char *desc, arm_inst_func func)
{
    struct reg_node *root = &en->enfa.root;
    int j, len = strlen(exp), rep, height, idx;
    const char *s = exp;

    for (; *s; s++) {
        if (isblank(*s))    continue;
        switch (*s) {
        case '0':
        case '1':
            idx = *s - '0';
            if (!root->childs[idx]) {
                root->childs[idx] = reg_node_new(&en->enfa, root);
            }

            root = root->childs[idx];
            break;

            /* setflags */
        case 's':
            /* immediate */
        case 'i':
            /* condition */
        case 'c':

            /* more register，一般是对寄存器列表的补充 */
        case 'm':
        case 'u':
loop_label:
            rep = atoi(&s[1]);
            if (!rep) 
                goto fail_label;

            for (j = 0; j < rep; j++) {
                if (!root->childs[2])
                    root->childs[2] = reg_node_new(&en->enfa, root);
                root = root->childs[2];
            }

            while (s[1] && isdigit(s[1])) s++;
            break;

        case 'r':
            if (s[1] != 'l')
                goto fail_label;

            s++;
            goto loop_label;

            /* register */
        case 'l':
            if (s[1] != 'm' && s[1] != 'd' && s[1] != 'n')
                goto fail_label;

            s++;
            goto loop_label;

        case 'h':
            if (s[1] != 'm' && s[1] != 'd' && s[1] != 'n')
                goto fail_label;

            s++;
            goto loop_label;

    fail_label:
        default:
            vm_error("inst expression [%s], position[%d:%s]\n", exp, (s - exp), s);
            break;
        }
    }

    root->exp = strdup(exp);
    root->func = func;
    root->desc = strdup(desc);

    // FIXME:测试完毕可以关闭掉，验证层数是否正确
    height = reg_node_height(root);
    if (height != 16 && height != 32) {
        vm_error("inst express error[%s], not size 16 or 32\n", exp);
    }

    return 0;
}

static struct arm_inst_engine *g_eng = NULL;

static struct reg_node *reg_node_find(struct reg_tree *tree, struct dynarray *arr)
{
    struct reg_node *node;
    int i;
    for (i = 0; i < tree->arr.len; i++) {
        node = (struct reg_node *)tree->arr.ptab[i];
        if (0 == dynarray_cmp(arr, &node->set))
            return node;
    }

    return NULL;
}

static int arm_insteng_gen_dfa(struct arm_inst_engine *eng)
{
    struct reg_node *nfa_root = &eng->enfa.root;

    struct reg_node *dfa_root = &eng->dfa.root;
    struct reg_node *node_stack[512], *droot, *nroot, *dnode1, *nnode;
    struct dynarray set = {0};
    int stack_top = -1, i, j, need_split;

#define istack_push(a)            (node_stack[++stack_top] = a)
#define istack_pop()            node_stack[stack_top--]        
#define istack_is_empty()        (stack_top == -1)

    dynarray_add(&dfa_root->set, nfa_root);
    istack_push(dfa_root);
    dynarray_reset(&set);

    while (!istack_is_empty()) {
        droot = istack_pop();

        for (i = need_split = 0; i < droot->set.len; i++) {
            nroot = (struct reg_node *)droot->set.ptab[i];
            if (nroot->childs[0] || nroot->childs[1]) {
                need_split = 1;
                break;
            }
        }

        for (i = need_split ? 0:2; i < (3 - need_split); i++) {
            dnode1 = NULL;

            for (j = 0; j < droot->set.len; j++) {
                if (!(nroot = (struct reg_node *)droot->set.ptab[j]))
                    continue;

                if (nroot->childs[i])    dynarray_add(&set, nroot->childs[i]);
                if ((i != 2) && nroot->childs[2])    dynarray_add(&set, nroot->childs[2]);
            }

            if (!dynarray_is_empty(&set)) {
                dnode1 = reg_node_find(&eng->dfa, &set);
                if (!dnode1) {
                    dnode1 = reg_node_new(&eng->dfa, droot);
                    dynarray_copy(&dnode1->set, &set);
                    istack_push(dnode1);
                }

                droot->childs[i] = dnode1;
            }

            /* 检查生成的DFA节点中包含的NFA节点，是否有多个终结态，有的话报错，没有的话把NFA的
            状态拷贝到DFA中去 */
            if (dnode1) {
                for (j = 0; j < dnode1->set.len; j++) {
                    if (!(nnode = (struct reg_node *)dnode1->set.ptab[j]) || !nnode->func)    continue;

                    if (dnode1->func) {
                        if (!strcmp(dnode1->desc, nnode->desc))
                            continue;

                        vm_error("conflict end state[%s] [%s]\n", dnode1->desc, nnode->desc);
                    }

                    reg_node_copy(dnode1, nnode);
                }
            }

            dynarray_reset(&set);
        }
    }

    return 0;
}

static int arm_insteng_gen_trans2d(struct arm_inst_engine *eng)
{
    int i;
    struct reg_tree *tree = &eng->dfa;
    struct reg_node *node;
    eng->width = eng->dfa.counts;
    eng->height = 2;

    eng->trans2d = calloc(1, eng->width * eng->height * sizeof (eng->trans2d[0]));
    if (!eng->trans2d)
        vm_error("trans2d calloc failure");

    for (i = 0; i < tree->arr.len; i++) {
        node = (struct reg_node *)tree->arr.ptab[i];

        if (node->childs[2]) {
            eng->trans2d[eng->width + node->id] = node->childs[2]->id;
            eng->trans2d[node->id] = node->childs[2]->id;
            continue;
        }

        if (node->childs[0]) {
            eng->trans2d[node->id] = node->childs[0]->id;
        }

        if (node->childs[1]) {
            eng->trans2d[eng->width + node->id] = node->childs[1]->id;
        }
    }

    return 0;
}

static int arm_insteng_init(struct arm_emu *emu)
{
    int i, j, k, m, n, len;
    const char *exp;
    char buf[128];

    if (g_eng)
        return 0;

    g_eng = calloc(1, sizeof (g_eng[0]));
    if (!g_eng)
        vm_error("arm_insteng_init() calloc failure");

    dynarray_add(&g_eng->enfa.arr, &g_eng->enfa.root);
    dynarray_add(&g_eng->dfa.arr, &g_eng->dfa.root);

    for (i = 0; i < count_of_array(desclist); i++) {
        exp = desclist[i].regexp;
        len = strlen(exp);
        for (j = k = 0; j < len; k++, j++) {
            if (exp[j] == 'o') {
                int bits = atoi(exp + j + 1);
                if (!bits)
                    vm_error("express error, unknown o token, %s\n", exp);

                while (isdigit(exp[++j]));

                int pow1 = (int)pow(2, bits);
                for (n = 0; n < pow1; n++) {
                    for (m = 0; m < bits; m++) {
                        buf[k + m] = !!(n & (1 <<  (bits - m - 1))) + '0';
                    }

                    strcpy(buf + k + bits, exp + j);
                    arm_insteng_add_exp(g_eng, buf, desclist[i].desc[n], desclist[i].funclist[n]);
                }
                break;
            }
            else {
                buf[k] = exp[j];
            }
        }
        buf[k] = 0;

        if (j == len)
            arm_insteng_add_exp(g_eng, buf, desclist[i].desc[0], desclist[i].funclist[0]);
    }

    if (emu->dump.nfa)
        reg_node_dump_dot("nfa.dot", &g_eng->enfa);

    arm_insteng_gen_dfa(g_eng);

    if (emu->dump.dfa)
        reg_node_dump_dot("dfa.dot", &g_eng->dfa);

    arm_insteng_gen_trans2d(g_eng);

    init_inst_map = 1;

    return 0;
}

static int arm_insteng_uninit()
{
}

static int arm_insteng_retrieve_context(struct arm_emu *emu, struct reg_node *reg_node, uint8_t *code, int code_len);

static void arm_inst_context_init(struct arm_inst_context *ctx)
{
    memset(ctx, 0, sizeof (ctx[0]));
    ctx->ld = -1;
    ctx->lm = -1;
    ctx->ln = -1;
}

static void arm_liveness_init(struct arm_emu *emu, struct minst *minst, struct arm_inst_context *ctx)
{
    if (ctx->ld >= 0) {
        live_def_set(&emu->mblk, ctx->ld);
    }

    if (ctx->lm >= 0) {
        live_use_set(&emu->mblk, ctx->lm);
    }

    if (ctx->ln >= 0) {
        live_use_set(&emu->mblk, ctx->ln);
    }

    if (InITBlock(emu)) {
        live_use_set(&emu->mblk, ARM_REG_APSR);
    }
}

static struct reg_node*     arm_insteng_parse(struct arm_emu *emu, uint8_t *code, int len, int *olen)
{
    struct reg_node *node = NULL, *p_end_node = NULL;
    int i, j, from = 0, to, bit;

    /* arm 解码时，假如第一个16bit没有解码到对应的thumb指令终结态，则认为是thumb32，开始进入
    第2轮的解码 */

    /* 搜索指令时采取最长匹配原则 */
    //printf("start state:%d\n", from);
    for (i = from = 0; i < 2; i++) {
        uint16_t inst = ((uint16_t *)code)[i];
        for (j = 0; j < 16; j++) {
            bit = !!(inst & (1 << (15 - j)));
            to = g_eng->trans2d[g_eng->width * bit + from];

            if (!to)
                goto exit;

            node = ((struct reg_node *)g_eng->dfa.arr.ptab[to]);
            from = node->id;

            //printf("%d ", from);
        }

        if (node->func)
            p_end_node = node;
    }
    exit:
    //printf("\n");

    node = p_end_node;

    if (!node || !node->func) {
        vm_error("arm_insteng_decode() meet unkown instruction, code[%02x %02x]", code[0], code[1]);
    }

    *olen = i;

    return node;
}

static int arm_minst_do(struct arm_emu *emu, struct minst *minst)
{
    struct reg_node *reg_node = minst->reg_node;

    if (minst->flag.prologue || minst->flag.epilogue)
        return 0;

    emu->regs[ARM_REG_PC] = emu->baseaddr + (minst->addr - emu->code.data) + 4;

    memcpy(emu->prev_regs, emu->regs, sizeof (emu->regs));

    /* 从指令中获取关键性上下文 */
    arm_insteng_retrieve_context(emu, reg_node, minst->addr, minst->len);

    arm_liveness_init(emu, minst, &emu->code.ctx);

    /* 不要挪动位置，假如你把他移动到 reg_node->func 后面，会导致 it 也被判断为在 it_block 中 */
    if (InITBlock(emu)) {
        minst->flag.in_it_block = 1;
    }

    reg_node->func(emu, minst, (uint16_t *)minst->addr, minst->len / 2);


    if (emu->it.inblock> 0)
        emu->it.inblock--;

    return 0;
}

static int arm_insteng_decode(struct arm_emu *emu, uint8_t *code, int len)
{
    int i;
    struct minst *minst;

    if (!g_eng)     arm_insteng_init(emu);

    struct reg_node *node = arm_insteng_parse(emu, code, len, &i);
    minst = minst_new(&emu->mblk, code, i * 2, node);

    arm_minst_do(emu, minst);

    if (!emu->prev_minst || !emu->prev_minst->flag.b || !emu->prev_minst->flag.b_al) {
        minst_succ_add(emu->prev_minst, minst);
        minst_pred_add(minst, emu->prev_minst);
    }

    emu->prev_minst = minst;

    return i * 2;
}

static int arm_insteng_retrieve_context(struct arm_emu *emu, struct reg_node *reg_node, uint8_t *code, int code_len)
{
    int i, len, c;
    char *exp = reg_node->exp;
    struct arm_inst_context *ctx = &emu->code.ctx;

    arm_inst_context_init(ctx);

    uint16_t inst = *(uint16_t *)code;

    i = 0;
    while(*exp) {
        switch ((c = *exp)) {
        case '0': case '1':
            exp++; i++; 
            break;
        case ' ':
            exp++;
            break;

        case 's':
        case 'i':
        case 'c':
        case 'u':
            len = atoi(++exp);

            if (c == 's')
                ctx->setflags = BITS_GET(inst, 16 - i - len, len);
            else if (c == 'i')
                ctx->imm = BITS_GET(inst, 16 -i - len, len);
            else if (c == 'u')
                ctx->u = BITS_GET(inst, 16 -i - len, len);
            else
                ctx->cond = BITS_GET(inst, 16 -i - len, len);
            while (isdigit(*exp)) exp++;
            i += len;
            break;


        case 'm':
            len = 1;
            ctx->m = BITS_GET(inst, 16 - i - len, len);
            exp += 2; i += len;
            break;

        case 'h':
        case 'l':
            exp++;
            len = atoi(exp + 1);
            switch (*exp++) {
            case 'n':
                ctx->ln = BITS_GET(inst, 16 - i - len, len) + (c == 'h') * 8;
                break;

            case 'm':
                ctx->lm = BITS_GET(inst, 16 - i - len, len) + (c == 'h') * 8;
                break;

            case 'd':
                ctx->ld = BITS_GET(inst, 16 - i - len, len) + (c == 'h') * 8;
                break;

            default:
                goto fail_label;
            }
            i += len;

            while (isdigit(*exp)) exp++;
            break;

        case 'r':
            exp++;
            if (*exp == 'l') {
                len = atoi(++exp);
                ctx->register_list |= BITS_GET(inst, 16 - i - len, len);
            }
            else
                goto fail_label;
            while (isdigit(*exp)) exp++;
            i += len;
            break;

        fail_label:
        default:
            vm_error("inst exp [%s], un-expect token[%s]\n", reg_node->exp, exp);
            break;
        }

        if ((i == 16) && (code_len == 4)) {
            i = 0;
            inst = *(uint16_t *)(code + 2);
        }
    }

    return 0;
}

static int arm_emu_cpu_reset(struct arm_emu *emu)
{
    memset(emu->regs, 0 , sizeof (emu->regs));
    memset(emu->prev_regs, 0, sizeof (emu->prev_regs));

    emu->regs[ARM_REG_SP] = PROCESS_STACK_BASE;
    emu->code.pos = 0;

    emu->known = 0;

    memset(&emu->it, 0, sizeof (emu->it));

    REG_SET_KNOWN(emu, ARM_REG_SP);
    REG_SET_KNOWN(emu, ARM_REG_PC);

    EMU_SET_SEQ_MODE(emu);

    return 0;
}

struct arm_emu   *arm_emu_create(struct arm_emu_create_param *param)
{
    struct arm_emu *emu;

    emu = calloc(1, sizeof (emu[0]));
    if (!emu)
        vm_error("arm_emu_create() failed with calloc");
    
    emu->code.data = param->code;
    emu->code.len = param->code_len;
    emu->baseaddr = param->baseaddr;
    emu->dump.cfg = 1;
    emu->dump.nfa = 1;
    emu->dump.dfa = 1;

    minst_blk_init(&emu->mblk, NULL);

    emu->stack.size = PROCESS_STACK_SIZE;
    emu->stack.data = calloc(1, emu->stack.size);
    if (NULL == emu->stack.data)
        vm_error("arm alloc stack size[%d] failure", emu->stack.size);

    emu->stack.base = emu->stack.data + emu->stack.size;

    arm_emu_cpu_reset(emu);

    return emu;
}

void        arm_emu_destroy(struct arm_emu *e)
{
    if (e->stack.data)
        free(e->stack.data);

    minst_blk_uninit(&e->mblk);
    free(e);
}

static int arm_emu_dump_mblk(struct arm_emu *emu)
{
    struct minst *minst;
    int i;
    char buf[4096];

    arm_emu_cpu_reset(emu);
    emu->decode_inst_flag = FLAG_DISABLE_EMU;
    for (i = 0; i < emu->mblk.allinst.len; i++) {
        minst = emu->mblk.allinst.ptab[i];

        arm_minst_do(emu, minst);

        arm_inst_print_format(emu, minst, -1, buf);

        printf("%s\n", buf);
    }

    return 0;
}

#define CFG_NODE_ID(addr)       (emu->baseaddr + (addr - emu->code.data))

static void arm_emu_dump_cfg(struct arm_emu *emu)
{
    int i, j;
    struct minst *minst, *prev_minst, *cur_grp_minst;
    struct minst_node *tnode;
    struct minst_blk *blk = &emu->mblk;
    char obuf[512];
    unsigned char *start;

    FILE *fp = fopen("cfg.dot", "w");

    fprintf(fp, "digraph G {");
    fprintf(fp, "node [fontname = \"helvetica\"]");

    arm_emu_cpu_reset(emu);
    for (i = 0; i < blk->allinst.len; i++) {
        minst = blk->allinst.ptab[i];

        if (0 == i) {
            cur_grp_minst = minst;
        }
        else if (prev_minst->flag.b || minst->preds.next || minst->preds.minst != blk->allinst.ptab[i - 1]) {
            cur_grp_minst = minst;
        }

        minst->cfg_node = cur_grp_minst;
        prev_minst = minst;
    }

    for (i = j =0; i < blk->allinst.len; i++) {
        minst = blk->allinst.ptab[i];

        /* 指令快刚进入的时候，必定生成一个节点 */
        if (0 == i) {
            start = minst->addr;
            fprintf(fp, "sub_%x [shape=MSquare, label=<<font color='red'><b>sub_%x</b></font><br/>", CFG_NODE_ID(minst->addr), CFG_NODE_ID(minst->addr));
        }
        /* 1. 物理前指令是跳转指令，自动切块
           2. 当前节点有多个前节点 
           3. 前节点不是物理前节点 */
        else if (prev_minst->flag.b || minst->preds.next || minst->preds.minst != blk->allinst.ptab[i - 1]) {
            fprintf(fp, ">]\n");

            for (tnode = &prev_minst->succs; tnode; tnode = tnode->next) {
                fprintf(fp, "sub_%x -> sub_%x\n", CFG_NODE_ID(prev_minst->cfg_node->addr), CFG_NODE_ID(tnode->minst->addr));
            }

            fprintf(fp, "sub_%x [shape=MSquare, label=<<font color='red'><b>sub_%x</b></font><br/>", CFG_NODE_ID(minst->addr), CFG_NODE_ID(minst->addr));
        }

        arm_minst_do(emu, minst);

        arm_inst_print_format(emu, minst, 0, obuf);

        fprintf(fp, "%s<br/>", obuf);
        prev_minst = minst;
    }
    fprintf(fp, ">]\n}");

    fclose(fp);
}

static int arm_emu_mblk_fix_pos(struct arm_emu *emu)
{
    int i;
    struct minst *minst, *b_minst;
    struct minst_node *succ_node;

    for (i = 0; i < emu->mblk.allinst.len; i++) {
        minst = emu->mblk.allinst.ptab[i];
        if (minst->flag.b && minst->flag.b_need_fixed) {
            unsigned long target_addr = emu_addr_host2target(emu, minst->host_addr);

            b_minst = minst_blk_find(&emu->mblk, target_addr);
            if (!b_minst)
                vm_error("arm emu host_addr[0x%x] target_addr[0x%x] not found", minst->host_addr, target_addr);

            minst_succ_add(minst, b_minst);
            minst_pred_add(b_minst, minst);

            minst->flag.b_need_fixed = 2;
        }

        if (minst->flag.in_it_block) {
            for (succ_node = &minst->succs; succ_node; succ_node = succ_node->next) {
                minst_succ_add(minst->preds.minst, succ_node->minst);
                minst_pred_add(succ_node->minst, minst->preds.minst);

                if (!succ_node->minst->flag.in_it_block)
                    break;
            }
        }
    }

    return 0;
}

int                 minst_blk_const_propagation(struct arm_emu *emu);

int         arm_emu_run(struct arm_emu *emu)
{
    int ret;

    emu->meet_blx = 0;
    /* first pass */
    arm_emu_cpu_reset(emu);
    minst_blk_live_prologue_add(&emu->mblk);
    for (emu->code.pos = 0; emu->code.pos < emu->code.len; ) {
        if (emu->meet_blx)
            break;

        ret = arm_insteng_decode(emu, emu->code.data + emu->code.pos, emu->code.len - emu->code.pos);
        if (ret < 0) {
            return -1;
        }
        emu->code.pos += ret;
    }
    minst_blk_live_epilogue_add(&emu->mblk);
    /* second pass */
    arm_emu_mblk_fix_pos(emu);

    /* third pass */
    minst_blk_liveness_calc(&emu->mblk);

    /* forth pass */
    minst_blk_dead_code_elim(&emu->mblk);

    minst_blk_gen_reaching_definitions(&emu->mblk);

    minst_blk_const_propagation(emu);

    arm_emu_dump_mblk(emu);

    if (emu->dump.cfg)
        arm_emu_dump_cfg(emu);

    return 0;
}

int         arm_emu_run_once(struct arm_emu *vm, unsigned char *code, int code_len)
{
    return 0;
}

void        arm_emu_dump(struct arm_emu *emu)
{
}

int                 minst_blk_const_propagation(struct arm_emu *emu)
{
    struct bitset *uses;
    struct minst *minst, *use_minst;
    struct minst_blk *blk = &emu->mblk;
    BITSET_INIT(def);
    int i, changed = 1, pos;

    EMU_SET_CONST_MODE(emu);

    while (changed) {
        changed = 0;
        for (i = 0; i < blk->allinst.len; i++) {
            minst = blk->allinst.ptab[i];
            if (minst->flag.is_const)
                dynarray_add(&blk->const_insts, minst);
        }

        for (i = 0; i < blk->const_insts.len; i++) {
            minst = blk->const_insts.ptab[i];
            if (minst_get_def(minst) < 0) continue;

            /* 遍历所有使用minst定值的 use 列表*/
            uses = &blk->uses[minst_get_def(minst)];
            for (pos = bitset_next_bit_pos(uses, 0); pos > 0; pos = bitset_next_bit_pos(uses, pos + 1)) {
                use_minst = blk->allinst.ptab[pos];

                /* 
                1. 查看使用列表中的指令的 reaching definitions in 集合中是否有这条指令，
                假如有的话，确认in集合中的def指令时唯一一条对 minst_get_def(minst) 中进行
                定值的指令.
                2. 查看是否在起点开始的唯一路径上 */
                bitset_clone(&def, &use_minst->rd_in);
                bitset_and(&def, &blk->defs[minst_get_def(minst)]);
                bitset_set(&def, minst->id, 0);
                if ((bitset_get(&use_minst->rd_in, minst->id) && bitset_is_empty(&def))
                    || ((minst != use_minst) && minst_blk_is_on_start_unique_path (blk, minst, use_minst))) {
                    arm_minst_do(emu, use_minst);

                    if (use_minst->flag.is_const)
                        dynarray_add(&blk->const_insts, use_minst);
                }
            }
        }
    }

    return 0;
}
