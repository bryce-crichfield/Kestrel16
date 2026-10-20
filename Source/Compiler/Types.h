// Types.h -- C types, variables and syntax tree nodes (Types.cpp).
// Internal to Source/Compiler; Compiler.h is the compiler's interface.

#pragma once

#include "Errors.h"

#include <cstdint>
#include <map>
#include <utility>

namespace Kestrel::Compiler {

enum Kind { KindVoid,
            KindBool,
            KindChar,
            KindInt,
            KindPointer,
            KindArray,
            KindFunction,
            KindStruct };

struct Type;

struct Member {
    string name;
    Type* type;
    int offset;
};

struct StructInfo {
    vector<Member> members;
    bool complete = false;
    bool isUnion = false;
};

struct Type {
    Kind kind;
    int size = 0;
    int align = 1;
    bool isUnsigned = false;
    Type* base = nullptr;  // pointee, element, or return type
    int length = 0;        // array length, -1 = incomplete
    vector<Type*> params;
    vector<string> paramNames;
    bool variadic = false;
    bool oldStyle = false;  // "int f()" -- arguments not checked
    StructInfo* info = nullptr;
};

extern Type* TypeVoid;
extern Type* TypeBool;
extern Type* TypeChar;
extern Type* TypeUChar;
extern Type* TypeInt;
extern Type* TypeUInt;

Type* MakeType(Kind kind, int size, int align, bool isUnsigned = false);
Type* PointerTo(Type* base);
Type* ArrayOf(Type* base, int length);
bool IsInteger(Type* type);
bool IsScalar(Type* type);
int AlignUp(int value, int alignment);
Type* Decay(Type* type);
Type* Promote(Type* type);
Type* UsualArithmetic(Type* left, Type* right);

struct Var {
    string name;
    string label;  // assembler label for globals
    Type* type = nullptr;
    bool isLocal = false;
    int offset = 0;          // frame offset for locals
    bool isDefined = false;  // has a non-extern definition (or a body)
    bool hasInit = false;
    vector<uint8_t> data;
    std::map<int, std::pair<string, int>> relocations;  // offset -> label+addend word
};

enum NodeKind {
    NodeNumber,
    NodeVar,
    NodeAdd,
    NodeSub,
    NodeMul,
    NodeDiv,
    NodeMod,
    NodeAnd,
    NodeOr,
    NodeXor,
    NodeShl,
    NodeShr,
    NodeEq,
    NodeNe,
    NodeLt,
    NodeLe,
    NodeLogAnd,
    NodeLogOr,
    NodeNot,
    NodeBitNot,
    NodeNeg,
    NodeAddr,
    NodeDeref,
    NodeMember,
    NodeAssign,
    NodeCompound,
    NodeIncDec,
    NodeCond,
    NodeComma,
    NodeCall,
    NodeCast,
    NodePtrDiff,
    NodeZero,
    NodeBlock,
    NodeIf,
    NodeWhile,
    NodeDo,
    NodeFor,
    NodeReturn,
    NodeBreak,
    NodeContinue,
    NodeSwitch,
    NodeCase,
    NodeGoto,
    NodeLabel,
    NodeExprStmt
};

struct Node {
    NodeKind kind;
    Type* type = nullptr;

    // Children. Which ones a node uses depends on its kind.
    Node* left = nullptr;         // binary operators, comparisons, &&, ||, comma, assignments, pointer difference
    Node* right = nullptr;        //
    Node* operand = nullptr;      // unary operators, casts, address-of, dereference, member access, ++/--
    Node* callee = nullptr;       // call (the arguments are in list)
    Node* expression = nullptr;   // expression statement, return (null for a bare return)
    Node* condition = nullptr;    // if, ?:, while, do, switch, for (null when omitted)
    Node* then = nullptr;         // if, ?:
    Node* otherwise = nullptr;    // ?:, if (null when there is no else)
    Node* init = nullptr;         // for (null when omitted)
    Node* step = nullptr;         // for (null when omitted)
    Node* body = nullptr;         // while, do, for, switch, case, label
    Node* defaultCase = nullptr;  // switch (null when there is no default)
    vector<Node*> list;           // block statements, call arguments, switch cases

    long long value = 0;  // number, var byte offset, inc delta, case value, ptrdiff size
    Var* var = nullptr;
    Member* member = nullptr;
    NodeKind op = NodeNumber;  // compound assignment operator
    bool opUnsigned = false;   // unsigned compare / divide / shift
    bool isPost = false;
    string name;
    int line = 0;
    int label = 0;
};

Node* NewNode(NodeKind kind, int line);
Node* NewNumber(long long value, int line, Type* type = TypeInt);

}  // namespace Kestrel::Compiler
