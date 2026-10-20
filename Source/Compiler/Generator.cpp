// Generator.cpp -- a function's syntax tree to Kestrel16 assembly.
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

#include "Generator.h"
#include "Parser.h"

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
static string LabelName(int number) {
    return ".L" + std::to_string(number);
}
static int NewLabel() {
    return ++labelCounter;
}
static bool Fits6(long long value) {
    return value >= -32 && value <= 31;
}

static string MemoryOperand(const char* reg, int offset) {
    char buffer[32];
    if (offset == 0) {
        snprintf(buffer, sizeof buffer, "[%s]", reg);
    }
    else {
        snprintf(buffer, sizeof buffer, "[%s%+d]", reg, offset);
    }
    return buffer;
}

// rd = ra + value  (uses r3 as scratch for big constants; rd/ra must not be r3)
static void AddConstant(const char* rd, const char* ra, long long value) {
    if (value == 0) {
        bool sameRegister = strcmp(rd, ra) == 0;
        if (!sameRegister) {
            Emit("mov %s, %s", rd, ra);
        }
        return;
    }
    if (Fits6(value)) {
        Emit("addi %s, %s, %lld", rd, ra, value);
        return;
    }
    Emit("li r3, %lld", value & 0xFFFF);
    Emit("add %s, %s, r3", rd, ra);
}

static void CallRuntime(const char* name) {
    Emit("la r4, %s", name);
    Emit("jalr r4");
}

// r0 = value at [reg+offset] (offset must fit in 6 bits). Aggregates yield their address.
static void EmitLoad(Type* type, const char* reg, int offset) {
    switch (type->kind) {
        case KindArray:
        case KindFunction:
        case KindStruct:
            AddConstant("r0", reg, offset);
            return;
        case KindChar:
        case KindBool:
            Emit("lb r0, %s", MemoryOperand(reg, offset).c_str());
            if (type->isUnsigned) {
                Emit("li r3, 255");
                Emit("and r0, r0, r3");
            }
            else {
                Emit("sxb r0, r0");
            }
            return;
        default:
            Emit("lw r0, %s", MemoryOperand(reg, offset).c_str());
    }
}

// [reg+offset] = r0
static void EmitStore(Type* type, const char* reg, int offset) {
    if (type->kind == KindChar || type->kind == KindBool) {
        Emit("sb r0, %s", MemoryOperand(reg, offset).c_str());
    }
    else {
        Emit("sw r0, %s", MemoryOperand(reg, offset).c_str());
    }
}

static void EmitCast(Type* from, Type* to) {
    if (to->kind == KindVoid) {
        return;
    }
    if (to->kind == KindBool) {
        if (from->kind == KindBool) {
            return;
        }
        int done = NewLabel();
        Emit("cmpi r0, 0");
        Emit("beq %s", LabelName(done).c_str());
        Emit("li r0, 1");
        EmitLabel(LabelName(done));
        return;
    }
    if (to->kind == KindChar) {
        if (from->kind == KindChar && from->isUnsigned == to->isUnsigned) {
            return;
        }
        if (from->kind == KindBool) {
            return;
        }
        if (to->isUnsigned) {
            Emit("li r3, 255");
            Emit("and r0, r0, r3");
        }
        else {
            Emit("sxb r0, r0");
        }
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

static void GenerateExpression(Node* node);

static bool IsLocalDirect(Node* node, int* offset) {
    if (node->kind != NodeVar || !node->var->isLocal) {
        return false;
    }
    *offset = node->var->offset + (int)node->value;
    return Fits6(*offset);
}

// r0 = address of the lvalue n
static void GenerateAddress(Node* node) {
    switch (node->kind) {
        case NodeVar:
            if (node->var->isLocal) {
                AddConstant("r0", "r5", node->var->offset + node->value);
            }
            else if (node->value) {
                Emit("la r0, %s+%lld", node->var->label.c_str(), node->value);
            }
            else {
                Emit("la r0, %s", node->var->label.c_str());
            }
            return;
        case NodeDeref:
            GenerateExpression(node->operand);
            return;
        case NodeMember:
            GenerateAddress(node->operand);
            AddConstant("r0", "r0", node->member->offset);
            return;
        default:
            if (node->type->kind == KindStruct) {
                GenerateExpression(node);
                return;
            }
            Fatal(node->line, "expression is not an lvalue");
    }
}

static bool IsPowerOfTwo(long long value, int* shift) {
    if (value <= 0) {
        return false;
    }
    bool hasOneBitSet = (value & (value - 1)) == 0;  // clearing the lowest set bit leaves nothing
    if (!hasOneBitSet) {
        return false;
    }
    *shift = 0;
    while ((1LL << *shift) != value) {
        (*shift)++;
    }
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
                }
                else {
                    Emit("li r1, %lld", constantRight->value - 1);
                    Emit("and r0, r0, r1");
                }
                return;
            }
            useDivide = true;
            CallRuntime(isUnsigned ? ".rt_divu" : ".rt_divs");
            if (op == NodeMod) {
                Emit("mov r0, r1");
            }
            return;
        case NodeEq:
        case NodeNe:
        case NodeLt:
        case NodeLe: {
            int yes = NewLabel(), done = NewLabel();
            Emit("cmp r0, r1");
            string yesLabel = LabelName(yes);
            if (op == NodeEq) {
                Emit("beq %s", yesLabel.c_str());
            }
            else if (op == NodeNe) {
                Emit("bne %s", yesLabel.c_str());
            }
            else if (op == NodeLt) {
                if (isUnsigned) {
                    Emit("bltu %s", yesLabel.c_str());
                }
                else {
                    Emit("blt %s", yesLabel.c_str());
                }
            }
            else if (isUnsigned) {
                Emit("bltu %s", yesLabel.c_str());
                Emit("beq %s", yesLabel.c_str());
            }
            else {
                Emit("ble %s", yesLabel.c_str());
            }
            Emit("li r0, 0");
            Emit("bra %s", LabelName(done).c_str());
            EmitLabel(yesLabel);
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

static void GenerateExpression(Node* node) {
    int offset;
    switch (node->kind) {
        case NodeNumber:
            Emit("li r0, %lld", node->value & 0xFFFF);
            return;

        case NodeVar:
            if (IsLocalDirect(node, &offset) && IsScalar(node->type)) {
                EmitLoad(node->type, "r5", offset);
                return;
            }
            GenerateAddress(node);
            EmitLoad(node->type, "r0", 0);
            return;

        case NodeMember:
        case NodeDeref:
            GenerateAddress(node);
            EmitLoad(node->type, "r0", 0);
            return;

        case NodeAddr:
            GenerateAddress(node->operand);
            return;

        case NodeCast:
            GenerateExpression(node->operand);
            EmitCast(node->operand->type, node->type);
            return;

        case NodeNeg:
            GenerateExpression(node->operand);
            Emit("neg r0, r0");
            return;

        case NodeBitNot:
            GenerateExpression(node->operand);
            Emit("not r0, r0");
            return;

        case NodeNot: {
            int yes = NewLabel(), done = NewLabel();
            GenerateExpression(node->operand);
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
            GenerateExpression(node->left);
            BranchIfFalse(no);
            GenerateExpression(node->right);
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
            GenerateExpression(node->left);
            BranchIfTrue(yes);
            GenerateExpression(node->right);
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
            GenerateExpression(node->condition);
            BranchIfFalse(otherwise);
            GenerateExpression(node->then);
            Emit("j %s", LabelName(done).c_str());
            EmitLabel(LabelName(otherwise));
            GenerateExpression(node->otherwise);
            EmitLabel(LabelName(done));
            return;
        }

        case NodeComma:
            GenerateExpression(node->left);
            GenerateExpression(node->right);
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
            GenerateExpression(node->left);
            // a constant right operand is loaded directly, and lets * / % use shifts
            Node* constant = nullptr;
            if (node->right->kind == NodeNumber) {
                constant = node->right;
            }
            if (constant) {
                Emit("li r1, %lld", constant->value & 0xFFFF);
            }
            else {
                Emit("push r0");
                GenerateExpression(node->right);
                Emit("mov r1, r0");
                Emit("pop r0");
            }
            ApplyOperator(node->kind, node->opUnsigned, constant);
            return;
        }

        case NodePtrDiff: {
            GenerateExpression(node->left);
            Emit("push r0");
            GenerateExpression(node->right);
            Emit("mov r1, r0");
            Emit("pop r0");
            Emit("sub r0, r0, r1");
            int shift;
            if (IsPowerOfTwo(node->value, &shift)) {
                if (shift) {
                    Emit("li r1, %d", shift);
                    Emit("sar r0, r0, r1");
                }
            }
            else {
                useDivide = true;
                Emit("li r1, %lld", node->value);
                CallRuntime(".rt_divs");
            }
            return;
        }

        case NodeAssign:
            if (node->type->kind == KindStruct) {
                GenerateAddress(node->left);
                Emit("push r0");
                GenerateAddress(node->right);
                Emit("pop r1");
                MemoryCopy(node->type->size);
                return;
            }
            if (IsLocalDirect(node->left, &offset)) {
                GenerateExpression(node->right);
                EmitCast(node->right->type, node->left->type);
                EmitStore(node->left->type, "r5", offset);
                return;
            }
            GenerateAddress(node->left);
            Emit("push r0");
            GenerateExpression(node->right);
            EmitCast(node->right->type, node->left->type);
            Emit("pop r1");
            EmitStore(node->left->type, "r1", 0);
            return;

        case NodeCompound: {
            GenerateAddress(node->left);
            Emit("push r0");
            EmitLoad(node->left->type, "r0", 0);
            Emit("push r0");
            GenerateExpression(node->right);
            Emit("mov r1, r0");
            Emit("pop r0");
            Node* constant = nullptr;
            if (node->right->kind == NodeNumber) {
                constant = node->right;
            }
            ApplyOperator(node->op, node->opUnsigned, constant);
            EmitCast(TypeInt, node->left->type);
            Emit("pop r1");
            EmitStore(node->left->type, "r1", 0);
            return;
        }

        case NodeIncDec:
            GenerateAddress(node->operand);
            Emit("mov r1, r0");
            EmitLoad(node->operand->type, "r1", 0);
            if (node->isPost) {
                Emit("mov r2, r0");
            }
            AddConstant("r0", "r0", node->value);
            EmitCast(TypeInt, node->operand->type);
            EmitStore(node->operand->type, "r1", 0);
            if (node->isPost) {
                Emit("mov r0, r2");
            }
            return;

        case NodeCall: {
            int count = (int)node->list.size();
            for (int i = count - 1; i >= 0; i--) {
                GenerateExpression(node->list[i]);
                Emit("push r0");
            }
            Node* callee = node->callee;
            if (callee->kind == NodeVar && callee->var->type->kind == KindFunction) {
                Emit("la r4, %s", callee->var->label.c_str());
            }
            else {
                GenerateExpression(callee);
                Emit("mov r4, r0");
            }
            Emit("jalr r4");
            if (count) {
                AddConstant("sp", "sp", 2 * count);
            }
            return;
        }

        case NodeZero:
            useMemzero = true;
            AddConstant("r0", "r5", node->var->offset);
            Emit("li r1, %d", node->var->type->size);
            CallRuntime(".rt_memzero");
            return;

        default:
            Fatal(node->line, "internal error: unexpected node in expression");
    }
}

static void GenerateStatement(Node* node) {
    switch (node->kind) {
        case NodeBlock:
            for (Node* statement : node->list) {
                GenerateStatement(statement);
            }
            return;

        case NodeExprStmt:
            GenerateExpression(node->expression);
            return;

        case NodeZero:
            GenerateExpression(node);
            return;

        case NodeIf: {
            int otherwise = NewLabel();
            GenerateExpression(node->condition);
            BranchIfFalse(otherwise);
            GenerateStatement(node->then);
            if (node->otherwise) {
                int done = NewLabel();
                Emit("j %s", LabelName(done).c_str());
                EmitLabel(LabelName(otherwise));
                GenerateStatement(node->otherwise);
                EmitLabel(LabelName(done));
            }
            else {
                EmitLabel(LabelName(otherwise));
            }
            return;
        }

        case NodeWhile: {
            int top = NewLabel(), done = NewLabel();
            breakLabels.push_back(done);
            continueLabels.push_back(top);
            EmitLabel(LabelName(top));
            GenerateExpression(node->condition);
            BranchIfFalse(done);
            GenerateStatement(node->body);
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
            GenerateStatement(node->body);
            EmitLabel(LabelName(next));
            GenerateExpression(node->condition);
            BranchIfTrue(top);
            EmitLabel(LabelName(done));
            breakLabels.pop_back();
            continueLabels.pop_back();
            return;
        }

        case NodeFor: {
            int top = NewLabel(), next = NewLabel(), done = NewLabel();
            if (node->init) {
                GenerateStatement(node->init);
            }
            breakLabels.push_back(done);
            continueLabels.push_back(next);
            EmitLabel(LabelName(top));
            if (node->condition) {
                GenerateExpression(node->condition);
                BranchIfFalse(done);
            }
            GenerateStatement(node->body);
            EmitLabel(LabelName(next));
            if (node->step) {
                GenerateExpression(node->step);
            }
            Emit("j %s", LabelName(top).c_str());
            EmitLabel(LabelName(done));
            breakLabels.pop_back();
            continueLabels.pop_back();
            return;
        }

        case NodeReturn:
            if (node->expression) {
                GenerateExpression(node->expression);
            }
            Emit("j .Lret%d", functionId);
            return;

        case NodeBreak:
            if (breakLabels.empty()) {
                Fatal(node->line, "'break' outside of a loop or switch");
            }
            Emit("j %s", LabelName(breakLabels.back()).c_str());
            return;

        case NodeContinue:
            if (continueLabels.empty()) {
                Fatal(node->line, "'continue' outside of a loop");
            }
            Emit("j %s", LabelName(continueLabels.back()).c_str());
            return;

        case NodeSwitch: {
            int done = NewLabel();
            GenerateExpression(node->condition);
            for (Node* caseNode : node->list) {
                int next = NewLabel();
                Emit("li r1, %lld", caseNode->value & 0xFFFF);
                Emit("cmp r0, r1");
                Emit("bne %s", LabelName(next).c_str());
                Emit("j %s", LabelName(caseNode->label).c_str());
                EmitLabel(LabelName(next));
            }
            // no case matched: go to default, or past the switch when there is none
            int noMatch = done;
            if (node->defaultCase) {
                noMatch = node->defaultCase->label;
            }
            Emit("j %s", LabelName(noMatch).c_str());
            breakLabels.push_back(done);
            GenerateStatement(node->body);
            breakLabels.pop_back();
            EmitLabel(LabelName(done));
            return;
        }

        case NodeCase:
            EmitLabel(LabelName(node->label));
            GenerateStatement(node->body);
            return;

        case NodeGoto:
            Emit("j .Lgoto%d_%s", functionId, node->name.c_str());
            return;

        case NodeLabel:
            EmitLabel(".Lgoto" + std::to_string(functionId) + "_" + node->name);
            GenerateStatement(node->body);
            return;

        default:
            Fatal(node->line, "internal error: unexpected statement");
    }
}

void GenerateFunction(Var* function, Node* body, int frameSize) {
    functionId++;
    code += "\n; ---- " + function->name + "\n";
    EmitLabel(function->label);
    Emit("push lr");
    Emit("push r5");
    Emit("mov r5, sp");
    Emit("li r2, .Lframe%d", functionId);
    Emit("sub sp, sp, r2");
    GenerateStatement(body);
    if (function->name == "main") {
        Emit("li r0, 0");
    }
    EmitLabel(".Lret" + std::to_string(functionId));
    Emit("mov sp, r5");
    Emit("pop r5");
    Emit("pop lr");
    Emit("ret");
    code += ".Lframe" + std::to_string(functionId) + " = " + std::to_string(AlignUp(frameSize, 2)) + "\n";
}

void ResetGenerator() {
    code.clear();
    useMultiply = useDivide = useMemcpy = useMemzero = false;
    breakLabels.clear();
    continueLabels.clear();
    functionId = 0;
}

}  // namespace Kestrel::Compiler
