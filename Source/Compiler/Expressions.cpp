// Expressions.cpp -- C expressions: parsed, typed as they are built, and evaluated when constant.

#include "CompilerInternal.h"

#include <cstdint>

namespace Kestrel::Compiler {

static Node* CastExpression();

// ===========================================================================
// Expression construction (with typing)

void NeedScalar(Node* node, const char* what) {
    Type* type = Decay(node->type);
    if (!IsScalar(type)) {
        Fatal(node->line, "invalid operand to %s", what);
    }
}

void NeedInteger(Node* node, const char* what) {
    if (!IsInteger(node->type)) {
        Fatal(node->line, "operand of %s must be an integer", what);
    }
}

Node* MakeCast(Node* expr, Type* type) {
    bool castsToStruct = type->kind == KindStruct;
    bool castsStructToValue = expr->type->kind == KindStruct && type->kind != KindVoid;
    if (castsToStruct || castsStructToValue) {
        Fatal(expr->line, "invalid cast involving a struct");
    }
    Node* cast = NewNode(NodeCast, expr->line);
    cast->operand = expr;
    cast->type = type;
    return cast;
}

// The type an integer operator works in. A shift takes its type from the left operand alone;
// everything else uses the usual arithmetic conversions of both.
static Type* OperatorType(NodeKind kind, Type* left, Type* right) {
    if (kind == NodeShl || kind == NodeShr) {
        return Promote(left);
    }
    return UsualArithmetic(left, right);
}

static Node* MakeBinary(NodeKind kind, Node* left, Node* right, int line, const char* what) {
    NeedInteger(left, what);
    NeedInteger(right, what);
    Node* node = NewNode(kind, line);
    node->left = left;
    node->right = right;
    node->type = OperatorType(kind, left->type, right->type);
    node->opUnsigned = node->type->isUnsigned;
    return node;
}

static int ElementSize(Type* pointer, int line) {
    Type* base = pointer->base;
    if (base->kind == KindVoid || base->kind == KindFunction) {
        return 1;
    }
    if (base->kind == KindStruct && !base->info->complete) {
        Fatal(line, "arithmetic on pointer to incomplete type");
    }
    return base->size;
}

static Node* Scaled(Node* index, int size) {
    if (size == 1) {
        return index;
    }
    Node* product = NewNode(NodeMul, index->line);
    product->left = index;
    product->right = NewNumber(size, index->line);
    product->type = TypeInt;
    return product;
}

static Node* MakeAdd(Node* left, Node* right, int line) {
    Type* leftType = Decay(left->type);
    Type* rightType = Decay(right->type);
    if (IsInteger(leftType) && IsInteger(rightType)) {
        return MakeBinary(NodeAdd, left, right, line, "+");
    }
    // integer + pointer: put the pointer on the left
    if (IsInteger(leftType) && rightType->kind == KindPointer) {
        std::swap(left, right);
        std::swap(leftType, rightType);
    }
    if (leftType->kind == KindPointer && IsInteger(rightType)) {
        Node* sum = NewNode(NodeAdd, line);
        sum->left = left;
        sum->right = Scaled(right, ElementSize(leftType, line));
        sum->type = leftType;
        return sum;
    }
    Fatal(line, "invalid operands to +");
}

static Node* MakeSub(Node* left, Node* right, int line) {
    Type* leftType = Decay(left->type);
    Type* rightType = Decay(right->type);
    if (IsInteger(leftType) && IsInteger(rightType)) {
        return MakeBinary(NodeSub, left, right, line, "-");
    }
    if (leftType->kind == KindPointer && IsInteger(rightType)) {
        Node* difference = NewNode(NodeSub, line);
        difference->left = left;
        difference->right = Scaled(right, ElementSize(leftType, line));
        difference->type = leftType;
        return difference;
    }
    if (leftType->kind == KindPointer && rightType->kind == KindPointer) {
        Node* difference = NewNode(NodePtrDiff, line);
        difference->left = left;
        difference->right = right;
        difference->value = ElementSize(leftType, line);
        difference->type = TypeInt;
        return difference;
    }
    Fatal(line, "invalid operands to -");
}

// swap turns > and >= into < and <= with the operands reversed.
static Node* MakeCompare(NodeKind kind, Node* left, Node* right, int line, bool swap) {
    NeedScalar(left, "comparison");
    NeedScalar(right, "comparison");
    if (swap) {
        std::swap(left, right);
    }
    Node* compare = NewNode(kind, line);
    compare->left = left;
    compare->right = right;
    compare->type = TypeInt;

    Type* leftType = Decay(left->type);
    Type* rightType = Decay(right->type);
    if (leftType->kind == KindPointer || rightType->kind == KindPointer) {
        compare->opUnsigned = true;  // addresses compare unsigned
    }
    else {
        compare->opUnsigned = UsualArithmetic(leftType, rightType)->isUnsigned;
    }
    return compare;
}

static bool IsLvalue(Node* node) {
    return node->kind == NodeVar || node->kind == NodeDeref || node->kind == NodeMember;
}

Node* MakeAssign(Node* lhs, Node* rhs, int line) {
    if (!IsLvalue(lhs)) {
        Fatal(line, "left side of assignment is not assignable");
    }
    if (lhs->type->kind == KindArray || lhs->type->kind == KindFunction) {
        Fatal(line, "cannot assign to an array or function");
    }
    if (lhs->type->kind == KindStruct) {
        if (rhs->type != lhs->type) {
            Fatal(line, "incompatible struct types in assignment");
        }
    }
    else {
        NeedScalar(rhs, "assignment");
    }
    Node* assign = NewNode(NodeAssign, line);
    assign->left = lhs;
    assign->right = rhs;
    assign->type = lhs->type;
    return assign;
}

static Node* MakeDeref(Node* pointer, int line) {
    Type* type = Decay(pointer->type);
    if (type->kind != KindPointer) {
        Fatal(line, "cannot dereference a non-pointer");
    }
    if (type->base->kind == KindVoid) {
        Fatal(line, "cannot dereference a void pointer");
    }
    Node* deref = NewNode(NodeDeref, line);
    deref->operand = pointer;
    deref->type = type->base;
    return deref;
}

static Node* MakeMember(Node* object, const string& name, int line) {
    if (object->type->kind != KindStruct) {
        Fatal(line, "'.%s' used on something that is not a struct", name.c_str());
    }
    if (!object->type->info->complete) {
        Fatal(line, "use of incomplete struct");
    }
    for (Member& member : object->type->info->members) {
        if (member.name == name) {
            Node* access = NewNode(NodeMember, line);
            access->operand = object;
            access->member = &member;
            access->type = member.type;
            return access;
        }
    }
    Fatal(line, "no member named '%s'", name.c_str());
}

static Node* MakeIncDec(Node* target, int delta, bool isPost, int line) {
    if (!IsLvalue(target)) {
        Fatal(line, "operand of ++/-- is not assignable");
    }
    NeedScalar(target, "++/--");
    Node* node = NewNode(NodeIncDec, line);
    node->operand = target;
    node->type = target->type;
    node->isPost = isPost;
    if (target->type->kind == KindPointer) {
        node->value = delta * ElementSize(target->type, line);  // a pointer steps by whole elements
    }
    else {
        node->value = delta;
    }
    return node;
}

static Var* NewStringVar(const string& bytes) {
    Var* literal = new Var;
    literal->label = ".Lstr" + std::to_string(++stringCounter);
    literal->type = ArrayOf(TypeChar, (int)bytes.size() + 1);
    literal->isDefined = true;
    literal->hasInit = true;
    literal->data.assign(bytes.begin(), bytes.end());
    literal->data.push_back(0);
    globals.push_back(literal);
    return literal;
}

static Node* VarNode(Var* var, int line) {
    Node* node = NewNode(NodeVar, line);
    node->var = var;
    node->type = var->type;
    return node;
}

// ===========================================================================
// Expression parsing

static Node* CallExpression(Node* callee, int line) {
    Type* function = callee->type;
    if (function->kind == KindPointer && function->base->kind == KindFunction) {
        function = function->base;
    }
    if (function->kind != KindFunction) {
        Fatal(line, "called object is not a function");
    }
    Node* call = NewNode(NodeCall, line);
    call->callee = callee;
    call->type = function->base;
    if (call->type->kind == KindStruct) {
        Fatal(line, "returning structs by value is not supported (pass a pointer)");
    }

    bool checkArguments = !function->oldStyle;
    if (!Accept(")")) {
        do {
            Node* argument = Assignment();
            size_t index = call->list.size();
            if (argument->type->kind == KindStruct) {
                Fatal(argument->line, "passing structs by value is not supported (pass a pointer)");
            }
            if (argument->type->kind == KindVoid) {
                Fatal(argument->line, "void value used as an argument");
            }
            if (checkArguments) {
                if (index < function->params.size()) {
                    argument = MakeCast(argument, function->params[index]);
                }
                else if (!function->variadic) {
                    Fatal(argument->line, "too many arguments in call");
                }
            }
            call->list.push_back(argument);
        } while (Accept(","));
        Expect(")");
    }
    if (checkArguments && call->list.size() < function->params.size()) {
        Fatal(line, "too few arguments in call");
    }
    return call;
}

static Node* Primary() {
    Token& token = Tok();
    int line = token.line;
    if (Accept("(")) {
        Node* expr = Expression();
        Expect(")");
        return expr;
    }
    if (token.kind == TokNumber) {
        pos++;
        return NewNumber(token.value, line, token.isUnsigned ? TypeUInt : TypeInt);
    }
    if (token.kind == TokString) {
        pos++;
        return VarNode(NewStringVar(token.text), line);
    }
    if (token.kind == TokIdent) {
        pos++;
        Symbol* symbol = FindSymbol(token.text);
        if (!symbol) {
            if (Is("(")) {
                Fatal(line, "call to undeclared function '%s' (declare a prototype first)", token.text.c_str());
            }
            Fatal(line, "undeclared identifier '%s'", token.text.c_str());
        }
        if (symbol->typedefType) {
            Fatal(line, "undeclared identifier '%s'", token.text.c_str());
        }
        if (symbol->isEnumConstant) {
            return NewNumber(symbol->enumValue, line);
        }
        return VarNode(symbol->var, line);
    }
    Fatal(line, "expected an expression but found '%s'", token.text.c_str());
}

static Node* Postfix() {
    Node* expr = Primary();
    for (;;) {
        int line = Tok().line;
        if (Accept("[")) {
            Node* index = Expression();
            Expect("]");
            expr = MakeDeref(MakeAdd(expr, index, line), line);
        }
        else if (Accept("(")) {
            expr = CallExpression(expr, line);
        }
        else if (Accept(".")) {
            expr = MakeMember(expr, ExpectIdent(), line);
        }
        else if (Accept("->")) {
            expr = MakeMember(MakeDeref(expr, line), ExpectIdent(), line);
        }
        else if (Accept("++")) {
            expr = MakeIncDec(expr, 1, true, line);
        }
        else if (Accept("--")) {
            expr = MakeIncDec(expr, -1, true, line);
        }
        else {
            return expr;
        }
    }
}

static Node* MakeUnary(NodeKind kind, Node* operand, Type* type, int line) {
    Node* node = NewNode(kind, line);
    node->operand = operand;
    node->type = type;
    return node;
}

static Node* Unary() {
    int line = Tok().line;
    if (Accept("+")) {
        Node* operand = CastExpression();
        NeedInteger(operand, "unary +");
        return MakeCast(operand, Promote(operand->type));
    }
    if (Accept("-")) {
        Node* operand = CastExpression();
        NeedInteger(operand, "unary -");
        return MakeUnary(NodeNeg, operand, Promote(operand->type), line);
    }
    if (Accept("!")) {
        Node* operand = CastExpression();
        NeedScalar(operand, "!");
        return MakeUnary(NodeNot, operand, TypeInt, line);
    }
    if (Accept("~")) {
        Node* operand = CastExpression();
        NeedInteger(operand, "~");
        return MakeUnary(NodeBitNot, operand, Promote(operand->type), line);
    }
    if (Accept("&")) {
        Node* operand = CastExpression();
        if (!IsLvalue(operand)) {
            Fatal(line, "cannot take the address of this expression");
        }
        return MakeUnary(NodeAddr, operand, PointerTo(operand->type), line);
    }
    if (Accept("*")) {
        return MakeDeref(CastExpression(), line);
    }
    if (Accept("++")) {
        return MakeIncDec(Unary(), 1, false, line);
    }
    if (Accept("--")) {
        return MakeIncDec(Unary(), -1, false, line);
    }
    if (Accept("sizeof")) {
        Type* type;
        bool isTypeName = Is("(") && IsTypeToken(TokAt(pos + 1));
        if (isTypeName) {
            pos++;
            type = TypeName();
            Expect(")");
        }
        else {
            type = Unary()->type;
        }
        if (type->kind == KindStruct && !type->info->complete) {
            Fatal(line, "sizeof of incomplete type");
        }
        if (type->kind == KindArray && type->length < 0) {
            Fatal(line, "sizeof of incomplete array");
        }
        return NewNumber(type->size, line, TypeUInt);
    }
    return Postfix();
}

static Node* CastExpression() {
    bool isCast = Is("(") && IsTypeToken(TokAt(pos + 1));
    if (!isCast) {
        return Unary();
    }
    int line = Tok().line;
    pos++;
    Type* type = TypeName();
    Expect(")");
    if (Is("{")) {
        Fatal(line, "compound literals are not supported");
    }
    Node* expr = CastExpression();
    if (type->kind != KindVoid) {
        NeedScalar(expr, "cast");
    }
    return MakeCast(expr, type);
}

static Node* Multiplicative() {
    Node* expr = CastExpression();
    for (;;) {
        int line = Tok().line;
        if (Accept("*")) {
            expr = MakeBinary(NodeMul, expr, CastExpression(), line, "*");
        }
        else if (Accept("/")) {
            expr = MakeBinary(NodeDiv, expr, CastExpression(), line, "/");
        }
        else if (Accept("%")) {
            expr = MakeBinary(NodeMod, expr, CastExpression(), line, "%");
        }
        else {
            return expr;
        }
    }
}

static Node* Additive() {
    Node* expr = Multiplicative();
    for (;;) {
        int line = Tok().line;
        if (Accept("+")) {
            expr = MakeAdd(expr, Multiplicative(), line);
        }
        else if (Accept("-")) {
            expr = MakeSub(expr, Multiplicative(), line);
        }
        else {
            return expr;
        }
    }
}

static Node* Shift() {
    Node* expr = Additive();
    for (;;) {
        int line = Tok().line;
        if (Accept("<<")) {
            expr = MakeBinary(NodeShl, expr, Additive(), line, "<<");
        }
        else if (Accept(">>")) {
            expr = MakeBinary(NodeShr, expr, Additive(), line, ">>");
        }
        else {
            return expr;
        }
    }
}

static Node* Relational() {
    Node* expr = Shift();
    for (;;) {
        int line = Tok().line;
        if (Accept("<")) {
            expr = MakeCompare(NodeLt, expr, Shift(), line, false);
        }
        else if (Accept("<=")) {
            expr = MakeCompare(NodeLe, expr, Shift(), line, false);
        }
        else if (Accept(">")) {
            expr = MakeCompare(NodeLt, expr, Shift(), line, true);
        }
        else if (Accept(">=")) {
            expr = MakeCompare(NodeLe, expr, Shift(), line, true);
        }
        else {
            return expr;
        }
    }
}

static Node* Equality() {
    Node* expr = Relational();
    for (;;) {
        int line = Tok().line;
        if (Accept("==")) {
            expr = MakeCompare(NodeEq, expr, Relational(), line, false);
        }
        else if (Accept("!=")) {
            expr = MakeCompare(NodeNe, expr, Relational(), line, false);
        }
        else {
            return expr;
        }
    }
}

static Node* BitAnd() {
    Node* expr = Equality();
    while (Is("&")) {
        int line = Tok().line;
        pos++;
        expr = MakeBinary(NodeAnd, expr, Equality(), line, "&");
    }
    return expr;
}

static Node* BitXor() {
    Node* expr = BitAnd();
    while (Is("^")) {
        int line = Tok().line;
        pos++;
        expr = MakeBinary(NodeXor, expr, BitAnd(), line, "^");
    }
    return expr;
}

static Node* BitOr() {
    Node* expr = BitXor();
    while (Is("|")) {
        int line = Tok().line;
        pos++;
        expr = MakeBinary(NodeOr, expr, BitXor(), line, "|");
    }
    return expr;
}

static Node* Logical(NodeKind kind, Node* left, Node* right, int line) {
    NeedScalar(left, "logical operator");
    NeedScalar(right, "logical operator");
    Node* node = NewNode(kind, line);
    node->left = left;
    node->right = right;
    node->type = TypeInt;
    return node;
}

static Node* LogicalAnd() {
    Node* expr = BitOr();
    while (Is("&&")) {
        int line = Tok().line;
        pos++;
        expr = Logical(NodeLogAnd, expr, BitOr(), line);
    }
    return expr;
}

static Node* LogicalOr() {
    Node* expr = LogicalAnd();
    while (Is("||")) {
        int line = Tok().line;
        pos++;
        expr = Logical(NodeLogOr, expr, LogicalAnd(), line);
    }
    return expr;
}

Node* Conditional() {
    Node* condition = LogicalOr();
    if (!Is("?")) {
        return condition;
    }
    int line = Tok().line;
    pos++;
    NeedScalar(condition, "?:");
    Node* node = NewNode(NodeCond, line);
    node->condition = condition;
    node->then = Expression();
    Expect(":");
    node->otherwise = Conditional();

    Type* thenType = Decay(node->then->type);
    Type* otherwiseType = Decay(node->otherwise->type);
    if (IsInteger(thenType) && IsInteger(otherwiseType)) {
        node->type = UsualArithmetic(thenType, otherwiseType);
    }
    else if (thenType->kind == KindPointer) {
        node->type = thenType;
    }
    else if (otherwiseType->kind == KindPointer) {
        node->type = otherwiseType;
    }
    else if (thenType->kind == KindStruct && thenType == otherwiseType) {
        node->type = thenType;
    }
    else if (thenType->kind == KindVoid && otherwiseType->kind == KindVoid) {
        node->type = TypeVoid;
    }
    else {
        Fatal(line, "incompatible operands to ?:");
    }
    return node;
}

struct CompoundOperator {
    const char* text;
    NodeKind op;
};

static const CompoundOperator compoundOperators[] = {
    {"+=", NodeAdd},
    {"-=", NodeSub},
    {"*=", NodeMul},
    {"/=", NodeDiv},
    {"%=", NodeMod},
    {"&=", NodeAnd},
    {"|=", NodeOr},
    {"^=", NodeXor},
    {"<<=", NodeShl},
    {">>=", NodeShr},
};

Node* Assignment() {
    Node* lhs = Conditional();
    int line = Tok().line;
    if (Accept("=")) {
        return MakeAssign(lhs, Assignment(), line);
    }

    for (const CompoundOperator& compound : compoundOperators) {
        if (!Accept(compound.text)) {
            continue;
        }
        Node* rhs = Assignment();
        if (!IsLvalue(lhs)) {
            Fatal(line, "left side of '%s' is not assignable", compound.text);
        }
        Node* node = NewNode(NodeCompound, line);
        node->op = compound.op;
        node->left = lhs;
        node->type = lhs->type;
        if (lhs->type->kind == KindPointer) {
            if (compound.op != NodeAdd && compound.op != NodeSub) {
                Fatal(line, "invalid '%s' on a pointer", compound.text);
            }
            NeedInteger(rhs, compound.text);
            node->right = Scaled(rhs, ElementSize(lhs->type, line));
            node->opUnsigned = true;
        }
        else {
            NeedInteger(lhs, compound.text);
            NeedInteger(rhs, compound.text);
            node->right = rhs;
            node->opUnsigned = OperatorType(compound.op, lhs->type, rhs->type)->isUnsigned;
        }
        return node;
    }
    return lhs;
}

Node* Expression() {
    Node* expr = Assignment();
    while (Is(",")) {
        int line = Tok().line;
        pos++;
        Node* comma = NewNode(NodeComma, line);
        comma->left = expr;
        comma->right = Assignment();
        comma->type = comma->right->type;
        expr = comma;
    }
    return expr;
}

// ===========================================================================
// Constant evaluation (array sizes, case labels, enums, global initializers)

long long Wrap16(long long value, bool isUnsigned) {
    value &= 0xFFFF;
    if (!isUnsigned && value >= 0x8000) {
        value -= 0x10000;
    }
    return value;
}

static long long EvaluateAddress(Node* node, string* label) {
    switch (node->kind) {
        case NodeVar:
            if (node->var->isLocal || !label) {
                Fatal(node->line, "not a constant expression");
            }
            if (!label->empty()) {
                Fatal(node->line, "constant expression refers to two addresses");
            }
            *label = node->var->label;
            return node->value;
        case NodeMember:
            return EvaluateAddress(node->operand, label) + node->member->offset;
        case NodeDeref:
            return Evaluate(node->operand, label);
        default:
            Fatal(node->line, "not a constant expression");
    }
}

// Unsigned operations work on the 16-bit pattern, so drop the sign extension first.
static long long AsUnsigned(long long value, bool isUnsigned) {
    return isUnsigned ? (value & 0xFFFF) : value;
}

long long Evaluate(Node* node, string* label) {
    bool isUnsigned = node->opUnsigned;
    switch (node->kind) {
        case NodeNumber:
            return node->value;
        case NodeAdd: {
            long long left = Evaluate(node->left, label);
            return Wrap16(left + Evaluate(node->right, label), node->type->isUnsigned);
        }
        case NodeSub: {
            long long left = Evaluate(node->left, label);
            return Wrap16(left - Evaluate(node->right), node->type->isUnsigned);
        }
        case NodeMul:
            return Wrap16(Evaluate(node->left) * Evaluate(node->right), isUnsigned);
        case NodeDiv:
        case NodeMod: {
            long long left = AsUnsigned(Evaluate(node->left), isUnsigned);
            long long right = AsUnsigned(Evaluate(node->right), isUnsigned);
            if (right == 0) {
                Fatal(node->line, "division by zero in constant expression");
            }
            if (node->kind == NodeDiv) {
                return Wrap16(left / right, isUnsigned);
            }
            return Wrap16(left % right, isUnsigned);
        }
        case NodeAnd:
            return Wrap16(Evaluate(node->left) & Evaluate(node->right), isUnsigned);
        case NodeOr:
            return Wrap16(Evaluate(node->left) | Evaluate(node->right), isUnsigned);
        case NodeXor:
            return Wrap16(Evaluate(node->left) ^ Evaluate(node->right), isUnsigned);
        case NodeShl: {
            long long count = Evaluate(node->right) & 15;
            return Wrap16(Evaluate(node->left) << count, isUnsigned);
        }
        case NodeShr: {
            long long left = AsUnsigned(Evaluate(node->left), isUnsigned);
            long long count = Evaluate(node->right) & 15;
            return Wrap16(left >> count, isUnsigned);
        }
        case NodeEq:
            return Evaluate(node->left) == Evaluate(node->right);
        case NodeNe:
            return Evaluate(node->left) != Evaluate(node->right);
        case NodeLt:
        case NodeLe: {
            long long left = AsUnsigned(Evaluate(node->left), isUnsigned);
            long long right = AsUnsigned(Evaluate(node->right), isUnsigned);
            if (node->kind == NodeLt) {
                return left < right;
            }
            return left <= right;
        }
        case NodeLogAnd:
            return Evaluate(node->left) && Evaluate(node->right);
        case NodeLogOr:
            return Evaluate(node->left) || Evaluate(node->right);
        case NodeNot:
            return !Evaluate(node->operand);
        case NodeBitNot:
            return Wrap16(~Evaluate(node->operand), node->type->isUnsigned);
        case NodeNeg:
            return Wrap16(-Evaluate(node->operand), node->type->isUnsigned);
        case NodeCond:
            if (Evaluate(node->condition)) {
                return Evaluate(node->then, label);
            }
            return Evaluate(node->otherwise, label);
        case NodeComma:
            return Evaluate(node->right, label);
        case NodeCast: {
            long long value = Evaluate(node->operand, label);
            Type* type = node->type;
            if (type->kind == KindBool) {
                return value != 0;
            }
            if (type->kind == KindChar) {
                if (type->isUnsigned) {
                    return value & 0xFF;
                }
                return (signed char)(value & 0xFF);
            }
            bool isAddress = label && !label->empty();
            if (isAddress) {
                return value;  // label + offset: the assembler does the wrapping
            }
            return Wrap16(value, type->isUnsigned);
        }
        case NodeAddr:
            return EvaluateAddress(node->operand, label);
        case NodeVar:
        case NodeDeref:
        case NodeMember:
            // arrays and functions decay to their (constant) address
            if (node->type->kind == KindArray || node->type->kind == KindFunction) {
                return EvaluateAddress(node, label);
            }
            Fatal(node->line, "not a constant expression");
        default:
            Fatal(node->line, "not a constant expression");
    }
}

long long ConstantExpression() {
    Node* expr = Conditional();
    return Evaluate(expr);
}

}  // namespace Kestrel::Compiler
