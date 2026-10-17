// Parser.cpp -- declarations, initializers and statements; each function is handed to the code
// generator as soon as its body is parsed.

#include "CompilerInternal.h"

#include <functional>

namespace Kestrel::Compiler {

// ===========================================================================
// Parser state

vector<Token> tokens;
size_t pos;
vector<Var*> globals;
std::map<string, Var*> globalsByName;
int labelCounter;
int stringCounter;

static vector<std::map<string, Symbol>> scopes;
static vector<std::map<string, Type*>> tagScopes;
static vector<Node*> switchStack;
static int frameSize;
static Type* currentReturnType;
static int staticCounter;

Token& Tok() {
    return tokens[pos];
}
Token& TokAt(size_t i) {
    return tokens[i < tokens.size() ? i : tokens.size() - 1];
}

bool IsText(const Token& t, const char* s) {
    return (t.kind == TokPunct || t.kind == TokKeyword) && t.text == s;
}
bool Is(const char* s) {
    return IsText(Tok(), s);
}
bool Accept(const char* s) {
    if (Is(s)) {
        pos++;
        return true;
    }
    return false;
}
void Expect(const char* s) {
    if (!Accept(s)) {
        Fatal(Tok().line, "expected '%s' but found '%s'", s, Tok().text.c_str());
    }
}
string ExpectIdent() {
    if (Tok().kind != TokIdent) {
        Fatal(Tok().line, "expected an identifier but found '%s'", Tok().text.c_str());
    }
    return tokens[pos++].text;
}

void EnterScope() {
    scopes.emplace_back();
    tagScopes.emplace_back();
}
static void LeaveScope() {
    scopes.pop_back();
    tagScopes.pop_back();
}

Symbol* FindSymbol(const string& name) {
    for (int i = (int)scopes.size() - 1; i >= 0; i--) {
        auto it = scopes[i].find(name);
        if (it != scopes[i].end()) {
            return &it->second;
        }
    }
    return nullptr;
}

static Type* FindTag(const string& name) {
    for (int i = (int)tagScopes.size() - 1; i >= 0; i--) {
        auto it = tagScopes[i].find(name);
        if (it != tagScopes[i].end()) {
            return it->second;
        }
    }
    return nullptr;
}

bool IsTypeToken(const Token& t) {
    static const std::set<string> typeWords = {
        "void", "_Bool", "char", "short", "int", "long", "signed", "unsigned", "struct", "union",
        "enum", "const", "volatile", "typedef", "static", "extern", "inline", "register", "auto",
        "restrict", "float", "double"};
    if (t.kind == TokKeyword) {
        return typeWords.count(t.text) > 0;
    }
    if (t.kind == TokIdent) {
        Symbol* s = FindSymbol(t.text);
        return s && s->typedefType;
    }
    return false;
}

static Node* Statement();
static Node* CompoundStatement();
static Type* ParseDeclarator(Type* type, string& name);
struct StorageClass;
static Type* DeclarationSpecifiers(StorageClass* storage);
static Type* TypeSuffix(Type* type);

// ===========================================================================
// Declarations

struct StorageClass {
    bool isTypedef = false;
    bool isStatic = false;
    bool isExtern = false;
};

static Type* StructDeclaration(bool isUnion) {
    int line = Tok().line;
    string tag;
    if (Tok().kind == TokIdent) {
        tag = tokens[pos++].text;
    }

    if (!tag.empty() && !Is("{")) {
        Type* existing = FindTag(tag);
        if (existing) {
            return existing;
        }
        Type* t = MakeType(KindStruct, 0, 1);
        t->info = new StructInfo;
        t->info->isUnion = isUnion;
        tagScopes.back()[tag] = t;
        return t;
    }

    Type* t = nullptr;
    if (!tag.empty()) {
        auto it = tagScopes.back().find(tag);
        if (it != tagScopes.back().end()) {
            if (it->second->info->complete) {
                Fatal(line, "redefinition of '%s %s'", isUnion ? "union" : "struct", tag.c_str());
            }
            t = it->second;
        }
    }
    if (!t) {
        t = MakeType(KindStruct, 0, 1);
        t->info = new StructInfo;
        t->info->isUnion = isUnion;
        if (!tag.empty()) {
            tagScopes.back()[tag] = t;
        }
    }

    Expect("{");
    int offset = 0, size = 0, align = 1;
    while (!Accept("}")) {
        Type* base = DeclarationSpecifiers(nullptr);
        if (Accept(";")) {
            continue;
        }
        do {
            string name;
            int memberLine = Tok().line;
            Type* memberType = ParseDeclarator(base, name);
            if (Is(":")) {
                Fatal(memberLine, "bit-fields are not supported");
            }
            if (memberType->kind == KindStruct && !memberType->info->complete) {
                Fatal(memberLine, "member '%s' has incomplete type", name.c_str());
            }
            if (memberType->kind == KindFunction || memberType->kind == KindVoid) {
                Fatal(memberLine, "member '%s' has invalid type", name.c_str());
            }
            if (memberType->kind == KindArray && memberType->length < 0) {
                Fatal(memberLine, "flexible array members are not supported");
            }
            int memberOffset;
            if (isUnion) {
                memberOffset = 0;
                if (memberType->size > size) {
                    size = memberType->size;
                }
            }
            else {
                offset = AlignUp(offset, memberType->align);
                memberOffset = offset;
                offset += memberType->size;
                size = offset;
            }
            if (memberType->align > align) {
                align = memberType->align;
            }
            t->info->members.push_back({name, memberType, memberOffset});
        } while (Accept(","));
        Expect(";");
    }
    t->size = AlignUp(size, align);
    t->align = align;
    t->info->complete = true;
    return t;
}

static Type* EnumDeclaration() {
    if (Tok().kind == TokIdent) {
        pos++;  // tag is accepted and ignored; enums are int
    }
    if (!Accept("{")) {
        return TypeInt;
    }
    long long value = 0;
    while (!Accept("}")) {
        string name = ExpectIdent();
        if (Accept("=")) {
            value = ConstantExpression();
        }
        Symbol s;
        s.isEnumConstant = true;
        s.enumValue = value;
        scopes.back()[name] = s;
        value++;
        if (!Accept(",")) {
            Expect("}");
            break;
        }
    }
    return TypeInt;
}

static Type* DeclarationSpecifiers(StorageClass* storage) {
    int line = Tok().line;
    int nUnsigned = 0, nChar = 0, nVoid = 0, nBool = 0, nOther = 0;
    Type* named = nullptr;

    for (;;) {
        Token& t = Tok();
        if (t.kind == TokKeyword) {
            const string& s = t.text;
            if (s == "typedef" || s == "static" || s == "extern") {
                if (!storage) {
                    Fatal(t.line, "'%s' is not allowed here", s.c_str());
                }
                if (s == "typedef") {
                    storage->isTypedef = true;
                }
                if (s == "static") {
                    storage->isStatic = true;
                }
                if (s == "extern") {
                    storage->isExtern = true;
                }
                pos++;
                continue;
            }
            if (s == "const" || s == "volatile" || s == "inline" || s == "register" || s == "auto" || s == "restrict") {
                pos++;
                continue;
            }
            if (s == "float" || s == "double") {
                Fatal(t.line, "floating point is not supported");
            }
            if (s == "struct" || s == "union") {
                if (named || nOther || nChar || nVoid || nBool) {
                    Fatal(t.line, "invalid type");
                }
                pos++;
                named = StructDeclaration(s == "union");
                continue;
            }
            if (s == "enum") {
                if (named || nOther || nChar || nVoid || nBool) {
                    Fatal(t.line, "invalid type");
                }
                pos++;
                named = EnumDeclaration();
                continue;
            }
            if (s == "unsigned") {
                nUnsigned++;
            }
            else if (s == "signed" || s == "short" || s == "long" || s == "int") {
                nOther++;
            }
            else if (s == "char") {
                nChar++;
            }
            else if (s == "void") {
                nVoid++;
            }
            else if (s == "_Bool") {
                nBool++;
            }
            else {
                break;
            }
            pos++;
            continue;
        }
        if (t.kind == TokIdent && !named && !nUnsigned && !nOther && !nChar && !nVoid && !nBool) {
            Symbol* sym = FindSymbol(t.text);
            if (sym && sym->typedefType) {
                named = sym->typedefType;
                pos++;
                continue;
            }
        }
        break;
    }
    if (named) {
        return named;
    }
    if (nVoid) {
        return TypeVoid;
    }
    if (nBool) {
        return TypeBool;
    }
    if (nChar) {
        return nUnsigned ? TypeUChar : TypeChar;
    }
    if (nUnsigned || nOther) {
        return nUnsigned ? TypeUInt : TypeInt;
    }
    Fatal(line, "expected a type but found '%s'", Tok().text.c_str());
}

static Type* FunctionParameters(Type* returnType) {
    Type* f = MakeType(KindFunction, 1, 1);
    f->base = returnType;
    if (Is("void") && IsText(TokAt(pos + 1), ")")) {
        pos += 2;
        return f;
    }
    if (Accept(")")) {
        f->oldStyle = true;
        return f;
    }
    for (;;) {
        if (Accept("...")) {
            f->variadic = true;
            Expect(")");
            break;
        }
        Type* base = DeclarationSpecifiers(nullptr);
        string name;
        Type* t = ParseDeclarator(base, name);
        if (t->kind == KindArray) {
            t = PointerTo(t->base);
        }
        else if (t->kind == KindFunction) {
            t = PointerTo(t);
        }
        f->params.push_back(t);
        f->paramNames.push_back(name);
        if (Accept(")")) {
            break;
        }
        Expect(",");
    }
    return f;
}

static Type* TypeSuffix(Type* type) {
    if (Accept("(")) {
        return FunctionParameters(type);
    }
    if (Accept("[")) {
        int line = Tok().line;
        int length = -1;
        if (!Is("]")) {
            long long n = ConstantExpression();
            if (n < 0) {
                Fatal(line, "array size is negative");
            }
            length = (int)n;
        }
        Expect("]");
        Type* element = TypeSuffix(type);
        if (element->kind == KindArray && element->length < 0) {
            Fatal(line, "array has incomplete element type");
        }
        return ArrayOf(element, length);
    }
    return type;
}

static Type* ParseDeclarator(Type* type, string& name) {
    while (Accept("*")) {
        type = PointerTo(type);
        while (Accept("const") || Accept("volatile") || Accept("restrict")) {
        }
    }
    if (Is("(")) {
        const Token& next = TokAt(pos + 1);
        bool nested = IsText(next, "*") || IsText(next, "(") || IsText(next, "[") ||
                      (next.kind == TokIdent && !IsTypeToken(next));
        if (nested) {
            pos++;
            size_t start = pos;
            int depth = 1;
            while (depth) {
                if (Tok().kind == TokEof) {
                    Fatal(Tok().line, "unbalanced parentheses in declarator");
                }
                if (Is("(")) {
                    depth++;
                }
                else if (Is(")")) {
                    depth--;
                }
                pos++;
            }
            Type* outer = TypeSuffix(type);
            size_t end = pos;
            pos = start;
            Type* result = ParseDeclarator(outer, name);
            Expect(")");
            pos = end;
            return result;
        }
    }
    if (Tok().kind == TokIdent) {
        name = tokens[pos++].text;
    }
    return TypeSuffix(type);
}

Type* TypeName() {
    Type* base = DeclarationSpecifiers(nullptr);
    string name;
    Type* t = ParseDeclarator(base, name);
    if (!name.empty()) {
        Fatal(Tok().line, "unexpected name '%s' in type", name.c_str());
    }
    return t;
}

// ===========================================================================
// Initializers

using LeafFunction = std::function<void(Type*, int, Node*)>;

// Parses an initializer for type t at byte offset off. Returns the element count
// for arrays (used to complete "int a[] = {...}").
static int Initializer(Type* t, int off, const LeafFunction& leaf) {
    int line = Tok().line;
    if (t->kind == KindArray) {
        Type* element = t->base;
        bool stringInit = element->kind == KindChar &&
                          (Tok().kind == TokString || (Is("{") && TokAt(pos + 1).kind == TokString));
        if (stringInit) {
            bool braced = Accept("{");
            string s = Tok().text;
            pos++;
            int count = t->length < 0 ? (int)s.size() + 1 : t->length;
            if ((int)s.size() > count) {
                Fatal(line, "string is too long for the array");
            }
            for (int i = 0; i < count && i <= (int)s.size(); i++) {
                leaf(element, off + i, NewNumber(i < (int)s.size() ? (signed char)s[i] : 0, line));
            }
            if (braced) {
                Accept(",");
                Expect("}");
            }
            return count;
        }
        if (!Accept("{")) {
            Fatal(line, "array initializer needs braces");
        }
        int i = 0;
        while (!Accept("}")) {
            if (Is(".") || Is("[")) {
                Fatal(Tok().line, "designated initializers are not supported");
            }
            if (t->length >= 0 && i >= t->length) {
                Fatal(Tok().line, "too many initializers for the array");
            }
            Initializer(element, off + i * element->size, leaf);
            i++;
            if (!Accept(",")) {
                Expect("}");
                break;
            }
        }
        return i;
    }
    if (t->kind == KindStruct) {
        if (Accept("{")) {
            size_t i = 0;
            auto& members = t->info->members;
            while (!Accept("}")) {
                if (Is(".") || Is("[")) {
                    Fatal(Tok().line, "designated initializers are not supported");
                }
                if (i >= members.size() || (t->info->isUnion && i >= 1)) {
                    Fatal(Tok().line, "too many initializers for the struct");
                }
                Initializer(members[i].type, off + members[i].offset, leaf);
                i++;
                if (!Accept(",")) {
                    Expect("}");
                    break;
                }
            }
            return 1;
        }
        leaf(t, off, Assignment());
        return 1;
    }
    bool braced = Accept("{");
    Node* e = Assignment();
    if (braced) {
        Accept(",");
        Expect("}");
    }
    leaf(t, off, e);
    return 1;
}

static void GlobalInitializer(Var* g) {
    if (g->hasInit) {
        Fatal(Tok().line, "redefinition of '%s'", g->name.c_str());
    }
    g->hasInit = true;
    g->data.clear();
    g->relocations.clear();
    auto leaf = [&](Type* t, int off, Node* e) {
        if (t->kind == KindStruct) {
            Fatal(e->line, "struct copy is not a constant initializer");
        }
        if (!IsScalar(Decay(e->type))) {
            Fatal(e->line, "invalid initializer");
        }
        string label;
        long long v = Evaluate(MakeCast(e, t), &label);
        if ((int)g->data.size() < off + t->size) {
            g->data.resize(off + t->size, 0);
        }
        if (t->size == 1) {
            if (!label.empty()) {
                Fatal(e->line, "an address does not fit in a char");
            }
            g->data[off] = (uint8_t)v;
        }
        else {
            if (!label.empty()) {
                g->relocations[off] = {label, (int)v};
            }
            else {
                g->data[off] = (uint8_t)(v & 0xFF);
                g->data[off + 1] = (uint8_t)((v >> 8) & 0xFF);
            }
        }
    };
    int count = Initializer(g->type, 0, leaf);
    if (g->type->kind == KindArray && g->type->length < 0) {
        g->type = ArrayOf(g->type->base, count);
    }
    g->data.resize(g->type->size, 0);
}

// ===========================================================================
// Globals, locals, statements

static Var* DeclareGlobal(const string& name, Type* t, int line) {
    auto it = globalsByName.find(name);
    if (it != globalsByName.end()) {
        Var* g = it->second;
        if ((g->type->kind == KindFunction) != (t->kind == KindFunction)) {
            Fatal(line, "'%s' redeclared as a different kind of symbol", name.c_str());
        }
        if (g->type->kind == KindArray && g->type->length < 0 && t->kind == KindArray) {
            g->type = t;
        }
        if (g->type->kind == KindFunction && g->type->oldStyle && !t->oldStyle) {
            g->type = t;
        }
        scopes[0][name].var = g;
        return g;
    }
    Var* g = new Var;
    g->name = name;
    g->label = "_" + name;
    g->type = t;
    globals.push_back(g);
    globalsByName[name] = g;
    Symbol s;
    s.var = g;
    scopes[0][name] = s;
    return g;
}

static Node* LocalDeclaration() {
    Node* block = NewNode(NodeBlock, Tok().line);
    StorageClass storage;
    Type* base = DeclarationSpecifiers(&storage);
    if (Accept(";")) {
        return block;
    }
    do {
        int line = Tok().line;
        string name;
        Type* t = ParseDeclarator(base, name);
        if (name.empty()) {
            Fatal(line, "declaration needs a name");
        }
        if (storage.isTypedef) {
            Symbol s;
            s.typedefType = t;
            scopes.back()[name] = s;
            continue;
        }
        if (t->kind == KindFunction || storage.isExtern) {
            Var* g = DeclareGlobal(name, t, line);
            Symbol s;
            s.var = g;
            scopes.back()[name] = s;
            continue;
        }
        if (t->kind == KindVoid) {
            Fatal(line, "variable '%s' declared void", name.c_str());
        }
        if (t->kind == KindStruct && !t->info->complete) {
            Fatal(line, "variable '%s' has incomplete type", name.c_str());
        }

        if (storage.isStatic) {
            Var* g = new Var;
            g->name = name;
            g->label = ".Lstatic" + std::to_string(++staticCounter) + "_" + name;
            g->type = t;
            g->isDefined = true;
            globals.push_back(g);
            Symbol s;
            s.var = g;
            scopes.back()[name] = s;
            if (Accept("=")) {
                GlobalInitializer(g);
            }
            if (g->type->kind == KindArray && g->type->length < 0) {
                Fatal(line, "array '%s' needs a size", name.c_str());
            }
            continue;
        }

        Var* v = new Var;
        v->name = name;
        v->type = t;
        v->isLocal = true;
        Symbol s;
        s.var = v;
        scopes.back()[name] = s;

        vector<Node*> stores;
        bool hasInit = Accept("=");
        if (hasInit) {
            auto leaf = [&](Type* lt, int off, Node* e) {
                Node* lhs = NewNode(NodeVar, e->line);
                lhs->var = v;
                lhs->value = off;
                lhs->type = lt;
                Node* stmt = NewNode(NodeExprStmt, e->line);
                stmt->a = MakeAssign(lhs, e, e->line);
                stores.push_back(stmt);
            };
            int count = Initializer(t, 0, leaf);
            if (t->kind == KindArray && t->length < 0) {
                v->type = t = ArrayOf(t->base, count);
            }
        }
        if (t->kind == KindArray && t->length < 0) {
            Fatal(line, "array '%s' needs a size", name.c_str());
        }
        frameSize += t->size;
        if (t->align >= 2) {
            frameSize = AlignUp(frameSize, 2);
        }
        v->offset = -frameSize;

        if (hasInit && (t->kind == KindArray || t->kind == KindStruct) && !(stores.size() == 1 && stores[0]->a->a->type == t)) {
            Node* zero = NewNode(NodeZero, line);
            zero->var = v;
            block->list.push_back(zero);
        }
        for (Node* st : stores) {
            block->list.push_back(st);
        }
    } while (Accept(","));
    Expect(";");
    return block;
}

static Node* Statement() {
    Token& t = Tok();
    int line = t.line;

    if (Is("{")) {
        return CompoundStatement();
    }

    if (Accept("if")) {
        Node* n = NewNode(NodeIf, line);
        Expect("(");
        n->a = Expression();
        NeedScalar(n->a, "if");
        Expect(")");
        n->b = Statement();
        if (Accept("else")) {
            n->c = Statement();
        }
        return n;
    }
    if (Accept("while")) {
        Node* n = NewNode(NodeWhile, line);
        Expect("(");
        n->a = Expression();
        NeedScalar(n->a, "while");
        Expect(")");
        n->b = Statement();
        return n;
    }
    if (Accept("do")) {
        Node* n = NewNode(NodeDo, line);
        n->b = Statement();
        Expect("while");
        Expect("(");
        n->a = Expression();
        NeedScalar(n->a, "do-while");
        Expect(")");
        Expect(";");
        return n;
    }
    if (Accept("for")) {
        Node* n = NewNode(NodeFor, line);
        Expect("(");
        EnterScope();
        if (IsTypeToken(Tok())) {
            n->a = LocalDeclaration();
        }
        else if (!Accept(";")) {
            n->a = NewNode(NodeExprStmt, line);
            n->a->a = Expression();
            Expect(";");
        }
        if (!Is(";")) {
            n->b = Expression();
            NeedScalar(n->b, "for");
        }
        Expect(";");
        if (!Is(")")) {
            n->c = Expression();
        }
        Expect(")");
        n->d = Statement();
        LeaveScope();
        return n;
    }
    if (Accept("return")) {
        Node* n = NewNode(NodeReturn, line);
        if (!Accept(";")) {
            Node* e = Expression();
            Expect(";");
            if (currentReturnType->kind == KindVoid) {
                n->a = e;  // allowed if e is void; value ignored
            }
            else {
                if (e->type->kind == KindStruct || currentReturnType->kind == KindStruct) {
                    Fatal(line, "returning structs by value is not supported");
                }
                NeedScalar(e, "return");
                n->a = MakeCast(e, currentReturnType);
            }
        }
        return n;
    }
    if (Accept("break")) {
        Expect(";");
        return NewNode(NodeBreak, line);
    }
    if (Accept("continue")) {
        Expect(";");
        return NewNode(NodeContinue, line);
    }
    if (Accept("goto")) {
        Node* n = NewNode(NodeGoto, line);
        n->name = ExpectIdent();
        Expect(";");
        return n;
    }
    if (Accept("switch")) {
        Node* n = NewNode(NodeSwitch, line);
        Expect("(");
        n->a = Expression();
        NeedInteger(n->a, "switch");
        Expect(")");
        switchStack.push_back(n);
        n->b = Statement();
        switchStack.pop_back();
        return n;
    }
    if (Accept("case")) {
        if (switchStack.empty()) {
            Fatal(line, "'case' outside of a switch");
        }
        Node* n = NewNode(NodeCase, line);
        n->value = ConstantExpression();
        Expect(":");
        n->label = ++labelCounter;
        for (Node* other : switchStack.back()->list) {
            if (Wrap16(other->value, false) == Wrap16(n->value, false)) {
                Fatal(line, "duplicate case value %lld", n->value);
            }
        }
        switchStack.back()->list.push_back(n);
        n->a = Is("}") ? NewNode(NodeBlock, line) : Statement();
        return n;
    }
    if (Accept("default")) {
        if (switchStack.empty()) {
            Fatal(line, "'default' outside of a switch");
        }
        if (switchStack.back()->d) {
            Fatal(line, "multiple default labels");
        }
        Expect(":");
        Node* n = NewNode(NodeCase, line);
        n->label = ++labelCounter;
        switchStack.back()->d = n;
        n->a = Is("}") ? NewNode(NodeBlock, line) : Statement();
        return n;
    }
    if (t.kind == TokIdent && IsText(TokAt(pos + 1), ":")) {
        Node* n = NewNode(NodeLabel, line);
        n->name = t.text;
        pos += 2;
        n->a = Is("}") ? NewNode(NodeBlock, line) : Statement();
        return n;
    }
    if (Accept(";")) {
        return NewNode(NodeBlock, line);
    }

    Node* n = NewNode(NodeExprStmt, line);
    n->a = Expression();
    Expect(";");
    return n;
}

static Node* CompoundStatement() {
    Node* n = NewNode(NodeBlock, Tok().line);
    Expect("{");
    EnterScope();
    while (!Accept("}")) {
        if (Tok().kind == TokEof) {
            Fatal(Tok().line, "missing '}'");
        }
        bool isLabel = Tok().kind == TokIdent && IsText(TokAt(pos + 1), ":");
        if (IsTypeToken(Tok()) && !isLabel) {
            n->list.push_back(LocalDeclaration());
        }
        else {
            n->list.push_back(Statement());
        }
    }
    LeaveScope();
    return n;
}

static void CheckGotos(Node* n, std::set<string>& defined, vector<Node*>& used) {
    if (!n) {
        return;
    }
    if (n->kind == NodeLabel) {
        if (!defined.insert(n->name).second) {
            Fatal(n->line, "duplicate label '%s'", n->name.c_str());
        }
    }
    if (n->kind == NodeGoto) {
        used.push_back(n);
    }
    if (n->kind >= NodeBlock) {
        CheckGotos(n->a, defined, used);
        CheckGotos(n->b, defined, used);
        CheckGotos(n->c, defined, used);
        CheckGotos(n->d, defined, used);
        if (n->kind == NodeBlock) {
            for (Node* s : n->list) {
                CheckGotos(s, defined, used);
            }
        }
    }
}

static void FunctionDefinition(Var* f, Type* t, int line) {
    if (f->isDefined) {
        Fatal(line, "redefinition of function '%s'", f->name.c_str());
    }
    f->isDefined = true;
    f->type = t;
    if (t->base->kind == KindStruct) {
        Fatal(line, "returning structs by value is not supported");
    }
    if (t->variadic && t->params.empty()) {
        Fatal(line, "a variadic function needs at least one named parameter");
    }

    frameSize = 0;
    currentReturnType = t->base;
    EnterScope();
    for (size_t i = 0; i < t->params.size(); i++) {
        if (t->paramNames[i].empty()) {
            Fatal(line, "parameter %d of '%s' has no name", (int)i + 1, f->name.c_str());
        }
        if (t->params[i]->kind == KindStruct) {
            Fatal(line, "passing structs by value is not supported (pass a pointer)");
        }
        Var* p = new Var;
        p->name = t->paramNames[i];
        p->type = t->params[i];
        p->isLocal = true;
        p->offset = 4 + 2 * (int)i;
        Symbol s;
        s.var = p;
        scopes.back()[p->name] = s;
    }
    Node* body = CompoundStatement();
    LeaveScope();

    std::set<string> definedLabels;
    vector<Node*> gotos;
    CheckGotos(body, definedLabels, gotos);
    for (Node* g : gotos) {
        if (!definedLabels.count(g->name)) {
            Fatal(g->line, "goto to undefined label '%s'", g->name.c_str());
        }
    }

    GenerateFunction(f, body, frameSize);
}

void TopLevel() {
    StorageClass storage;
    Type* base = DeclarationSpecifiers(&storage);
    if (Accept(";")) {
        return;
    }
    bool first = true;
    for (;;) {
        int line = Tok().line;
        string name;
        Type* t = ParseDeclarator(base, name);
        if (name.empty()) {
            Fatal(line, "declaration needs a name");
        }
        if (storage.isTypedef) {
            Symbol s;
            s.typedefType = t;
            scopes[0][name] = s;
        }
        else if (t->kind == KindFunction) {
            Var* f = DeclareGlobal(name, t, line);
            if (first && Is("{")) {
                FunctionDefinition(f, t, line);
                return;
            }
        }
        else {
            if (t->kind == KindVoid) {
                Fatal(line, "variable '%s' declared void", name.c_str());
            }
            Var* g = DeclareGlobal(name, t, line);
            if (!storage.isExtern) {
                if (t->kind == KindStruct && !t->info->complete) {
                    Fatal(line, "variable '%s' has incomplete type", name.c_str());
                }
                g->isDefined = true;
            }
            if (Accept("=")) {
                g->isDefined = true;
                GlobalInitializer(g);
            }
        }
        first = false;
        if (Accept(",")) {
            continue;
        }
        Expect(";");
        return;
    }
}

void ResetParser() {
    tokens.clear();
    pos = 0;
    globals.clear();
    globalsByName.clear();
    labelCounter = stringCounter = staticCounter = 0;
    scopes.clear();
    tagScopes.clear();
    switchStack.clear();
    frameSize = 0;
    currentReturnType = nullptr;
}

}  // namespace Kestrel::Compiler
