// Types.cpp -- C types, and the nodes of the syntax tree.

#include "CompilerInternal.h"

namespace Kestrel::Compiler {

Type* MakeType(Kind kind, int size, int align, bool isUnsigned)
{
    Type* t = new Type;
    t->kind = kind;
    t->size = size;
    t->align = align;
    t->isUnsigned = isUnsigned;
    return t;
}

Type* TypeVoid  = MakeType(KindVoid, 1, 1);
Type* TypeBool  = MakeType(KindBool, 1, 1, true);
Type* TypeChar  = MakeType(KindChar, 1, 1);
Type* TypeUChar = MakeType(KindChar, 1, 1, true);
Type* TypeInt   = MakeType(KindInt, 2, 2);
Type* TypeUInt  = MakeType(KindInt, 2, 2, true);

Type* PointerTo(Type* base)
{
    Type* t = MakeType(KindPointer, 2, 2, true);
    t->base = base;
    return t;
}

Type* ArrayOf(Type* base, int length)
{
    Type* t = MakeType(KindArray, length < 0 ? 0 : base->size * length, base->align);
    t->base = base;
    t->length = length;
    return t;
}

bool IsInteger(Type* t) { return t->kind == KindBool || t->kind == KindChar || t->kind == KindInt; }
bool IsScalar(Type* t) { return IsInteger(t) || t->kind == KindPointer; }
int AlignUp(int n, int a) { return (n + a - 1) / a * a; }

Type* Decay(Type* t)
{
    if (t->kind == KindArray) return PointerTo(t->base);
    if (t->kind == KindFunction) return PointerTo(t);
    return t;
}

Type* Promote(Type* t)
{
    if (t->kind == KindInt) return t;
    if (IsInteger(t)) return TypeInt;
    return t;
}

Type* UsualArithmetic(Type* a, Type* b)
{
    a = Promote(a);
    b = Promote(b);
    return (a->isUnsigned || b->isUnsigned) ? TypeUInt : TypeInt;
}

// ===========================================================================
// AST nodes

Node* NewNode(NodeKind kind, int line)
{
    Node* n = new Node;
    n->kind = kind;
    n->line = line;
    return n;
}

Node* NewNumber(long long value, int line, Type* type)
{
    Node* n = NewNode(NodeNumber, line);
    n->value = value;
    n->type = type;
    return n;
}

}  // namespace Kestrel::Compiler
