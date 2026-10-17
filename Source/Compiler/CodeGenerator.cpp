// CodeGenerator.cpp -- a function's syntax tree to Kestrel16 assembly.
//
// Code generation model
//
//   r0        accumulator / return value
//   r1-r4     scratch (caller-saved); r4 holds call targets
//   r5        frame pointer (callee-saved)
//   r6 = sp   stack pointer, r7 = lr
//
//   Arguments are pushed right-to-left, 2 bytes each; the caller pops them.
//   Frame:  [fp+4+2i] = argument i, [fp+2] = saved lr, [fp+0] = saved fp,
//           [fp-n]    = locals.
//   Calls use `la r4, target ; jalr r4`, so there is no range limit.
//   *, /, % are done by small runtime routines appended only when used.
//   Global C names get a leading '_' so they never clash with register names.

#include "CompilerInternal.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace Kestrel::Compiler {

string code;
bool useMultiply, useDivide, useMemcpy, useMemzero;

static vector<int> breakLabels, continueLabels;
static int functionId;

static void Emit(const char* format, ...) {
    char buffer[512];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof buffer, format, args);
    va_end(args);
    code += "    ";
    code += buffer;
    code += "\n";
}

static void EmitLabel(const string& name) {
    code += name + ":\n";
}
static string LabelName(int n) {
    return ".L" + std::to_string(n);
}
static int NewLabel() {
    return ++labelCounter;
}
static bool Fits6(long long v) {
    return v >= -32 && v <= 31;
}

static string MemoryOperand(const char* reg, int off) {
    char buffer[32];
    if (off == 0)
        snprintf(buffer, sizeof buffer, "[%s]", reg);
    else
        snprintf(buffer, sizeof buffer, "[%s%+d]", reg, off);
    return buffer;
}

// rd = ra + v  (uses r3 as scratch for big constants; rd/ra must not be r3)
static void AddConstant(const char* rd, const char* ra, long long v) {
    if (v == 0) {
        if (strcmp(rd, ra))
            Emit("mov %s, %s", rd, ra);
        return;
    }
    if (Fits6(v)) {
        Emit("addi %s, %s, %lld", rd, ra, v);
        return;
    }
    Emit("li r3, %lld", v & 0xFFFF);
    Emit("add %s, %s, r3", rd, ra);
}

static void CallRuntime(const char* name) {
    Emit("la r4, %s", name);
    Emit("jalr r4");
}

// r0 = value at [reg+off] (off must fit in 6 bits). Aggregates yield their address.
static void EmitLoad(Type* t, const char* reg, int off) {
    switch (t->kind) {
        case KindArray:
        case KindFunction:
        case KindStruct:
            AddConstant("r0", reg, off);
            return;
        case KindChar:
        case KindBool:
            Emit("lb r0, %s", MemoryOperand(reg, off).c_str());
            if (t->isUnsigned) {
                Emit("li r3, 255");
                Emit("and r0, r0, r3");
            } else
                Emit("sxb r0, r0");
            return;
        default:
            Emit("lw r0, %s", MemoryOperand(reg, off).c_str());
    }
}

// [reg+off] = r0
static void EmitStore(Type* t, const char* reg, int off) {
    if (t->kind == KindChar || t->kind == KindBool)
        Emit("sb r0, %s", MemoryOperand(reg, off).c_str());
    else
        Emit("sw r0, %s", MemoryOperand(reg, off).c_str());
}

static void EmitCast(Type* from, Type* to) {
    if (to->kind == KindVoid)
        return;
    if (to->kind == KindBool) {
        if (from->kind == KindBool)
            return;
        int done = NewLabel();
        Emit("cmpi r0, 0");
        Emit("beq %s", LabelName(done).c_str());
        Emit("li r0, 1");
        EmitLabel(LabelName(done));
        return;
    }
    if (to->kind == KindChar) {
        if (from->kind == KindChar && from->isUnsigned == to->isUnsigned)
            return;
        if (from->kind == KindBool)
            return;
        if (to->isUnsigned) {
            Emit("li r3, 255");
            Emit("and r0, r0, r3");
        } else
            Emit("sxb r0, r0");
    }
}

static void BranchIfFalse(int label) {
    int skip = NewLabel();
    Emit("cmpi r0, 0");
    Emit("bne %s", LabelName(skip).c_str());
    Emit("j %s", LabelName(label).c_str());
    EmitLabel(LabelName(skip));
}

static void BranchIfTrue(int label) {
    int skip = NewLabel();
    Emit("cmpi r0, 0");
    Emit("beq %s", LabelName(skip).c_str());
    Emit("j %s", LabelName(label).c_str());
    EmitLabel(LabelName(skip));
}

static void GenerateExpression(Node* n);

static bool IsLocalDirect(Node* n, int* off) {
    if (n->kind != NodeVar || !n->var->isLocal)
        return false;
    *off = n->var->offset + (int)n->value;
    return Fits6(*off);
}

// r0 = address of the lvalue n
static void GenerateAddress(Node* n) {
    switch (n->kind) {
        case NodeVar:
            if (n->var->isLocal)
                AddConstant("r0", "r5", n->var->offset + n->value);
            else if (n->value)
                Emit("la r0, %s+%lld", n->var->label.c_str(), n->value);
            else
                Emit("la r0, %s", n->var->label.c_str());
            return;
        case NodeDeref:
            GenerateExpression(n->a);
            return;
        case NodeMember:
            GenerateAddress(n->a);
            AddConstant("r0", "r0", n->member->offset);
            return;
        default:
            if (n->type->kind == KindStruct) {
                GenerateExpression(n);
                return;
            }
            Fatal(n->line, "expression is not an lvalue");
    }
}

static bool IsPowerOfTwo(long long v, int* shift) {
    if (v <= 0 || (v & (v - 1)))
        return false;
    *shift = 0;
    while ((1LL << *shift) != v)
        (*shift)++;
    return true;
}

// r0 = r0 op r1
static void ApplyOperator(NodeKind op, bool isUnsigned, Node* constantRight) {
    int shift;
    switch (op) {
        case NodeAdd:
            Emit("add r0, r0, r1");
            return;
        case NodeSub:
            Emit("sub r0, r0, r1");
            return;
        case NodeAnd:
            Emit("and r0, r0, r1");
            return;
        case NodeOr:
            Emit("or r0, r0, r1");
            return;
        case NodeXor:
            Emit("xor r0, r0, r1");
            return;
        case NodeShl:
            Emit("shl r0, r0, r1");
            return;
        case NodeShr:
            Emit(isUnsigned ? "shr r0, r0, r1" : "sar r0, r0, r1");
            return;
        case NodeMul:
            if (constantRight && IsPowerOfTwo(constantRight->value, &shift)) {
                if (shift) {
                    Emit("li r1, %d", shift);
                    Emit("shl r0, r0, r1");
                }
                return;
            }
            useMultiply = true;
            CallRuntime(".rt_mul");
            return;
        case NodeDiv:
        case NodeMod:
            if (isUnsigned && constantRight && IsPowerOfTwo(constantRight->value, &shift)) {
                if (op == NodeDiv) {
                    if (shift) {
                        Emit("li r1, %d", shift);
                        Emit("shr r0, r0, r1");
                    }
                } else {
                    Emit("li r1, %lld", constantRight->value - 1);
                    Emit("and r0, r0, r1");
                }
                return;
            }
            useDivide = true;
            CallRuntime(isUnsigned ? ".rt_divu" : ".rt_divs");
            if (op == NodeMod)
                Emit("mov r0, r1");
            return;
        case NodeEq:
        case NodeNe:
        case NodeLt:
        case NodeLe: {
            int yes = NewLabel(), done = NewLabel();
            Emit("cmp r0, r1");
            string y = LabelName(yes);
            if (op == NodeEq)
                Emit("beq %s", y.c_str());
            else if (op == NodeNe)
                Emit("bne %s", y.c_str());
            else if (op == NodeLt)
                Emit("%s %s", isUnsigned ? "bltu" : "blt", y.c_str());
            else if (isUnsigned) {
                Emit("bltu %s", y.c_str());
                Emit("beq %s", y.c_str());
            } else
                Emit("ble %s", y.c_str());
            Emit("li r0, 0");
            Emit("bra %s", LabelName(done).c_str());
            EmitLabel(y);
            Emit("li r0, 1");
            EmitLabel(LabelName(done));
            return;
        }
        default:
            Fatal(0, "internal error: bad operator");
    }
}

static void MemoryCopy(int size) {
    // r1 = destination, r0 = source; result r0 = destination
    useMemcpy = true;
    Emit("li r2, %d", size);
    CallRuntime(".rt_memcpy");
}

static void GenerateExpression(Node* n) {
    int off;
    switch (n->kind) {
        case NodeNumber:
            Emit("li r0, %lld", n->value & 0xFFFF);
            return;

        case NodeVar:
            if (IsLocalDirect(n, &off) && IsScalar(n->type)) {
                EmitLoad(n->type, "r5", off);
                return;
            }
            GenerateAddress(n);
            EmitLoad(n->type, "r0", 0);
            return;

        case NodeMember:
        case NodeDeref:
            GenerateAddress(n);
            EmitLoad(n->type, "r0", 0);
            return;

        case NodeAddr:
            GenerateAddress(n->a);
            return;

        case NodeCast:
            GenerateExpression(n->a);
            EmitCast(n->a->type, n->type);
            return;

        case NodeNeg:
            GenerateExpression(n->a);
            Emit("neg r0, r0");
            return;

        case NodeBitNot:
            GenerateExpression(n->a);
            Emit("not r0, r0");
            return;

        case NodeNot: {
            int yes = NewLabel(), done = NewLabel();
            GenerateExpression(n->a);
            Emit("cmpi r0, 0");
            Emit("beq %s", LabelName(yes).c_str());
            Emit("li r0, 0");
            Emit("bra %s", LabelName(done).c_str());
            EmitLabel(LabelName(yes));
            Emit("li r0, 1");
            EmitLabel(LabelName(done));
            return;
        }

        case NodeLogAnd: {
            int no = NewLabel(), done = NewLabel();
            GenerateExpression(n->a);
            BranchIfFalse(no);
            GenerateExpression(n->b);
            BranchIfFalse(no);
            Emit("li r0, 1");
            Emit("j %s", LabelName(done).c_str());
            EmitLabel(LabelName(no));
            Emit("li r0, 0");
            EmitLabel(LabelName(done));
            return;
        }

        case NodeLogOr: {
            int yes = NewLabel(), done = NewLabel();
            GenerateExpression(n->a);
            BranchIfTrue(yes);
            GenerateExpression(n->b);
            BranchIfTrue(yes);
            Emit("li r0, 0");
            Emit("j %s", LabelName(done).c_str());
            EmitLabel(LabelName(yes));
            Emit("li r0, 1");
            EmitLabel(LabelName(done));
            return;
        }

        case NodeCond: {
            int otherwise = NewLabel(), done = NewLabel();
            GenerateExpression(n->a);
            BranchIfFalse(otherwise);
            GenerateExpression(n->b);
            Emit("j %s", LabelName(done).c_str());
            EmitLabel(LabelName(otherwise));
            GenerateExpression(n->c);
            EmitLabel(LabelName(done));
            return;
        }

        case NodeComma:
            GenerateExpression(n->a);
            GenerateExpression(n->b);
            return;

        case NodeAdd:
        case NodeSub:
        case NodeMul:
        case NodeDiv:
        case NodeMod:
        case NodeAnd:
        case NodeOr:
        case NodeXor:
        case NodeShl:
        case NodeShr:
        case NodeEq:
        case NodeNe:
        case NodeLt:
        case NodeLe: {
            GenerateExpression(n->a);
            Node* constant = n->b->kind == NodeNumber ? n->b : nullptr;
            if (constant) {
                Emit("li r1, %lld", constant->value & 0xFFFF);
            } else {
                Emit("push r0");
                GenerateExpression(n->b);
                Emit("mov r1, r0");
                Emit("pop r0");
            }
            ApplyOperator(n->kind, n->opUnsigned, constant);
            return;
        }

        case NodePtrDiff: {
            GenerateExpression(n->a);
            Emit("push r0");
            GenerateExpression(n->b);
            Emit("mov r1, r0");
            Emit("pop r0");
            Emit("sub r0, r0, r1");
            int shift;
            if (IsPowerOfTwo(n->value, &shift)) {
                if (shift) {
                    Emit("li r1, %d", shift);
                    Emit("sar r0, r0, r1");
                }
            } else {
                useDivide = true;
                Emit("li r1, %lld", n->value);
                CallRuntime(".rt_divs");
            }
            return;
        }

        case NodeAssign:
            if (n->type->kind == KindStruct) {
                GenerateAddress(n->a);
                Emit("push r0");
                GenerateAddress(n->b);
                Emit("pop r1");
                MemoryCopy(n->type->size);
                return;
            }
            if (IsLocalDirect(n->a, &off)) {
                GenerateExpression(n->b);
                EmitCast(n->b->type, n->a->type);
                EmitStore(n->a->type, "r5", off);
                return;
            }
            GenerateAddress(n->a);
            Emit("push r0");
            GenerateExpression(n->b);
            EmitCast(n->b->type, n->a->type);
            Emit("pop r1");
            EmitStore(n->a->type, "r1", 0);
            return;

        case NodeCompound:
            GenerateAddress(n->a);
            Emit("push r0");
            EmitLoad(n->a->type, "r0", 0);
            Emit("push r0");
            GenerateExpression(n->b);
            Emit("mov r1, r0");
            Emit("pop r0");
            ApplyOperator(n->op, n->opUnsigned, n->b->kind == NodeNumber ? n->b : nullptr);
            EmitCast(TypeInt, n->a->type);
            Emit("pop r1");
            EmitStore(n->a->type, "r1", 0);
            return;

        case NodeIncDec:
            GenerateAddress(n->a);
            Emit("mov r1, r0");
            EmitLoad(n->a->type, "r1", 0);
            if (n->isPost)
                Emit("mov r2, r0");
            AddConstant("r0", "r0", n->value);
            EmitCast(TypeInt, n->a->type);
            EmitStore(n->a->type, "r1", 0);
            if (n->isPost)
                Emit("mov r0, r2");
            return;

        case NodeCall: {
            int count = (int)n->list.size();
            for (int i = count - 1; i >= 0; i--) {
                GenerateExpression(n->list[i]);
                Emit("push r0");
            }
            Node* fn = n->a;
            if (fn->kind == NodeVar && fn->var->type->kind == KindFunction) {
                Emit("la r4, %s", fn->var->label.c_str());
            } else {
                GenerateExpression(fn);
                Emit("mov r4, r0");
            }
            Emit("jalr r4");
            if (count)
                AddConstant("sp", "sp", 2 * count);
            return;
        }

        case NodeZero:
            useMemzero = true;
            AddConstant("r0", "r5", n->var->offset);
            Emit("li r1, %d", n->var->type->size);
            CallRuntime(".rt_memzero");
            return;

        default:
            Fatal(n->line, "internal error: unexpected node in expression");
    }
}

static void GenerateStatement(Node* n) {
    switch (n->kind) {
        case NodeBlock:
            for (Node* s : n->list)
                GenerateStatement(s);
            return;

        case NodeExprStmt:
        case NodeZero:
            GenerateExpression(n->kind == NodeZero ? n : n->a);
            return;

        case NodeIf: {
            int otherwise = NewLabel();
            GenerateExpression(n->a);
            BranchIfFalse(otherwise);
            GenerateStatement(n->b);
            if (n->c) {
                int done = NewLabel();
                Emit("j %s", LabelName(done).c_str());
                EmitLabel(LabelName(otherwise));
                GenerateStatement(n->c);
                EmitLabel(LabelName(done));
            } else {
                EmitLabel(LabelName(otherwise));
            }
            return;
        }

        case NodeWhile: {
            int top = NewLabel(), done = NewLabel();
            breakLabels.push_back(done);
            continueLabels.push_back(top);
            EmitLabel(LabelName(top));
            GenerateExpression(n->a);
            BranchIfFalse(done);
            GenerateStatement(n->b);
            Emit("j %s", LabelName(top).c_str());
            EmitLabel(LabelName(done));
            breakLabels.pop_back();
            continueLabels.pop_back();
            return;
        }

        case NodeDo: {
            int top = NewLabel(), next = NewLabel(), done = NewLabel();
            breakLabels.push_back(done);
            continueLabels.push_back(next);
            EmitLabel(LabelName(top));
            GenerateStatement(n->b);
            EmitLabel(LabelName(next));
            GenerateExpression(n->a);
            BranchIfTrue(top);
            EmitLabel(LabelName(done));
            breakLabels.pop_back();
            continueLabels.pop_back();
            return;
        }

        case NodeFor: {
            int top = NewLabel(), next = NewLabel(), done = NewLabel();
            if (n->a)
                GenerateStatement(n->a);
            breakLabels.push_back(done);
            continueLabels.push_back(next);
            EmitLabel(LabelName(top));
            if (n->b) {
                GenerateExpression(n->b);
                BranchIfFalse(done);
            }
            GenerateStatement(n->d);
            EmitLabel(LabelName(next));
            if (n->c)
                GenerateExpression(n->c);
            Emit("j %s", LabelName(top).c_str());
            EmitLabel(LabelName(done));
            breakLabels.pop_back();
            continueLabels.pop_back();
            return;
        }

        case NodeReturn:
            if (n->a)
                GenerateExpression(n->a);
            Emit("j .Lret%d", functionId);
            return;

        case NodeBreak:
            if (breakLabels.empty())
                Fatal(n->line, "'break' outside of a loop or switch");
            Emit("j %s", LabelName(breakLabels.back()).c_str());
            return;

        case NodeContinue:
            if (continueLabels.empty())
                Fatal(n->line, "'continue' outside of a loop");
            Emit("j %s", LabelName(continueLabels.back()).c_str());
            return;

        case NodeSwitch: {
            int done = NewLabel();
            GenerateExpression(n->a);
            for (Node* c : n->list) {
                int next = NewLabel();
                Emit("li r1, %lld", c->value & 0xFFFF);
                Emit("cmp r0, r1");
                Emit("bne %s", LabelName(next).c_str());
                Emit("j %s", LabelName(c->label).c_str());
                EmitLabel(LabelName(next));
            }
            Emit("j %s", LabelName(n->d ? n->d->label : done).c_str());
            breakLabels.push_back(done);
            GenerateStatement(n->b);
            breakLabels.pop_back();
            EmitLabel(LabelName(done));
            return;
        }

        case NodeCase:
            EmitLabel(LabelName(n->label));
            GenerateStatement(n->a);
            return;

        case NodeGoto:
            Emit("j .Lgoto%d_%s", functionId, n->name.c_str());
            return;

        case NodeLabel:
            EmitLabel(".Lgoto" + std::to_string(functionId) + "_" + n->name);
            GenerateStatement(n->a);
            return;

        default:
            Fatal(n->line, "internal error: unexpected statement");
    }
}

void GenerateFunction(Var* f, Node* body, int frameSize) {
    functionId++;
    code += "\n; ---- " + f->name + "\n";
    EmitLabel(f->label);
    Emit("push lr");
    Emit("push r5");
    Emit("mov r5, sp");
    Emit("li r2, .Lframe%d", functionId);
    Emit("sub sp, sp, r2");
    GenerateStatement(body);
    if (f->name == "main")
        Emit("li r0, 0");
    EmitLabel(".Lret" + std::to_string(functionId));
    Emit("mov sp, r5");
    Emit("pop r5");
    Emit("pop lr");
    Emit("ret");
    code += ".Lframe" + std::to_string(functionId) + " = " + std::to_string(AlignUp(frameSize, 2)) + "\n";
}

void ResetCodeGenerator() {
    code.clear();
    useMultiply = useDivide = useMemcpy = useMemzero = false;
    breakLabels.clear();
    continueLabels.clear();
    functionId = 0;
}

}  // namespace Kestrel::Compiler
