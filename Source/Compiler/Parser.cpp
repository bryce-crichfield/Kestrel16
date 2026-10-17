// Parser.cpp -- declarations, initializers and statements; each function is handed to the code
// generator as soon as its body is parsed.

#include "Parser.h"
#include "Expressions.h"
#include "Generator.h"

#include <functional>
#include <set>

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

// Past the end, this returns the end-of-file token.
Token& TokAt(size_t index) {
    if (index >= tokens.size()) {
        return tokens.back();
    }
    return tokens[index];
}

bool IsText(const Token& token, const char* text) {
    bool isWord = token.kind == TokPunct || token.kind == TokKeyword;
    return isWord && token.text == text;
}
bool Is(const char* text) {
    return IsText(Tok(), text);
}
bool Accept(const char* text) {
    if (Is(text)) {
        pos++;
        return true;
    }
    return false;
}
void Expect(const char* text) {
    if (!Accept(text)) {
        Fatal(Tok().line, "expected '%s' but found '%s'", text, Tok().text.c_str());
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
        auto found = scopes[i].find(name);
        if (found != scopes[i].end()) {
            return &found->second;
        }
    }
    return nullptr;
}

static Type* FindTag(const string& name) {
    for (int i = (int)tagScopes.size() - 1; i >= 0; i--) {
        auto found = tagScopes[i].find(name);
        if (found != tagScopes[i].end()) {
            return found->second;
        }
    }
    return nullptr;
}

bool IsTypeToken(const Token& token) {
    static const std::set<string> typeWords = {
        "void", "_Bool", "char", "short", "int", "long", "signed", "unsigned", "struct", "union",
        "enum", "const", "volatile", "typedef", "static", "extern", "inline", "register", "auto",
        "restrict", "float", "double"};
    if (token.kind == TokKeyword) {
        return typeWords.count(token.text) > 0;
    }
    if (token.kind == TokIdent) {
        Symbol* symbol = FindSymbol(token.text);
        return symbol && symbol->typedefType;
    }
    return false;
}

// A name followed by a colon starts a labeled statement, not a declaration or expression.
static bool IsLabelAhead() {
    return Tok().kind == TokIdent && IsText(TokAt(pos + 1), ":");
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

static Type* NewStructType(bool isUnion) {
    Type* structType = MakeType(KindStruct, 0, 1);
    structType->info = new StructInfo;
    structType->info->isUnion = isUnion;
    return structType;
}

static Type* StructDeclaration(bool isUnion) {
    int line = Tok().line;
    string tag;
    if (Tok().kind == TokIdent) {
        tag = tokens[pos++].text;
    }
    bool hasTag = !tag.empty();

    // "struct tag" with no body: a use of the tag, or a forward declaration of it
    if (hasTag && !Is("{")) {
        Type* existing = FindTag(tag);
        if (existing) {
            return existing;
        }
        Type* structType = NewStructType(isUnion);
        tagScopes.back()[tag] = structType;
        return structType;
    }

    // A body completes a forward declaration in this scope, or else declares a new type.
    Type* structType = nullptr;
    if (hasTag) {
        auto found = tagScopes.back().find(tag);
        if (found != tagScopes.back().end()) {
            if (found->second->info->complete) {
                Fatal(line, "redefinition of '%s %s'", isUnion ? "union" : "struct", tag.c_str());
            }
            structType = found->second;
        }
    }
    if (!structType) {
        structType = NewStructType(isUnion);
        if (hasTag) {
            tagScopes.back()[tag] = structType;
        }
    }

    Expect("{");
    int offset = 0;
    int size = 0;
    int align = 1;
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
            structType->info->members.push_back({name, memberType, memberOffset});
        } while (Accept(","));
        Expect(";");
    }
    structType->size = AlignUp(size, align);
    structType->align = align;
    structType->info->complete = true;
    return structType;
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
        Symbol constant;
        constant.isEnumConstant = true;
        constant.enumValue = value;
        scopes.back()[name] = constant;
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
    int unsignedCount = 0;
    int charCount = 0;
    int voidCount = 0;
    int boolCount = 0;
    int intCount = 0;       // signed, short, long, int
    Type* named = nullptr;  // a struct, union, enum or typedef name

    // "unsigned" alone may still be followed by char or int, so it doesn't count here.
    auto hasBaseType = [&]() {
        return named || intCount || charCount || voidCount || boolCount;
    };

    for (;;) {
        Token& token = Tok();
        if (token.kind == TokKeyword) {
            const string& word = token.text;
            if (word == "typedef" || word == "static" || word == "extern") {
                if (!storage) {
                    Fatal(token.line, "'%s' is not allowed here", word.c_str());
                }
                if (word == "typedef") {
                    storage->isTypedef = true;
                }
                if (word == "static") {
                    storage->isStatic = true;
                }
                if (word == "extern") {
                    storage->isExtern = true;
                }
                pos++;
                continue;
            }
            bool isIgnoredQualifier = word == "const" || word == "volatile" || word == "inline" ||
                                      word == "register" || word == "auto" || word == "restrict";
            if (isIgnoredQualifier) {
                pos++;
                continue;
            }
            if (word == "float" || word == "double") {
                Fatal(token.line, "floating point is not supported");
            }
            if (word == "struct" || word == "union") {
                if (hasBaseType()) {
                    Fatal(token.line, "invalid type");
                }
                pos++;
                named = StructDeclaration(word == "union");
                continue;
            }
            if (word == "enum") {
                if (hasBaseType()) {
                    Fatal(token.line, "invalid type");
                }
                pos++;
                named = EnumDeclaration();
                continue;
            }
            if (word == "unsigned") {
                unsignedCount++;
            }
            else if (word == "signed" || word == "short" || word == "long" || word == "int") {
                intCount++;
            }
            else if (word == "char") {
                charCount++;
            }
            else if (word == "void") {
                voidCount++;
            }
            else if (word == "_Bool") {
                boolCount++;
            }
            else {
                break;
            }
            pos++;
            continue;
        }
        // A typedef name is only a type when no other type word came before it;
        // otherwise it is the name being declared.
        if (token.kind == TokIdent && !hasBaseType() && !unsignedCount) {
            Symbol* symbol = FindSymbol(token.text);
            if (symbol && symbol->typedefType) {
                named = symbol->typedefType;
                pos++;
                continue;
            }
        }
        break;
    }
    if (named) {
        return named;
    }
    if (voidCount) {
        return TypeVoid;
    }
    if (boolCount) {
        return TypeBool;
    }
    if (charCount) {
        return unsignedCount ? TypeUChar : TypeChar;
    }
    if (unsignedCount) {
        return TypeUInt;
    }
    if (intCount) {
        return TypeInt;
    }
    Fatal(line, "expected a type but found '%s'", Tok().text.c_str());
}

static Type* FunctionParameters(Type* returnType) {
    Type* function = MakeType(KindFunction, 1, 1);
    function->base = returnType;
    if (Is("void") && IsText(TokAt(pos + 1), ")")) {
        pos += 2;
        return function;
    }
    if (Accept(")")) {
        function->oldStyle = true;
        return function;
    }
    for (;;) {
        if (Accept("...")) {
            function->variadic = true;
            Expect(")");
            break;
        }
        Type* base = DeclarationSpecifiers(nullptr);
        string name;
        Type* paramType = ParseDeclarator(base, name);
        // array and function parameters are really pointers
        if (paramType->kind == KindArray) {
            paramType = PointerTo(paramType->base);
        }
        else if (paramType->kind == KindFunction) {
            paramType = PointerTo(paramType);
        }
        function->params.push_back(paramType);
        function->paramNames.push_back(name);
        if (Accept(")")) {
            break;
        }
        Expect(",");
    }
    return function;
}

static Type* TypeSuffix(Type* type) {
    if (Accept("(")) {
        return FunctionParameters(type);
    }
    if (Accept("[")) {
        int line = Tok().line;
        int length = -1;
        if (!Is("]")) {
            long long size = ConstantExpression();
            if (size < 0) {
                Fatal(line, "array size is negative");
            }
            length = (int)size;
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
        // "(" starts a nested declarator like (*name)[4], unless it starts a parameter list
        const Token& next = TokAt(pos + 1);
        bool startsDeclarator = IsText(next, "*") || IsText(next, "(") || IsText(next, "[");
        bool isName = next.kind == TokIdent && !IsTypeToken(next);
        if (startsDeclarator || isName) {
            // The suffix after the parentheses applies first, so skip ahead to read it,
            // then come back and parse the inside around that type.
            pos++;
            size_t start = pos;
            int depth = 1;
            while (depth > 0) {
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
    Type* type = ParseDeclarator(base, name);
    if (!name.empty()) {
        Fatal(Tok().line, "unexpected name '%s' in type", name.c_str());
    }
    return type;
}

// ===========================================================================
// Initializers

// Called for each scalar in an initializer: its type, its byte offset in the object, its value.
using LeafFunction = std::function<void(Type*, int, Node*)>;

// Parses an initializer for an object of the given type at a byte offset. Returns the element
// count for arrays (used to complete "int a[] = {...}").
static int Initializer(Type* type, int offset, const LeafFunction& leaf) {
    int line = Tok().line;
    if (type->kind == KindArray) {
        Type* element = type->base;
        bool isString = Tok().kind == TokString;
        bool isBracedString = Is("{") && TokAt(pos + 1).kind == TokString;
        if (element->kind == KindChar && (isString || isBracedString)) {
            bool braced = Accept("{");
            string text = Tok().text;
            pos++;
            int length = (int)text.size();
            int count = type->length;
            if (count < 0) {
                count = length + 1;  // room for the terminating zero
            }
            if (length > count) {
                Fatal(line, "string is too long for the array");
            }
            for (int i = 0; i < length; i++) {
                leaf(element, offset + i, NewNumber((signed char)text[i], line));
            }
            if (length < count) {
                leaf(element, offset + length, NewNumber(0, line));  // the terminator, if it fits
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
        int index = 0;
        while (!Accept("}")) {
            if (Is(".") || Is("[")) {
                Fatal(Tok().line, "designated initializers are not supported");
            }
            if (type->length >= 0 && index >= type->length) {
                Fatal(Tok().line, "too many initializers for the array");
            }
            Initializer(element, offset + index * element->size, leaf);
            index++;
            if (!Accept(",")) {
                Expect("}");
                break;
            }
        }
        return index;
    }
    if (type->kind == KindStruct) {
        if (!Accept("{")) {
            leaf(type, offset, Assignment());  // a struct copied whole
            return 1;
        }
        auto& members = type->info->members;
        size_t maxCount = members.size();
        if (type->info->isUnion && maxCount > 1) {
            maxCount = 1;  // a union initializes its first member only
        }
        size_t index = 0;
        while (!Accept("}")) {
            if (Is(".") || Is("[")) {
                Fatal(Tok().line, "designated initializers are not supported");
            }
            if (index >= maxCount) {
                Fatal(Tok().line, "too many initializers for the struct");
            }
            Initializer(members[index].type, offset + members[index].offset, leaf);
            index++;
            if (!Accept(",")) {
                Expect("}");
                break;
            }
        }
        return 1;
    }
    bool braced = Accept("{");
    Node* expr = Assignment();
    if (braced) {
        Accept(",");
        Expect("}");
    }
    leaf(type, offset, expr);
    return 1;
}

static void GlobalInitializer(Var* global) {
    if (global->hasInit) {
        Fatal(Tok().line, "redefinition of '%s'", global->name.c_str());
    }
    global->hasInit = true;
    global->data.clear();
    global->relocations.clear();
    auto leaf = [&](Type* type, int offset, Node* expr) {
        if (type->kind == KindStruct) {
            Fatal(expr->line, "struct copy is not a constant initializer");
        }
        if (!IsScalar(Decay(expr->type))) {
            Fatal(expr->line, "invalid initializer");
        }
        string label;  // set when the value is an address: label + value
        long long value = Evaluate(MakeCast(expr, type), &label);
        bool isAddress = !label.empty();
        if ((int)global->data.size() < offset + type->size) {
            global->data.resize(offset + type->size, 0);
        }
        if (type->size == 1) {
            if (isAddress) {
                Fatal(expr->line, "an address does not fit in a char");
            }
            global->data[offset] = (uint8_t)value;
        }
        else if (isAddress) {
            global->relocations[offset] = {label, (int)value};
        }
        else {
            global->data[offset] = (uint8_t)(value & 0xFF);
            global->data[offset + 1] = (uint8_t)((value >> 8) & 0xFF);
        }
    };
    int count = Initializer(global->type, 0, leaf);
    if (global->type->kind == KindArray && global->type->length < 0) {
        global->type = ArrayOf(global->type->base, count);
    }
    global->data.resize(global->type->size, 0);
}

// ===========================================================================
// Globals, locals, statements

static void AddSymbol(std::map<string, Symbol>& scope, const string& name, Var* var) {
    Symbol symbol;
    symbol.var = var;
    scope[name] = symbol;
}

static void AddTypedef(std::map<string, Symbol>& scope, const string& name, Type* type) {
    Symbol symbol;
    symbol.typedefType = type;
    scope[name] = symbol;
}

static Var* DeclareGlobal(const string& name, Type* type, int line) {
    auto found = globalsByName.find(name);
    if (found != globalsByName.end()) {
        Var* global = found->second;
        bool wasFunction = global->type->kind == KindFunction;
        bool isFunction = type->kind == KindFunction;
        if (wasFunction != isFunction) {
            Fatal(line, "'%s' redeclared as a different kind of symbol", name.c_str());
        }
        // a redeclaration can fill in an array's size or a function's parameters
        bool completesArray = global->type->kind == KindArray && global->type->length < 0 && type->kind == KindArray;
        bool addsPrototype = wasFunction && global->type->oldStyle && !type->oldStyle;
        if (completesArray || addsPrototype) {
            global->type = type;
        }
        scopes[0][name].var = global;
        return global;
    }
    Var* global = new Var;
    global->name = name;
    global->label = "_" + name;
    global->type = type;
    globals.push_back(global);
    globalsByName[name] = global;
    AddSymbol(scopes[0], name, global);
    return global;
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
        Type* type = ParseDeclarator(base, name);
        if (name.empty()) {
            Fatal(line, "declaration needs a name");
        }
        if (storage.isTypedef) {
            AddTypedef(scopes.back(), name, type);
            continue;
        }
        if (type->kind == KindFunction || storage.isExtern) {
            Var* global = DeclareGlobal(name, type, line);
            AddSymbol(scopes.back(), name, global);
            continue;
        }
        if (type->kind == KindVoid) {
            Fatal(line, "variable '%s' declared void", name.c_str());
        }
        if (type->kind == KindStruct && !type->info->complete) {
            Fatal(line, "variable '%s' has incomplete type", name.c_str());
        }

        if (storage.isStatic) {
            Var* global = new Var;
            global->name = name;
            global->label = ".Lstatic" + std::to_string(++staticCounter) + "_" + name;
            global->type = type;
            global->isDefined = true;
            globals.push_back(global);
            AddSymbol(scopes.back(), name, global);
            if (Accept("=")) {
                GlobalInitializer(global);
            }
            if (global->type->kind == KindArray && global->type->length < 0) {
                Fatal(line, "array '%s' needs a size", name.c_str());
            }
            continue;
        }

        Var* local = new Var;
        local->name = name;
        local->type = type;
        local->isLocal = true;
        AddSymbol(scopes.back(), name, local);

        // The initializer becomes one assignment statement per scalar.
        vector<Node*> stores;
        bool hasInit = Accept("=");
        if (hasInit) {
            auto leaf = [&](Type* leafType, int offset, Node* expr) {
                Node* target = NewNode(NodeVar, expr->line);
                target->var = local;
                target->value = offset;
                target->type = leafType;
                Node* store = NewNode(NodeExprStmt, expr->line);
                store->expression = MakeAssign(target, expr, expr->line);
                stores.push_back(store);
            };
            int count = Initializer(type, 0, leaf);
            if (type->kind == KindArray && type->length < 0) {
                type = ArrayOf(type->base, count);
                local->type = type;
            }
        }
        if (type->kind == KindArray && type->length < 0) {
            Fatal(line, "array '%s' needs a size", name.c_str());
        }
        frameSize += type->size;
        if (type->align >= 2) {
            frameSize = AlignUp(frameSize, 2);
        }
        local->offset = -frameSize;

        // An initialized array or struct is zeroed first so the parts the initializer leaves out
        // are zero -- unless it is copied whole from another struct.
        bool isAggregate = type->kind == KindArray || type->kind == KindStruct;
        bool isWholeCopy = stores.size() == 1 && stores[0]->expression->left->type == type;
        if (hasInit && isAggregate && !isWholeCopy) {
            Node* zero = NewNode(NodeZero, line);
            zero->var = local;
            block->list.push_back(zero);
        }
        for (Node* store : stores) {
            block->list.push_back(store);
        }
    } while (Accept(","));
    Expect(";");
    return block;
}

// The statement after "case x:", "default:" or a label; empty if the block closes right there.
static Node* LabeledStatement(int line) {
    if (Is("}")) {
        return NewNode(NodeBlock, line);
    }
    return Statement();
}

static Node* Statement() {
    Token& token = Tok();
    int line = token.line;

    if (Is("{")) {
        return CompoundStatement();
    }

    if (Accept("if")) {
        Node* node = NewNode(NodeIf, line);
        Expect("(");
        node->condition = Expression();
        NeedScalar(node->condition, "if");
        Expect(")");
        node->then = Statement();
        if (Accept("else")) {
            node->otherwise = Statement();
        }
        return node;
    }
    if (Accept("while")) {
        Node* node = NewNode(NodeWhile, line);
        Expect("(");
        node->condition = Expression();
        NeedScalar(node->condition, "while");
        Expect(")");
        node->body = Statement();
        return node;
    }
    if (Accept("do")) {
        Node* node = NewNode(NodeDo, line);
        node->body = Statement();
        Expect("while");
        Expect("(");
        node->condition = Expression();
        NeedScalar(node->condition, "do-while");
        Expect(")");
        Expect(";");
        return node;
    }
    if (Accept("for")) {
        Node* node = NewNode(NodeFor, line);
        Expect("(");
        EnterScope();
        if (IsTypeToken(Tok())) {
            node->init = LocalDeclaration();
        }
        else if (!Accept(";")) {
            node->init = NewNode(NodeExprStmt, line);
            node->init->expression = Expression();
            Expect(";");
        }
        if (!Is(";")) {
            node->condition = Expression();
            NeedScalar(node->condition, "for");
        }
        Expect(";");
        if (!Is(")")) {
            node->step = Expression();
        }
        Expect(")");
        node->body = Statement();
        LeaveScope();
        return node;
    }
    if (Accept("return")) {
        Node* node = NewNode(NodeReturn, line);
        if (Accept(";")) {
            return node;
        }
        Node* value = Expression();
        Expect(";");
        if (currentReturnType->kind == KindVoid) {
            node->expression = value;  // allowed if the value is void; it is ignored
            return node;
        }
        if (value->type->kind == KindStruct || currentReturnType->kind == KindStruct) {
            Fatal(line, "returning structs by value is not supported");
        }
        NeedScalar(value, "return");
        node->expression = MakeCast(value, currentReturnType);
        return node;
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
        Node* node = NewNode(NodeGoto, line);
        node->name = ExpectIdent();
        Expect(";");
        return node;
    }
    if (Accept("switch")) {
        Node* node = NewNode(NodeSwitch, line);
        Expect("(");
        node->condition = Expression();
        NeedInteger(node->condition, "switch");
        Expect(")");
        switchStack.push_back(node);
        node->body = Statement();
        switchStack.pop_back();
        return node;
    }
    if (Accept("case")) {
        if (switchStack.empty()) {
            Fatal(line, "'case' outside of a switch");
        }
        Node* switchNode = switchStack.back();
        Node* node = NewNode(NodeCase, line);
        node->value = ConstantExpression();
        Expect(":");
        node->label = ++labelCounter;
        for (Node* other : switchNode->list) {
            if (Wrap16(other->value, false) == Wrap16(node->value, false)) {
                Fatal(line, "duplicate case value %lld", node->value);
            }
        }
        switchNode->list.push_back(node);
        node->body = LabeledStatement(line);
        return node;
    }
    if (Accept("default")) {
        if (switchStack.empty()) {
            Fatal(line, "'default' outside of a switch");
        }
        Node* switchNode = switchStack.back();
        if (switchNode->defaultCase) {
            Fatal(line, "multiple default labels");
        }
        Expect(":");
        Node* node = NewNode(NodeCase, line);
        node->label = ++labelCounter;
        switchNode->defaultCase = node;
        node->body = LabeledStatement(line);
        return node;
    }
    if (IsLabelAhead()) {
        Node* node = NewNode(NodeLabel, line);
        node->name = token.text;
        pos += 2;
        node->body = LabeledStatement(line);
        return node;
    }
    if (Accept(";")) {
        return NewNode(NodeBlock, line);
    }

    Node* node = NewNode(NodeExprStmt, line);
    node->expression = Expression();
    Expect(";");
    return node;
}

static Node* CompoundStatement() {
    Node* block = NewNode(NodeBlock, Tok().line);
    Expect("{");
    EnterScope();
    while (!Accept("}")) {
        if (Tok().kind == TokEof) {
            Fatal(Tok().line, "missing '}'");
        }
        // "name:" is a label even when name is also a typedef
        if (IsTypeToken(Tok()) && !IsLabelAhead()) {
            block->list.push_back(LocalDeclaration());
        }
        else {
            block->list.push_back(Statement());
        }
    }
    LeaveScope();
    return block;
}

// Collects the labels a function defines and the gotos it uses. Only statements can hold
// labels, so expressions are not searched.
static void CheckGotos(Node* node, std::set<string>& defined, vector<Node*>& used) {
    if (!node) {
        return;
    }
    switch (node->kind) {
        case NodeLabel:
            if (!defined.insert(node->name).second) {
                Fatal(node->line, "duplicate label '%s'", node->name.c_str());
            }
            CheckGotos(node->body, defined, used);
            return;
        case NodeGoto:
            used.push_back(node);
            return;
        case NodeBlock:
            for (Node* statement : node->list) {
                CheckGotos(statement, defined, used);
            }
            return;
        case NodeIf:
            CheckGotos(node->then, defined, used);
            CheckGotos(node->otherwise, defined, used);
            return;
        case NodeFor:
            CheckGotos(node->init, defined, used);
            CheckGotos(node->body, defined, used);
            return;
        case NodeWhile:
        case NodeDo:
        case NodeSwitch:
        case NodeCase:
            CheckGotos(node->body, defined, used);
            return;
        default:
            return;
    }
}

static void FunctionDefinition(Var* function, Type* type, int line) {
    if (function->isDefined) {
        Fatal(line, "redefinition of function '%s'", function->name.c_str());
    }
    function->isDefined = true;
    function->type = type;
    if (type->base->kind == KindStruct) {
        Fatal(line, "returning structs by value is not supported");
    }
    if (type->variadic && type->params.empty()) {
        Fatal(line, "a variadic function needs at least one named parameter");
    }

    frameSize = 0;
    currentReturnType = type->base;
    EnterScope();
    for (size_t i = 0; i < type->params.size(); i++) {
        if (type->paramNames[i].empty()) {
            Fatal(line, "parameter %d of '%s' has no name", (int)i + 1, function->name.c_str());
        }
        if (type->params[i]->kind == KindStruct) {
            Fatal(line, "passing structs by value is not supported (pass a pointer)");
        }
        Var* param = new Var;
        param->name = type->paramNames[i];
        param->type = type->params[i];
        param->isLocal = true;
        param->offset = 4 + 2 * (int)i;  // above the saved frame pointer and return address
        AddSymbol(scopes.back(), param->name, param);
    }
    Node* body = CompoundStatement();
    LeaveScope();

    std::set<string> definedLabels;
    vector<Node*> gotos;
    CheckGotos(body, definedLabels, gotos);
    for (Node* jump : gotos) {
        if (!definedLabels.count(jump->name)) {
            Fatal(jump->line, "goto to undefined label '%s'", jump->name.c_str());
        }
    }

    GenerateFunction(function, body, frameSize);
}

void TopLevel() {
    StorageClass storage;
    Type* base = DeclarationSpecifiers(&storage);
    if (Accept(";")) {
        return;
    }
    bool isFirst = true;  // only the first declarator may have a function body
    for (;;) {
        int line = Tok().line;
        string name;
        Type* type = ParseDeclarator(base, name);
        if (name.empty()) {
            Fatal(line, "declaration needs a name");
        }
        if (storage.isTypedef) {
            AddTypedef(scopes[0], name, type);
        }
        else if (type->kind == KindFunction) {
            Var* function = DeclareGlobal(name, type, line);
            if (isFirst && Is("{")) {
                FunctionDefinition(function, type, line);
                return;
            }
        }
        else {
            if (type->kind == KindVoid) {
                Fatal(line, "variable '%s' declared void", name.c_str());
            }
            Var* global = DeclareGlobal(name, type, line);
            if (!storage.isExtern) {
                if (type->kind == KindStruct && !type->info->complete) {
                    Fatal(line, "variable '%s' has incomplete type", name.c_str());
                }
                global->isDefined = true;
            }
            if (Accept("=")) {
                global->isDefined = true;
                GlobalInitializer(global);
            }
        }
        isFirst = false;
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
    labelCounter = 0;
    stringCounter = 0;
    staticCounter = 0;
    scopes.clear();
    tagScopes.clear();
    switchStack.clear();
    frameSize = 0;
    currentReturnType = nullptr;
}

}  // namespace Kestrel::Compiler
