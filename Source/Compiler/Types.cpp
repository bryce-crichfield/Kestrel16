// Types.cpp -- C types, and the nodes of the syntax tree.

#include "Types.h"

namespace Kestrel::Compiler {

Type* MakeType(Kind kind, int size, int align, bool isUnsigned) {
    Type* type = new Type;
    type->kind = kind;
    type->size = size;
    type->align = align;
    type->isUnsigned = isUnsigned;
    return type;
}

Type* TypeVoid = MakeType(KindVoid, 1, 1);
Type* TypeBool = MakeType(KindBool, 1, 1, true);
Type* TypeChar = MakeType(KindChar, 1, 1);
Type* TypeUChar = MakeType(KindChar, 1, 1, true);
Type* TypeInt = MakeType(KindInt, 2, 2);
Type* TypeUInt = MakeType(KindInt, 2, 2, true);

Type* PointerTo(Type* base) {
    Type* type = MakeType(KindPointer, 2, 2, true);
    type->base = base;
    return type;
}

Type* ArrayOf(Type* base, int length) {
    Type* type = MakeType(KindArray, length < 0 ? 0 : base->size * length, base->align);
    type->base = base;
    type->length = length;
    return type;
}

bool IsInteger(Type* type) {
    return type->kind == KindBool || type->kind == KindChar || type->kind == KindInt;
}
bool IsScalar(Type* type) {
    return IsInteger(type) || type->kind == KindPointer;
}
int AlignUp(int value, int alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

Type* Decay(Type* type) {
    if (type->kind == KindArray) {
        return PointerTo(type->base);
    }
    if (type->kind == KindFunction) {
        return PointerTo(type);
    }
    return type;
}

Type* Promote(Type* type) {
    if (type->kind == KindInt) {
        return type;
    }
    if (IsInteger(type)) {
        return TypeInt;
    }
    return type;
}

Type* UsualArithmetic(Type* left, Type* right) {
    left = Promote(left);
    right = Promote(right);
    return (left->isUnsigned || right->isUnsigned) ? TypeUInt : TypeInt;
}

// ===========================================================================
// AST nodes

Node* NewNode(NodeKind kind, int line) {
    Node* node = new Node;
    node->kind = kind;
    node->line = line;
    return node;
}

Node* NewNumber(long long value, int line, Type* type) {
    Node* node = NewNode(NodeNumber, line);
    node->value = value;
    node->type = type;
    return node;
}

}  // namespace Kestrel::Compiler
