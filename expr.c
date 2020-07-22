// RPN expression evaluator
#define _DEFAULT_SOURCE
#include "v.h"

enum {STACK_SIZE = 4096};

int64_t eval(char *expr) {
	int64_t stack[STACK_SIZE], *p = stack;

	char *tok = strtok(expr, " ");
	while (tok) {
		if (!strcmp(tok, "+")) {
			int64_t b = *--p, a = *--p;
			*p++ = a + b;
		} else if (!strcmp(tok, "-")) {
			int64_t b = *--p, a = *--p;
			*p++ = a - b;
		} else if (!strcmp(tok, "*")) {
			int64_t b = *--p, a = *--p;
			*p++ = a * b;
		} else if (!strcmp(tok, "/")) {
			int64_t b = *--p, a = *--p;
			*p++ = a / b;
		} else if (!strcmp(tok, "%")) {
			int64_t b = *--p, a = *--p;
			*p++ = a % b;
		} else {
			char *end;
			*p++ = strtol(tok, &end, 0);
			if (*end) panic("Invalid token: %s", tok);
			// TODO: detect stack overflows. "Memory safety" or some shit idfk
		}

		tok = strtok(NULL, " ");
	}

	if (p - stack > 1) panic("%ld extra values left on stack", p - stack - 1);
	if (p - stack < 1) panic("No value left on stack");

	return *stack;
}

enum {
	REXB = 0x41,
	REXX = 0x42,
	REXR = 0x44,
	REXW = 0x48,

	MODRM_DIRECT = 0xc0,
	// TODO: indirect modrm addressing modes

	RAX = 0,
	RCX,
	RDX,
	RBX,
	RSP,
	RBP,
	RSI,
	RDI,
};

#define MODRM(mode, reg, rm) (MODRM_##mode | ((reg) & 07) << 3 | ((rm) & 07))

struct instruction {
	enum {
		NO_OPCODE = 0,

		ADD_RR = 0x03,
		CQO = 0x99,
		IDIV_R = 0xF7,
		IMUL_RR = 0xAF0F,
		MOV_RI = 0xB8,
		MOV_RR = 0x8B,
		POP_R = 0x58,
		PUSH_R = 0x50,
		RET = 0xC3,
		SUB_RR = 0x2B,
		XCHG_RR = 0x87,
	} op;
	int64_t a, b;
};

struct jit_state {
	unsigned char *start, *end, *p;
	struct instruction buffer;
};

static inline void jit_wbyte(struct jit_state *j, unsigned char b) {
	if (j->p >= j->end) panic("Overflowed JIT buffer");
	*j->p++ = b;
}

static inline void jit_wcode(struct jit_state *j, int op) {
	do {
		jit_wbyte(j, op & 0xff);
		op >>= 8;
	} while (op);
}

static inline void jit_wi64(struct jit_state *j, uint64_t x) {
	for (int i = 0; i < 8; i++) {
		jit_wbyte(j, (x >> (i*8)) & 0xff);
	}
}

void jit_flush(struct jit_state *j) {
	struct instruction i = j->buffer;
	j->buffer = (struct instruction){0};

	switch (i.op) {
	case NO_OPCODE:
		return;

	case ADD_RR: // 03 /r
		jit_wbyte(j, REXW);
		jit_wcode(j, ADD_RR);
		jit_wbyte(j, MODRM(DIRECT, i.a, i.b));
		break;

	case CQO: // 99
		jit_wbyte(j, REXW);
		jit_wcode(j, CQO);
		break;

	case IDIV_R: // F7 /7
		jit_wbyte(j, REXW);
		jit_wcode(j, IDIV_R);
		jit_wbyte(j, MODRM(DIRECT, 07, i.a));
		break;

	case IMUL_RR: // 0F AF /r
		jit_wbyte(j, REXW);
		jit_wcode(j, IMUL_RR);
		jit_wbyte(j, MODRM(DIRECT, i.a, i.b));
		break;

	case MOV_RI: // B8 +rq iq
		jit_wbyte(j, REXW);
		jit_wcode(j, MOV_RI + i.a);
		jit_wi64(j, i.b);
		break;

	case MOV_RR: // 8B /r
		jit_wbyte(j, REXW);
		jit_wcode(j, MOV_RR);
		jit_wbyte(j, MODRM(DIRECT, i.a, i.b));
		break;

	case POP_R: // 58 +rq
		jit_wcode(j, POP_R + i.a);
		break;

	case PUSH_R: // 50 +rq
		jit_wcode(j, PUSH_R + i.a);
		break;

	case RET: // C3
		jit_wcode(j, RET);
		break;

	case SUB_RR: // 2B /r
		jit_wbyte(j, REXW);
		jit_wcode(j, SUB_RR);
		jit_wbyte(j, MODRM(DIRECT, i.a, i.b));
		break;

	case XCHG_RR:
		jit_wbyte(j, REXW);
		if (i.a == RAX) { // 90 +rq
			jit_wcode(j, 0x90 + i.b);
		} else if (i.b == RAX) { // 90 +rq
			jit_wcode(j, 0x90 + i.a);
		} else { // 87 /r
			jit_wcode(j, XCHG_RR);
			jit_wbyte(j, MODRM(DIRECT, i.a, i.b));
		}
		break;
	}
}

void jit_write(struct jit_state *j, struct instruction i) {
	switch (i.op) {
	case POP_R:
		if (j->buffer.op == PUSH_R && j->buffer.a == i.a) {
			j->buffer.op = NO_OPCODE;
			return;
		}
		break;

	// XXX: DANGEROUS! Works here because we know that registers are reset immediately after a push
	case PUSH_R:
		if (j->buffer.op == POP_R && j->buffer.a == i.a) {
			j->buffer.op = NO_OPCODE;
			return;
		}
		break;

	default:
		break;
	}

	jit_flush(j);
	j->buffer = i;
}

enum {JIT_BUF_SIZE = 4096};
typedef int64_t (*exprfn_t)(void);
exprfn_t jit(char *expr) {
	int stack = 0;

	struct jit_state j = {0};
	j.start = mmap(NULL, JIT_BUF_SIZE, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
	j.end = j.start + JIT_BUF_SIZE;
	j.p = j.start;

	int reg1 = RCX, reg2 = RAX;

#define EMIT(...) jit_write(&j, (struct instruction){__VA_ARGS__})

	char *tok = strtok(expr, " ");
	while (tok) {
		if (!strcmp(tok, "+")) {
			// add reg1, reg2
			EMIT(ADD_RR, reg1, reg2);

			// pop reg2
			EMIT(POP_R, reg2);
			stack--;
		} else if (!strcmp(tok, "-")) {
			// sub reg2, reg1
			EMIT(SUB_RR, reg2, reg1);

			// Swap reg1 and reg2
			int tmp = reg1; reg1 = reg2; reg2 = tmp;

			// pop reg2
			EMIT(POP_R, reg2);
			stack--;
		} else if (!strcmp(tok, "*")) {
			// imul reg1, reg2
			EMIT(IMUL_RR, reg1, reg2);

			// pop reg2
			EMIT(POP_R, reg2);
			stack--;
		} else if (!strcmp(tok, "/") || !strcmp(tok, "%")) {
			// Division requires rax as the destination
			// We swap the args and `mov` if necessary
			if (reg1 == RAX) {
				// Divisor is in rax, move it to rcx
				// xchg reg1, reg2
				EMIT(XCHG_RR, reg1, reg2);
			} else {
				// Divisor is in rcx, swap registers
				int tmp = reg1; reg1 = reg2; reg2 = tmp;
			}

			// cqo
			EMIT(CQO);

			// idiv reg2
			EMIT(IDIV_R, reg2);

			if (*tok == '%') {
				// mov reg1, rdx
				EMIT(MOV_RR, reg1, RDX);
			}

			// pop reg2
			EMIT(POP_R, reg2);
			stack--;
		} else {
			if (++stack >= STACK_SIZE) panic("Stack overflow");
			char *end;
			long val = strtol(tok, &end, 0);
			if (*end) panic("Invalid token: %s", tok);

			// push reg2
			EMIT(PUSH_R, reg2);

			// Swap reg1 and reg2
			int tmp = reg1; reg1 = reg2; reg2 = tmp;

			// mov reg1, $val
			EMIT(MOV_RI, reg1, val);
		}

		tok = strtok(NULL, " ");
	}

	// pop reg2
	EMIT(POP_R, reg2);

	if (reg1 != RAX) {
		// Move reg1 to rax for returning
		// mov rax, reg1
		EMIT(MOV_RR, RAX, reg1);
	}

	// ret
	EMIT(RET);

	jit_flush(&j);

	if (stack > 1) panic("%d extra values left on stack", stack - 1);
	if (stack < 1) panic("No value left on stack");

	FILE *f = fopen("test.bin", "wb");
	if (!f) panic("Failed to open file");
	if (fwrite(j.start, j.p - j.start, 1, f) != 1) panic("Failed to write file");
	fclose(f);

	mprotect(j.start, JIT_BUF_SIZE, PROT_EXEC);
	return (exprfn_t)j.start;
}

enum {ITERATIONS = 10000000};

int main(int argc, char **argv) {
	if (argc < 2) panic("Not enough arguments");

#if 0
	char *expr = strdup(argv[1]);
	int64_t val = eval(expr);
	for (int i = 0; i < ITERATIONS; i++) {
		strcpy(expr, argv[1]);
		int64_t val2 = eval(expr);
		assert(val == val2, "Not deterministic! %"PRId64" != %"PRId64, val, val2);
	}
	free(expr);
	printf("%ld\n", val);
#else
	char *expr = strdup(argv[1]);
	exprfn_t fn = jit(expr);
	free(expr);
	int64_t val = fn();
	for (int i = 0; i < ITERATIONS * 100; i++) {
		int64_t val2 = fn();
		assert(val == val2, "Not deterministic! %"PRId64" != %"PRId64, val, val2);
	}
	printf("%"PRId64"\n", val);
#endif

	return 0;
}
