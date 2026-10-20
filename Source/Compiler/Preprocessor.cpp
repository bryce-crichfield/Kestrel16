// Preprocessor.cpp -- #include, #define, #if and friends: C source text to C source text.

#include "Preprocessor.h"
#include "Kestrel/Files.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <set>
#include <utility>

namespace Kestrel::Compiler {

// A text-to-text pass that runs before Lex(). It handles #include "file" and
// <file> (searched in the including file's folder, then the -I folders; <>
// skips the including file's folder), object-like and function-like #define,
// #undef, #ifdef / #ifndef / #if / #elif / #else / #endif (with defined()),
// #error, #pragma once, and backslash line continuation. Not supported: #
// stringizing, ## pasting, variadic macros, #line, and macro calls whose
// arguments span lines.
//
// The output carries line markers, '# <line> "<file>"', so Lex() can give
// every token its real file and line.

vector<string> fileNames;
vector<string> includePaths;
std::map<string, Macro> macros;
static std::set<string> onceFiles;

static int FileIndex(const string& name) {
    for (size_t i = 0; i < fileNames.size(); i++) {
        if (fileNames[i] == name) {
            return (int)i;
        }
    }
    fileNames.push_back(name);
    return (int)fileNames.size() - 1;
}

int Location(const string& file, int line) {
    return FileIndex(file) * LocationLines + line;
}

static bool IsIdentStart(char character) {
    return isalpha((unsigned char)character) || character == '_';
}
static bool IsIdentChar(char character) {
    return isalnum((unsigned char)character) || character == '_';
}

// Splits a line into pp-tokens: identifiers, numbers, string and character literals, runs of
// whitespace, and single other characters. Concatenating them gives back the line.
vector<string> PPTokens(const string& text) {
    vector<string> tokens;
    size_t index = 0;
    while (index < text.size()) {
        size_t start = index;
        char first = text[index];
        if (IsIdentStart(first)) {
            while (index < text.size() && IsIdentChar(text[index])) {
                index++;
            }
        }
        else if (isdigit((unsigned char)first)) {
            while (index < text.size() && (IsIdentChar(text[index]) || text[index] == '.')) {
                index++;
            }
        }
        else if (first == '"' || first == '\'') {
            char quote = first;
            index++;
            while (index < text.size() && text[index] != quote) {
                if (text[index] == '\\') {
                    index += 2;  // skip the escaped character, which may be a quote
                }
                else {
                    index++;
                }
            }
            index = std::min(index + 1, text.size());  // past the closing quote, if there is one
        }
        else if (isspace((unsigned char)first)) {
            while (index < text.size() && isspace((unsigned char)text[index])) {
                index++;
            }
        }
        else {
            index++;
        }
        tokens.push_back(text.substr(start, index - start));
    }
    return tokens;
}

static bool IsSpace(const string& token) {
    return !token.empty() && isspace((unsigned char)token[0]);
}

static size_t SkipSpaces(const vector<string>& tokens, size_t index) {
    while (index < tokens.size() && IsSpace(tokens[index])) {
        index++;
    }
    return index;
}

static vector<string> WithoutSpaces(const vector<string>& tokens) {
    vector<string> words;
    for (const string& token : tokens) {
        if (!IsSpace(token)) {
            words.push_back(token);
        }
    }
    return words;
}

static void Append(vector<string>& to, const vector<string>& tokens) {
    to.insert(to.end(), tokens.begin(), tokens.end());
}

// ---- Macro expansion

static vector<string> Expand(const vector<string>& tokens, std::set<string> disabled, int loc);

// Splits the arguments of a function-like macro call at the commas that aren't inside nested
// parentheses. next starts on the '(' after the macro's name and is left on the closing ')'.
static vector<vector<string>> MacroArguments(const vector<string>& tokens, size_t& next, const string& name, int loc) {
    vector<vector<string>> args(1);
    int depth = 0;
    for (next++;; next++) {
        if (next >= tokens.size()) {
            Fatal(loc, "unterminated call of macro '%s' (arguments must be on one line)", name.c_str());
        }
        const string& token = tokens[next];
        if (token == ")" && depth == 0) {
            return args;
        }
        if (token == "," && depth == 0) {
            args.emplace_back();
            continue;
        }
        if (token == "(") {
            depth++;
        }
        if (token == ")") {
            depth--;
        }
        args.back().push_back(token);
    }
}

// "f()" reads as one empty argument; for a macro with no parameters it is no arguments.
static void CheckArgumentCount(vector<vector<string>>& args, const Macro& macro, const string& name, int loc) {
    if (args.size() == 1 && macro.params.empty()) {
        if (!WithoutSpaces(args[0]).empty()) {
            Fatal(loc, "macro '%s' takes no arguments", name.c_str());
        }
        args.clear();
    }
    if (args.size() != macro.params.size()) {
        Fatal(loc, "macro '%s' takes %d arguments, not %d", name.c_str(), (int)macro.params.size(), (int)args.size());
    }
}

// The macro's body with each parameter replaced by its argument, expanded on its own first.
static vector<string> Substitute(const Macro& macro, const vector<vector<string>>& args, const std::set<string>& disabled, int loc) {
    vector<string> body;
    for (const string& token : macro.body) {
        auto param = std::find(macro.params.begin(), macro.params.end(), token);
        if (param == macro.params.end()) {
            body.push_back(token);
            continue;
        }
        size_t paramIndex = param - macro.params.begin();
        Append(body, Expand(args[paramIndex], disabled, loc));
    }
    return body;
}

// Expands the macros in a list of pp-tokens. A macro is not expanded again inside its own
// expansion, so '#define x x + 1' terminates.
static vector<string> Expand(const vector<string>& tokens, std::set<string> disabled, int loc) {
    vector<string> out;
    for (size_t i = 0; i < tokens.size(); i++) {
        const string& name = tokens[i];
        bool canBeMacro = IsIdentStart(name[0]) && !disabled.count(name);
        auto found = canBeMacro ? macros.find(name) : macros.end();
        if (found == macros.end()) {
            out.push_back(name);
            continue;
        }
        const Macro& macro = found->second;
        std::set<string> insideMacro = disabled;
        insideMacro.insert(name);
        if (!macro.function) {
            Append(out, Expand(macro.body, insideMacro, loc));
            continue;
        }

        // A function-like macro is only a call when '(' follows its name.
        size_t next = SkipSpaces(tokens, i + 1);
        if (next >= tokens.size() || tokens[next] != "(") {
            out.push_back(name);
            continue;
        }
        vector<vector<string>> args = MacroArguments(tokens, next, name, loc);
        i = next;
        CheckArgumentCount(args, macro, name, loc);
        Append(out, Expand(Substitute(macro, args, disabled, loc), insideMacro, loc));
    }
    return out;
}

// ---- #if expressions

// Evaluates the expression of an #if (after defined() and macro expansion), on non-space tokens.
struct IfExpression {
    vector<string> tokens;
    size_t pos = 0;
    int loc;

    bool Is(const char* text) {
        return pos < tokens.size() && tokens[pos] == text;
    }
    bool Accept(const char* text) {
        if (!Is(text)) {
            return false;
        }
        pos++;
        return true;
    }

    long long Primary() {
        if (pos >= tokens.size()) {
            Fatal(loc, "#if expression ends early");
        }
        string token = tokens[pos++];
        if (token == "(") {
            long long value = Conditional();
            if (!Accept(")")) {
                Fatal(loc, "missing ')' in #if");
            }
            return value;
        }
        if (token == "!") {
            return !Primary();
        }
        if (token == "~") {
            return ~Primary();
        }
        if (token == "-") {
            return -Primary();
        }
        if (token == "+") {
            return Primary();
        }
        if (isdigit((unsigned char)token[0])) {
            return strtoll(token.c_str(), nullptr, 0);
        }
        if (token[0] == '\'') {
            if (token.size() > 2) {
                return (unsigned char)token[1];  // 'x'
            }
            return 0;
        }
        if (IsIdentStart(token[0])) {
            return 0;  // an identifier that isn't a macro is 0
        }
        Fatal(loc, "unexpected '%s' in #if", token.c_str());
    }

    // Pp-tokens are single characters, so a two-character operator like << arrives as two
    // tokens. This checks whether the operator starts at the current token.
    bool MatchesOperator(const string& op) {
        bool hasNext = pos + 1 < tokens.size();
        if (op.size() == 2) {
            return hasNext && tokens[pos] == op.substr(0, 1) && tokens[pos + 1] == op.substr(1);
        }
        if (!Is(op.c_str())) {
            return false;
        }
        // A single < > | & is not the operator when it is the first half of << >> || && <= >=.
        bool canStartLonger = string("<>|&").find(op) != string::npos;
        if (canStartLonger && hasNext) {
            const string& following = tokens[pos + 1];
            if (following == op || following == "=") {
                return false;
            }
        }
        return true;
    }

    static long long Apply(const string& op, long long left, long long right) {
        using Operation = long long (*)(long long, long long);
        static const std::map<string, Operation> operations = {
            {"||", [](long long l, long long r) -> long long { return l || r; }},
            {"&&", [](long long l, long long r) -> long long { return l && r; }},
            {"|", [](long long l, long long r) { return l | r; }},
            {"^", [](long long l, long long r) { return l ^ r; }},
            {"&", [](long long l, long long r) { return l & r; }},
            {"==", [](long long l, long long r) -> long long { return l == r; }},
            {"!=", [](long long l, long long r) -> long long { return l != r; }},
            {"<", [](long long l, long long r) -> long long { return l < r; }},
            {">", [](long long l, long long r) -> long long { return l > r; }},
            {"<=", [](long long l, long long r) -> long long { return l <= r; }},
            {">=", [](long long l, long long r) -> long long { return l >= r; }},
            {"<<", [](long long l, long long r) { return l << r; }},
            {">>", [](long long l, long long r) { return l >> r; }},
            {"+", [](long long l, long long r) { return l + r; }},
            {"-", [](long long l, long long r) { return l - r; }},
            {"*", [](long long l, long long r) { return l * r; }},
            {"/", [](long long l, long long r) { return l / r; }},
            {"%", [](long long l, long long r) { return l % r; }},
        };
        return operations.at(op)(left, right);
    }

    // Binary operators by precedence, loosest first.
    long long Binary(int level) {
        static const vector<vector<string>> levels = {
            {"||"},
            {"&&"},
            {"|"},
            {"^"},
            {"&"},
            {"==", "!="},
            {"<", ">", "<=", ">="},
            {"<<", ">>"},
            {"+", "-"},
            {"*", "/", "%"},
        };
        if (level == (int)levels.size()) {
            return Primary();
        }
        long long left = Binary(level + 1);
        for (;;) {
            string op;
            for (const string& candidate : levels[level]) {
                if (MatchesOperator(candidate)) {
                    op = candidate;
                    break;
                }
            }
            if (op.empty()) {
                return left;
            }
            pos += op.size();
            long long right = Binary(level + 1);
            if ((op == "/" || op == "%") && right == 0) {
                Fatal(loc, "division by zero in #if");
            }
            left = Apply(op, left, right);
        }
    }

    long long Conditional() {
        long long condition = Binary(0);
        if (!Accept("?")) {
            return condition;
        }
        long long then = Conditional();
        if (!Accept(":")) {
            Fatal(loc, "missing ':' in #if");
        }
        long long otherwise = Conditional();
        if (condition) {
            return then;
        }
        return otherwise;
    }
};

// Replaces defined X and defined(X) with 1 or 0, which has to happen before macro expansion.
static vector<string> ReplaceDefined(const vector<string>& line, int loc) {
    vector<string> replaced;
    for (size_t i = 0; i < line.size(); i++) {
        if (line[i] != "defined") {
            replaced.push_back(line[i]);
            continue;
        }
        size_t next = SkipSpaces(line, i + 1);
        bool hasParen = next < line.size() && line[next] == "(";
        if (hasParen) {
            next = SkipSpaces(line, next + 1);
        }
        if (next >= line.size() || !IsIdentStart(line[next][0])) {
            Fatal(loc, "defined needs a macro name");
        }
        bool isDefined = macros.count(line[next]) > 0;
        replaced.push_back(isDefined ? "1" : "0");
        if (hasParen) {
            next = SkipSpaces(line, next + 1);
            if (next >= line.size() || line[next] != ")") {
                Fatal(loc, "missing ')' after defined");
            }
        }
        i = next;
    }
    return replaced;
}

static long long EvaluateIf(const string& text, int loc) {
    IfExpression expression;
    expression.tokens = WithoutSpaces(Expand(ReplaceDefined(PPTokens(text), loc), {}, loc));
    expression.loc = loc;
    long long value = expression.Conditional();
    if (expression.pos != expression.tokens.size()) {
        Fatal(loc, "unexpected '%s' in #if", expression.tokens[expression.pos].c_str());
    }
    return value;
}

// ---- Comments

static bool StartsWith(const string& text, size_t index, const char* two) {
    return index + 1 < text.size() && text[index] == two[0] && text[index + 1] == two[1];
}

// Copies a string or character literal starting at index, stopping at the end of the line if it
// isn't closed.
static void CopyLiteral(const string& text, size_t& index, string& out) {
    char quote = text[index];
    out += text[index++];
    while (index < text.size() && text[index] != quote && text[index] != '\n') {
        if (text[index] == '\\' && index + 1 < text.size()) {
            out += text[index++];  // the backslash; the escaped character follows
        }
        out += text[index++];
    }
    if (index < text.size() && text[index] == quote) {
        out += text[index++];
    }
}

// Replaces the /* */ comment starting at index with a space, keeping its newlines.
static void SkipBlockComment(const string& text, size_t& index, int& line, string& out, const string& file) {
    int startLine = line;
    index += 2;
    while (index + 1 < text.size() && !StartsWith(text, index, "*/")) {
        if (text[index] == '\n') {
            out += '\n';
            line++;
        }
        index++;
    }
    if (index + 1 >= text.size()) {
        Fatal(Location(file, startLine), "unterminated comment");
    }
    index += 2;
    out += ' ';
}

// Replaces comments with spaces (keeping their newlines), leaving string and character literals alone.
static string StripComments(const string& text, const string& file) {
    string out;
    int line = 1;
    size_t index = 0;
    while (index < text.size()) {
        char character = text[index];
        if (character == '\n') {
            line++;
        }
        if (character == '"' || character == '\'') {
            CopyLiteral(text, index, out);
        }
        else if (StartsWith(text, index, "//")) {
            while (index < text.size() && text[index] != '\n') {
                index++;
            }
        }
        else if (StartsWith(text, index, "/*")) {
            SkipBlockComment(text, index, line, out, file);
        }
        else {
            out += text[index++];
        }
    }
    return out;
}

// ---- Directives

static string Folder(const string& path) {
    size_t slash = path.find_last_of("/\\");
    if (slash == string::npos) {
        return "";
    }
    return path.substr(0, slash + 1);
}

static string WithTrailingSlash(const string& folder) {
    if (folder.empty() || folder.back() == '/' || folder.back() == '\\') {
        return folder;
    }
    return folder + "/";
}

static void Include(const string& rest, const string& file, int loc, string& out, int depth) {
    size_t open = rest.find_first_of("\"<");
    if (open == string::npos) {
        Fatal(loc, "#include expects \"file\" or <file>");
    }
    bool isQuoted = rest[open] == '"';
    size_t close = rest.find(isQuoted ? '"' : '>', open + 1);
    if (close == string::npos) {
        Fatal(loc, "#include expects \"file\" or <file>");
    }
    string name = rest.substr(open + 1, close - open - 1);

    vector<string> folders;
    if (isQuoted) {
        folders.push_back(Folder(file));
    }
    for (const string& includePath : includePaths) {
        folders.push_back(WithTrailingSlash(includePath));
    }
    for (const string& folder : folders) {
        string path = folder + name;
        string text;
        if (!ReadFile(path, text)) {
            continue;
        }
        if (depth >= 64) {
            Fatal(loc, "#include nested too deeply");
        }
        if (!onceFiles.count(path)) {
            Preprocess(path, text, out, depth + 1);
        }
        return;
    }
    Fatal(loc, "cannot find include file '%s'", name.c_str());
}

// Reads a function-like macro's parameter list. index starts on the '(' and is left just past the ')'.
static vector<string> MacroParameters(const vector<string>& tokens, size_t& index, int loc) {
    vector<string> params;
    for (index++;; index++) {
        if (index >= tokens.size()) {
            Fatal(loc, "missing ')' in macro parameters");
        }
        const string& token = tokens[index];
        if (IsSpace(token) || token == ",") {
            continue;
        }
        if (token == ")") {
            index++;
            return params;
        }
        if (token == ".") {
            Fatal(loc, "variadic macros are not supported");
        }
        if (!IsIdentStart(token[0])) {
            Fatal(loc, "bad macro parameter '%s'", token.c_str());
        }
        params.push_back(token);
    }
}

static void DefineMacro(const string& rest, int loc) {
    vector<string> tokens = PPTokens(rest);
    size_t index = SkipSpaces(tokens, 0);
    if (index >= tokens.size() || !IsIdentStart(tokens[index][0])) {
        Fatal(loc, "#define needs a macro name");
    }
    string name = tokens[index++];
    Macro macro;
    bool isFunctionLike = index < tokens.size() && tokens[index] == "(";  // '(' right after the name
    if (isFunctionLike) {
        macro.function = true;
        macro.params = MacroParameters(tokens, index, loc);
    }
    index = SkipSpaces(tokens, index);
    macro.body.assign(tokens.begin() + index, tokens.end());
    while (!macro.body.empty() && IsSpace(macro.body.back())) {
        macro.body.pop_back();
    }
    for (const string& token : macro.body) {
        if (token == "#") {
            Fatal(loc, "# and ## in macros are not supported");
        }
    }
    macros[name] = macro;
}

// One #if / #ifdef / #ifndef ... #endif block being read.
struct ConditionalBlock {
    bool outerActive;  // whether the enclosing code is being kept
    bool active;       // whether the current branch is being kept
    bool taken;        // whether some branch has already been kept
    bool seenElse;
    int loc;
};

// A file being preprocessed.
struct SourceFile {
    const string& name;
    string text;
    size_t index = 0;
    int line = 1;
    int outLine = -1;  // the line the next output line will be taken as
    vector<ConditionalBlock> conditionals;

    SourceFile(const string& name, string text) : name(name), text(std::move(text)) {}

    bool Active() const {
        return conditionals.empty() || conditionals.back().active;
    }
};

// A directive: its name and the text after the name, and that text's non-space tokens.
struct Directive {
    string name;
    string rest;
    vector<string> words;
    int loc;
};

static Directive ParseDirective(const string& afterHash, int loc) {
    Directive directive;
    directive.loc = loc;
    size_t nameStart = afterHash.find_first_not_of(" \t");
    if (nameStart != string::npos) {
        size_t nameEnd = nameStart;
        while (nameEnd < afterHash.size() && IsIdentChar(afterHash[nameEnd])) {
            nameEnd++;
        }
        directive.name = afterHash.substr(nameStart, nameEnd - nameStart);
        directive.rest = afterHash.substr(nameEnd);
    }
    directive.words = WithoutSpaces(PPTokens(directive.rest));
    return directive;
}

// #elif and #else: only one branch is kept, the first whose condition holds, inside kept code.
static void ElseDirective(SourceFile& source, const Directive& directive) {
    const char* name = directive.name.c_str();
    if (source.conditionals.empty()) {
        Fatal(directive.loc, "#%s without #if", name);
    }
    ConditionalBlock& block = source.conditionals.back();
    if (block.seenElse) {
        Fatal(directive.loc, "#%s after #else", name);
    }
    bool isElse = directive.name == "else";
    block.seenElse = isElse;
    if (!block.outerActive || block.taken) {
        block.active = false;
    }
    else if (isElse) {
        block.active = true;
    }
    else {
        block.active = EvaluateIf(directive.rest, directive.loc) != 0;
    }
    if (block.active) {
        block.taken = true;
    }
}

// Handles #ifdef, #ifndef, #if, #elif, #else and #endif, and returns false for any other directive.
static bool ConditionalDirective(SourceFile& source, const Directive& directive) {
    const string& name = directive.name;
    int loc = directive.loc;
    bool active = source.Active();
    if (name == "ifdef" || name == "ifndef") {
        if (active && (directive.words.empty() || !IsIdentStart(directive.words[0][0]))) {
            Fatal(loc, "#%s needs a macro name", name.c_str());
        }
        bool keep = false;
        if (active) {
            bool isDefined = macros.count(directive.words[0]) != 0;
            keep = name == "ifdef" ? isDefined : !isDefined;
        }
        source.conditionals.push_back({active, keep, keep, false, loc});
    }
    else if (name == "if") {
        bool keep = active && EvaluateIf(directive.rest, loc) != 0;
        source.conditionals.push_back({active, keep, keep, false, loc});
    }
    else if (name == "elif" || name == "else") {
        ElseDirective(source, directive);
    }
    else if (name == "endif") {
        if (source.conditionals.empty()) {
            Fatal(loc, "#endif without #if");
        }
        source.conditionals.pop_back();
    }
    else {
        return false;
    }
    return true;
}

// Handles the directives that aren't conditionals, in code that is being kept.
static void OtherDirective(SourceFile& source, const Directive& directive, string& out, int depth) {
    const string& name = directive.name;
    int loc = directive.loc;
    if (name == "include") {
        Include(directive.rest, source.name, loc, out, depth);
    }
    else if (name == "define") {
        DefineMacro(directive.rest, loc);
    }
    else if (name == "undef") {
        if (directive.words.empty()) {
            Fatal(loc, "#undef needs a macro name");
        }
        macros.erase(directive.words[0]);
    }
    else if (name == "error") {
        size_t messageStart = directive.rest.find_first_not_of(" \t");
        string message;
        if (messageStart != string::npos) {
            message = directive.rest.substr(messageStart);
        }
        Fatal(loc, "#error %s", message.c_str());
    }
    else if (name == "pragma") {
        if (!directive.words.empty() && directive.words[0] == "once") {
            onceFiles.insert(source.name);
        }
    }
    else if (!name.empty()) {
        Fatal(loc, "unknown directive '#%s'", name.c_str());
    }
}

// ---- Lines

// Reads one logical line, joining backslash-newline continuations.
static string ReadLogicalLine(SourceFile& source) {
    const string& text = source.text;
    string logical;
    for (;;) {
        size_t newline = text.find('\n', source.index);
        string physical;
        if (newline == string::npos) {
            physical = text.substr(source.index);
            source.index = text.size();
        }
        else {
            physical = text.substr(source.index, newline - source.index);
            source.index = newline + 1;
        }
        source.line++;
        if (!physical.empty() && physical.back() == '\r') {
            physical.pop_back();
        }
        bool continues = !physical.empty() && physical.back() == '\\' && source.index < text.size();
        if (!continues) {
            return logical + physical;
        }
        physical.pop_back();
        logical += physical;
    }
}

// Writes a line of code with its macros expanded, after a line marker if the output has lost track.
static void EmitCode(SourceFile& source, const string& logical, int startLine, int loc, string& out) {
    if (source.outLine != startLine) {
        out += "# " + std::to_string(startLine) + " \"" + source.name + "\"\n";
        source.outLine = startLine;
    }
    for (const string& token : Expand(PPTokens(logical), {}, loc)) {
        out += token;
    }
    out += '\n';
    source.outLine++;
}

void Preprocess(const string& file, const string& rawText, string& out, int depth) {
    SourceFile source(file, StripComments(rawText, file));
    while (source.index < source.text.size()) {
        int startLine = source.line;
        string logical = ReadLogicalLine(source);
        int loc = Location(file, startLine);
        size_t first = logical.find_first_not_of(" \t");
        bool isDirective = first != string::npos && logical[first] == '#';
        if (!isDirective) {
            if (source.Active()) {
                EmitCode(source, logical, startLine, loc, out);
            }
            continue;
        }

        source.outLine = -1;  // a marker is needed after any directive
        Directive directive = ParseDirective(logical.substr(first + 1), loc);
        // Directives other than the conditionals are ignored in a skipped block.
        if (!ConditionalDirective(source, directive) && source.Active()) {
            OtherDirective(source, directive, out, depth);
        }
    }
    if (!source.conditionals.empty()) {
        Fatal(source.conditionals.back().loc, "unterminated #if");
    }
}

void ResetPreprocessor() {
    fileNames.clear();
    includePaths.clear();
    macros.clear();
    onceFiles.clear();
}

}  // namespace Kestrel::Compiler
