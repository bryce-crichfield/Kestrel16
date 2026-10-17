// Expressions.cpp -- C expressions: parsed, typed as they are built, and evaluated when constant.

#include "CompilerInternal.h"

#include <cstdint>

namespace Kestrel::Compiler {

static Node* CastExpression();

// ===========================================================================
// Expression construction (with typing)

void NeedScalar(Node* n, const char* what) {
    Type* t = Decay(n->type);
    if (!IsScalar(t))
        Fatal(n->line, "invalid operand to %s", what);
}

void NeedInteger(Node* n, const char* what) {
    if (!IsInteger(n->type))
        Fatal(n->line, "operand of %s must be an integer", what);
}

Node* MakeCast(Node* e, Type* t) {
    if (t->kind == KindStruct || (e->type->kind == KindStruct && t->kind != KindVoid))
        Fatal(e->line, "invalid cast involving a struct");
    Node* n = NewNode(NodeCast, e->line);
    n->a = e;
    n->type = t;
    return n;
}

static Node* MakeBinary(NodeKind kind, Node* a, Node* b, int line, const char* what) {
    NeedInteger(a, what);
    NeedInteger(b, what);
    Node* n = NewNode(kind, line);
    n->a = a;
    n->b = b;
    n->type = (kind == NodeShl || kind == NodeShr) ? Promote(a->type) : UsualArithmetic(a->type, b->type);
    n->opUnsigned = n->type->isUnsigned;
    return n;
}

static int ElementSize(Type* pointer, int line) {
    Type* base = pointer->base;
    if (base->kind == KindVoid || base->kind == KindFunction)
        return 1;
    if (base->kind == KindStruct && !base->info->complete)
        Fatal(line, "arithmetic on pointer to incomplete type");
    return base->size;
}

static Node* Scaled(Node* index, int size) {
    if (size == 1)
        return index;
    Node* m = NewNode(NodeMul, index->line);
    m->a = index;
    m->b = NewNumber(size, index->line);
    m->type = TypeInt;
    return m;
}

static Node* MakeAdd(Node* a, Node* b, int line) {
    Type* ta = Decay(a->type);
    Type* tb = Decay(b->type);
    if (IsInteger(ta) && IsInteger(tb))
        return MakeBinary(NodeAdd, a, b, line, "+");
    if (IsInteger(ta) && tb->kind == KindPointer) {
        std::swap(a, b);
        std::swap(ta, tb);
    }
    if (ta->kind == KindPointer && IsInteger(tb)) {
        Node* n = NewNode(NodeAdd, line);
        n->a = a;
        n->b = Scaled(b, ElementSize(ta, line));
        n->type = ta;
        return n;
    }
    Fatal(line, "invalid operands to +");
}

static Node* MakeSub(Node* a, Node* b, int line) {
    Type* ta = Decay(a->type);
    Type* tb = Decay(b->type);
    if (IsInteger(ta) && IsInteger(tb))
        return MakeBinary(NodeSub, a, b, line, "-");
    if (ta->kind == KindPointer && IsInteger(tb)) {
        Node* n = NewNode(NodeSub, line);
        n->a = a;
        n->b = Scaled(b, ElementSize(ta, line));
        n->type = ta;
        return n;
    }
    if (ta->kind == KindPointer && tb->kind == KindPointer) {
        Node* n = NewNode(NodePtrDiff, line);
        n->a = a;
        n->b = b;
        n->value = ElementSize(ta, line);
        n->type = TypeInt;
        return n;
    }
    Fatal(line, "invalid operands to -");
}

static Node* MakeCompare(NodeKind kind, Node* a, Node* b, int line, bool swap) {
    NeedScalar(a, "comparison");
    NeedScalar(b, "comparison");
    if (swap)
        std::swap(a, b);
    Node* n = NewNode(kind, line);
    n->a = a;
    n->b = b;
    n->type = TypeInt;
    Type* ta = Decay(a->type);
    Type* tb = Decay(b->type);
    n->opUnsigned = ta->kind == KindPointer || tb->kind == KindPointer || UsualArithmetic(ta, tb)->isUnsigned;
    return n;
}

static bool IsLvalue(Node* n) {
    return n->kind == NodeVar || n->kind == NodeDeref || n->kind == NodeMember;
}

Node* MakeAssign(Node* lhs, Node* rhs, int line) {
    if (!IsLvalue(lhs))
        Fatal(line, "left side of assignment is not assignable");
    if (lhs->type->kind == KindArray || lhs->type->kind == KindFunction)
        Fatal(line, "cannot assign to an array or function");
    if (lhs->type->kind == KindStruct) {
        if (rhs->type != lhs->type)
            Fatal(line, "incompatible struct types in assignment");
    } else {
        NeedScalar(rhs, "assignment");
    }
    Node* n = NewNode(NodeAssign, line);
    n->a = lhs;
    n->b = rhs;
    n->type = lhs->type;
    return n;
}

static Node* MakeDeref(Node* e, int line) {
    Type* t = Decay(e->type);
    if (t->kind != KindPointer)
        Fatal(line, "cannot dereference a non-pointer");
    if (t->base->kind == KindVoid)
        Fatal(line, "cannot dereference a void pointer");
    Node* n = NewNode(NodeDeref, line);
    n->a = e;
    n->type = t->base;
    return n;
}

static Node* MakeMember(Node* s, const string& name, int line) {
    if (s->type->kind != KindStruct)
        Fatal(line, "'.%s' used on something that is not a struct", name.c_str());
    if (!s->type->info->complete)
        Fatal(line, "use of incomplete struct");
    for (Member& m : s->type->info->members) {
        if (m.name == name) {
            Node* n = NewNode(NodeMember, line);
            n->a = s;
            n->member = &m;
            n->type = m.type;
            return n;
        }
    }
    Fatal(line, "no member named '%s'", name.c_str());
}

static Node* MakeIncDec(Node* e, int delta, bool isPost, int line) {
    if (!IsLvalue(e))
        Fatal(line, "operand of ++/-- is not assignable");
    NeedScalar(e, "++/--");
    Node* n = NewNode(NodeIncDec, line);
    n->a = e;
    n->type = e->type;
    n->isPost = isPost;
    n->value = e->type->kind == KindPointer ? delta * ElementSize(e->type, line) : delta;
    return n;
}

static Var* NewStringVar(const string& bytes) {
    Var* g = new Var;
    g->label = ".Lstr" + std::to_string(++stringCounter);
    g->type = ArrayOf(TypeChar, (int)bytes.size() + 1);
    g->isDefined = true;
    g->hasInit = true;
    g->data.assign(bytes.begin(), bytes.end());
    g->data.push_back(0);
    globals.push_back(g);
    return g;
}

static Node* VarNode(Var* v, int line) {
    Node* n = NewNode(NodeVar, line);
    n->var = v;
    n->type = v->type;
    return n;
}

// ===========================================================================
// Expression parsing

static Node* CallExpression(Node* fn, int line) {
    Type* ft = fn->type;
    if (ft->kind == KindPointer && ft->base->kind == KindFunction)
        ft = ft->base;
    if (ft->kind != KindFunction)
        Fatal(line, "called object is not a function");
    Node* n = NewNode(NodeCall, line);
    n->a = fn;
    n->type = ft->base;
    if (n->type->kind == KindStruct)
        Fatal(line, "returning structs by value is not supported (pass a pointer)");
    if (!Accept(")")) {
        do {
            Node* arg = Assignment();
            size_t index = n->list.size();
            if (arg->type->kind == KindStruct)
                Fatal(arg->line, "passing structs by value is not supported (pass a pointer)");
            if (arg->type->kind == KindVoid)
                Fatal(arg->line, "void value used as an argument");
            if (!ft->oldStyle && index < ft->params.size())
                arg = MakeCast(arg, ft->params[index]);
            else if (!ft->oldStyle && !ft->variadic)
                Fatal(arg->line, "too many arguments in call");
            n->list.push_back(arg);
        } while (Accept(","));
        Expect(")");
    }
    if (!ft->oldStyle && n->list.size() < ft->params.size())
        Fatal(line, "too few arguments in call");
    return n;
}

static Node* Primary() {
    Token& t = Tok();
    int line = t.line;
    if (Accept("(")) {
        Node* e = Expression();
        Expect(")");
        return e;
    }
    if (t.kind == TokNumber) {
        pos++;
        return NewNumber(t.value, line, t.isUnsigned ? TypeUInt : TypeInt);
    }
    if (t.kind == TokString) {
        pos++;
        return VarNode(NewStringVar(t.text), line);
    }
    if (t.kind == TokIdent) {
        pos++;
        Symbol* s = FindSymbol(t.text);
        if (!s || s->typedefType) {
            if (!s && Is("("))
                Fatal(line, "call to undeclared function '%s' (declare a prototype first)", t.text.c_str());
            Fatal(line, "undeclared identifier '%s'", t.text.c_str());
        }
        if (s->isEnumConstant)
            return NewNumber(s->enumValue, line);
        return VarNode(s->var, line);
    }
    Fatal(line, "expected an expression but found '%s'", t.text.c_str());
}

static Node* Postfix() {
    Node* e = Primary();
    for (;;) {
        int line = Tok().line;
        if (Accept("[")) {
            Node* index = Expression();
            Expect("]");
            e = MakeDeref(MakeAdd(e, index, line), line);
        } else if (Accept("(")) {
            e = CallExpression(e, line);
        } else if (Accept(".")) {
            e = MakeMember(e, ExpectIdent(), line);
        } else if (Accept("->")) {
            e = MakeMember(MakeDeref(e, line), ExpectIdent(), line);
        } else if (Accept("++")) {
            e = MakeIncDec(e, 1, true, line);
        } else if (Accept("--")) {
            e = MakeIncDec(e, -1, true, line);
        } else {
            return e;
        }
    }
}

static Node* Unary() {
    int line = Tok().line;
    if (Accept("+")) {
        Node* e = CastExpression();
        NeedInteger(e, "unary +");
        return MakeCast(e, Promote(e->type));
    }
    if (Accept("-")) {
        Node* e = CastExpression();
        NeedInteger(e, "unary -");
        Node* n = NewNode(NodeNeg, line);
        n->a = e;
        n->type = Promote(e->type);
        return n;
    }
    if (Accept("!")) {
        Node* e = CastExpression();
        NeedScalar(e, "!");
        Node* n = NewNode(NodeNot, line);
        n->a = e;
        n->type = TypeInt;
        return n;
    }
    if (Accept("~")) {
        Node* e = CastExpression();
        NeedInteger(e, "~");
        Node* n = NewNode(NodeBitNot, line);
        n->a = e;
        n->type = Promote(e->type);
        return n;
    }
    if (Accept("&")) {
        Node* e = CastExpression();
        if (!IsLvalue(e))
            Fatal(line, "cannot take the address of this expression");
        Node* n = NewNode(NodeAddr, line);
        n->a = e;
        n->type = PointerTo(e->type);
        return n;
    }
    if (Accept("*"))
        return MakeDeref(CastExpression(), line);
    if (Accept("++"))
        return MakeIncDec(Unary(), 1, false, line);
    if (Accept("--"))
        return MakeIncDec(Unary(), -1, false, line);
    if (Accept("sizeof")) {
        Type* t;
        if (Is("(") && IsTypeToken(TokAt(pos + 1))) {
            pos++;
            t = TypeName();
            Expect(")");
        } else {
            t = Unary()->type;
        }
        if (t->kind == KindStruct && !t->info->complete)
            Fatal(line, "sizeof of incomplete type");
        if (t->kind == KindArray && t->length < 0)
            Fatal(line, "sizeof of incomplete array");
        return NewNumber(t->size, line, TypeUInt);
    }
    return Postfix();
}

static Node* CastExpression() {
    if (Is("(") && IsTypeToken(TokAt(pos + 1))) {
        int line = Tok().line;
        pos++;
        Type* t = TypeName();
        Expect(")");
        if (Is("{"))
            Fatal(line, "compound literals are not supported");
        Node* e = CastExpression();
        if (t->kind != KindVoid)
            NeedScalar(e, "cast");
        return MakeCast(e, t);
    }
    return Unary();
}

static Node* Multiplicative() {
    Node* e = CastExpression();
    for (;;) {
        int line = Tok().line;
        if (Accept("*"))
            e = MakeBinary(NodeMul, e, CastExpression(), line, "*");
        else if (Accept("/"))
            e = MakeBinary(NodeDiv, e, CastExpression(), line, "/");
        else if (Accept("%"))
            e = MakeBinary(NodeMod, e, CastExpression(), line, "%");
        else
            return e;
    }
}

static Node* Additive() {
    Node* e = Multiplicative();
    for (;;) {
        int line = Tok().line;
        if (Accept("+"))
            e = MakeAdd(e, Multiplicative(), line);
        else if (Accept("-"))
            e = MakeSub(e, Multiplicative(), line);
        else
            return e;
    }
}

static Node* Shift() {
    Node* e = Additive();
    for (;;) {
        int line = Tok().line;
        if (Accept("<<"))
            e = MakeBinary(NodeShl, e, Additive(), line, "<<");
        else if (Accept(">>"))
            e = MakeBinary(NodeShr, e, Additive(), line, ">>");
        else
            return e;
    }
}

static Node* Relational() {
    Node* e = Shift();
    for (;;) {
        int line = Tok().line;
        if (Accept("<"))
            e = MakeCompare(NodeLt, e, Shift(), line, false);
        else if (Accept("<="))
            e = MakeCompare(NodeLe, e, Shift(), line, false);
        else if (Accept(">"))
            e = MakeCompare(NodeLt, e, Shift(), line, true);
        else if (Accept(">="))
            e = MakeCompare(NodeLe, e, Shift(), line, true);
        else
            return e;
    }
}

static Node* Equality() {
    Node* e = Relational();
    for (;;) {
        int line = Tok().line;
        if (Accept("=="))
            e = MakeCompare(NodeEq, e, Relational(), line, false);
        else if (Accept("!="))
            e = MakeCompare(NodeNe, e, Relational(), line, false);
        else
            return e;
    }
}

static Node* BitAnd() {
    Node* e = Equality();
    while (Is("&")) {
        int line = Tok().line;
        pos++;
        e = MakeBinary(NodeAnd, e, Equality(), line, "&");
    }
    return e;
}

static Node* BitXor() {
    Node* e = BitAnd();
    while (Is("^")) {
        int line = Tok().line;
        pos++;
        e = MakeBinary(NodeXor, e, BitAnd(), line, "^");
    }
    return e;
}

static Node* BitOr() {
    Node* e = BitXor();
    while (Is("|")) {
        int line = Tok().line;
        pos++;
        e = MakeBinary(NodeOr, e, BitXor(), line, "|");
    }
    return e;
}

static Node* Logical(NodeKind kind, Node* a, Node* b, int line) {
    NeedScalar(a, "logical operator");
    NeedScalar(b, "logical operator");
    Node* n = NewNode(kind, line);
    n->a = a;
    n->b = b;
    n->type = TypeInt;
    return n;
}

static Node* LogicalAnd() {
    Node* e = BitOr();
    while (Is("&&")) {
        int line = Tok().line;
        pos++;
        e = Logical(NodeLogAnd, e, BitOr(), line);
    }
    return e;
}

static Node* LogicalOr() {
    Node* e = LogicalAnd();
    while (Is("||")) {
        int line = Tok().line;
        pos++;
        e = Logical(NodeLogOr, e, LogicalAnd(), line);
    }
    return e;
}

Node* Conditional() {
    Node* cond = LogicalOr();
    if (!Is("?"))
        return cond;
    int line = Tok().line;
    pos++;
    NeedScalar(cond, "?:");
    Node* n = NewNode(NodeCond, line);
    n->a = cond;
    n->b = Expression();
    Expect(":");
    n->c = Conditional();
    Type* tb = Decay(n->b->type);
    Type* tc = Decay(n->c->type);
    if (IsInteger(tb) && IsInteger(tc))
        n->type = UsualArithmetic(tb, tc);
    else if (tb->kind == KindPointer)
        n->type = tb;
    else if (tc->kind == KindPointer)
        n->type = tc;
    else if (tb->kind == KindStruct && tb == tc)
        n->type = tb;
    else if (tb->kind == KindVoid && tc->kind == KindVoid)
        n->type = TypeVoid;
    else
        Fatal(line, "incompatible operands to ?:");
    return n;
}

Node* Assignment() {
    Node* lhs = Conditional();
    int line = Tok().line;
    if (Accept("="))
        return MakeAssign(lhs, Assignment(), line);

    static const struct {
        const char* text;
        NodeKind op;
    } compound[] = {
        {"+=", NodeAdd}, {"-=", NodeSub}, {"*=", NodeMul}, {"/=", NodeDiv}, {"%=", NodeMod}, {"&=", NodeAnd}, {"|=", NodeOr}, {"^=", NodeXor}, {"<<=", NodeShl}, {">>=", NodeShr}};
    for (auto& c : compound) {
        if (!Accept(c.text))
            continue;
        Node* rhs = Assignment();
        if (!IsLvalue(lhs))
            Fatal(line, "left side of '%s' is not assignable", c.text);
        Node* n = NewNode(NodeCompound, line);
        n->op = c.op;
        n->a = lhs;
        n->type = lhs->type;
        if (lhs->type->kind == KindPointer) {
            if (c.op != NodeAdd && c.op != NodeSub)
                Fatal(line, "invalid '%s' on a pointer", c.text);
            NeedInteger(rhs, c.text);
            n->b = Scaled(rhs, ElementSize(lhs->type, line));
            n->opUnsigned = true;
        } else {
            NeedInteger(lhs, c.text);
            NeedInteger(rhs, c.text);
            n->b = rhs;
            Type* opType = (c.op == NodeShl || c.op == NodeShr) ? Promote(lhs->type) : UsualArithmetic(lhs->type, rhs->type);
            n->opUnsigned = opType->isUnsigned;
        }
        return n;
    }
    return lhs;
}

Node* Expression() {
    Node* e = Assignment();
    while (Is(",")) {
        int line = Tok().line;
        pos++;
        Node* n = NewNode(NodeComma, line);
        n->a = e;
        n->b = Assignment();
        n->type = n->b->type;
        e = n;
    }
    return e;
}

// ===========================================================================
// Constant evaluation (array sizes, case labels, enums, global initializers)

long long Wrap16(long long v, bool isUnsigned) {
    v &= 0xFFFF;
    if (!isUnsigned && v >= 0x8000)
        v -= 0x10000;
    return v;
}

static long long EvaluateAddress(Node* n, string* label) {
    switch (n->kind) {
        case NodeVar:
            if (n->var->isLocal || !label)
                Fatal(n->line, "not a constant expression");
            if (!label->empty())
                Fatal(n->line, "constant expression refers to two addresses");
            *label = n->var->label;
            return n->value;
        case NodeMember:
            return EvaluateAddress(n->a, label) + n->member->offset;
        case NodeDeref:
            return Evaluate(n->a, label);
        default:
            Fatal(n->line, "not a constant expression");
    }
}

long long Evaluate(Node* n, string* label) {
    bool u = n->opUnsigned;
    switch (n->kind) {
        case NodeNumber:
            return n->value;
        case NodeAdd: {
            long long a = Evaluate(n->a, label);
            return Wrap16(a + Evaluate(n->b, label), n->type->isUnsigned);
        }
        case NodeSub: {
            long long a = Evaluate(n->a, label);
            return Wrap16(a - Evaluate(n->b), n->type->isUnsigned);
        }
        case NodeMul:
            return Wrap16(Evaluate(n->a) * Evaluate(n->b), u);
        case NodeDiv:
        case NodeMod: {
            long long a = Evaluate(n->a), b = Evaluate(n->b);
            if (u) {
                a &= 0xFFFF;
                b &= 0xFFFF;
            }
            if (b == 0)
                Fatal(n->line, "division by zero in constant expression");
            return Wrap16(n->kind == NodeDiv ? a / b : a % b, u);
        }
        case NodeAnd:
            return Wrap16(Evaluate(n->a) & Evaluate(n->b), u);
        case NodeOr:
            return Wrap16(Evaluate(n->a) | Evaluate(n->b), u);
        case NodeXor:
            return Wrap16(Evaluate(n->a) ^ Evaluate(n->b), u);
        case NodeShl:
            return Wrap16(Evaluate(n->a) << (Evaluate(n->b) & 15), u);
        case NodeShr: {
            long long a = Evaluate(n->a);
            if (u)
                a &= 0xFFFF;
            return Wrap16(a >> (Evaluate(n->b) & 15), u);
        }
        case NodeEq:
            return Evaluate(n->a) == Evaluate(n->b);
        case NodeNe:
            return Evaluate(n->a) != Evaluate(n->b);
        case NodeLt:
        case NodeLe: {
            long long a = Evaluate(n->a), b = Evaluate(n->b);
            if (u) {
                a &= 0xFFFF;
                b &= 0xFFFF;
            }
            return n->kind == NodeLt ? a < b : a <= b;
        }
        case NodeLogAnd:
            return Evaluate(n->a) && Evaluate(n->b);
        case NodeLogOr:
            return Evaluate(n->a) || Evaluate(n->b);
        case NodeNot:
            return !Evaluate(n->a);
        case NodeBitNot:
            return Wrap16(~Evaluate(n->a), n->type->isUnsigned);
        case NodeNeg:
            return Wrap16(-Evaluate(n->a), n->type->isUnsigned);
        case NodeCond:
            return Evaluate(n->a) ? Evaluate(n->b, label) : Evaluate(n->c, label);
        case NodeComma:
            return Evaluate(n->b, label);
        case NodeCast: {
            long long v = Evaluate(n->a, label);
            Type* t = n->type;
            if (t->kind == KindBool)
                return v != 0;
            if (t->kind == KindChar)
                return t->isUnsigned ? (v & 0xFF) : (signed char)(v & 0xFF);
            if (label && !label->empty())
                return v;
            return Wrap16(v, t->isUnsigned);
        }
        case NodeAddr:
            return EvaluateAddress(n->a, label);
        case NodeVar:
        case NodeDeref:
        case NodeMember:
            // arrays and functions decay to their (constant) address
            if (n->type->kind == KindArray || n->type->kind == KindFunction)
                return EvaluateAddress(n, label);
            Fatal(n->line, "not a constant expression");
        default:
            Fatal(n->line, "not a constant expression");
    }
}

long long ConstantExpression() {
    Node* e = Conditional();
    return Evaluate(e);
}

}  // namespace Kestrel::Compiler
