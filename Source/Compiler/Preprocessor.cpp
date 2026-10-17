// Preprocessor.cpp -- #include, #define, #if and friends: C source text to C source text.

#include "CompilerInternal.h"
#include "Kestrel/Files.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

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

static void Append(vector<string>& to, const vector<string>& tokens) {
    to.insert(to.end(), tokens.begin(), tokens.end());
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
        size_t next = i + 1;
        while (next < tokens.size() && IsSpace(tokens[next])) {
            next++;
        }
        if (next >= tokens.size() || tokens[next] != "(") {
            out.push_back(name);
            continue;
        }

        // Split the arguments at the commas that aren't inside nested parentheses.
        vector<vector<string>> args(1);
        int depth = 0;
        for (next++;; next++) {
            if (next >= tokens.size()) {
                Fatal(loc, "unterminated call of macro '%s' (arguments must be on one line)", name.c_str());
            }
            const string& token = tokens[next];
            if (token == ")" && depth == 0) {
                break;
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
        i = next;

        // "f()" reads as one empty argument; for a macro with no parameters it is no arguments.
        if (args.size() == 1 && macro.params.empty()) {
            for (const string& token : args[0]) {
                if (!IsSpace(token)) {
                    Fatal(loc, "macro '%s' takes no arguments", name.c_str());
                }
            }
            args.clear();
        }
        if (args.size() != macro.params.size()) {
            Fatal(loc, "macro '%s' takes %d arguments, not %d", name.c_str(), (int)macro.params.size(), (int)args.size());
        }

        // Substitute each parameter with its argument, expanded on its own first.
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
        Append(out, Expand(body, insideMacro, loc));
    }
    return out;
}

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
        if (op == "||") {
            return left || right;
        }
        if (op == "&&") {
            return left && right;
        }
        if (op == "|") {
            return left | right;
        }
        if (op == "^") {
            return left ^ right;
        }
        if (op == "&") {
            return left & right;
        }
        if (op == "==") {
            return left == right;
        }
        if (op == "!=") {
            return left != right;
        }
        if (op == "<") {
            return left < right;
        }
        if (op == ">") {
            return left > right;
        }
        if (op == "<=") {
            return left <= right;
        }
        if (op == ">=") {
            return left >= right;
        }
        if (op == "<<") {
            return left << right;
        }
        if (op == ">>") {
            return left >> right;
        }
        if (op == "+") {
            return left + right;
        }
        if (op == "-") {
            return left - right;
        }
        if (op == "*") {
            return left * right;
        }
        if (op == "/") {
            return left / right;
        }
        return left % right;
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

static size_t SkipSpaces(const vector<string>& tokens, size_t index) {
    while (index < tokens.size() && IsSpace(tokens[index])) {
        index++;
    }
    return index;
}

static long long EvaluateIf(const vector<string>& line, int loc) {
    vector<string> replaced;  // defined X and defined(X) become 1 or 0 before expansion
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
    IfExpression expression;
    for (const string& token : Expand(replaced, {}, loc)) {
        if (!IsSpace(token)) {
            expression.tokens.push_back(token);
        }
    }
    expression.loc = loc;
    long long value = expression.Conditional();
    if (expression.pos != expression.tokens.size()) {
        Fatal(loc, "unexpected '%s' in #if", expression.tokens[expression.pos].c_str());
    }
    return value;
}

// Replaces comments with spaces (keeping their newlines), leaving string and character literals alone.
static string StripComments(const string& text, const string& file) {
    string out;
    int line = 1;
    size_t index = 0;
    auto startsWith = [&](const char* two) {
        return index + 1 < text.size() && text[index] == two[0] && text[index + 1] == two[1];
    };
    while (index < text.size()) {
        char character = text[index];
        if (character == '\n') {
            line++;
        }
        if (character == '"' || character == '\'') {
            char quote = character;
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
        else if (startsWith("//")) {
            while (index < text.size() && text[index] != '\n') {
                index++;
            }
        }
        else if (startsWith("/*")) {
            int startLine = line;
            index += 2;
            while (index + 1 < text.size() && !startsWith("*/")) {
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
        else {
            out += text[index++];
        }
    }
    return out;
}

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

// One #if / #ifdef / #ifndef ... #endif block being read.
struct ConditionalBlock {
    bool outerActive;  // whether the enclosing code is being kept
    bool active;       // whether the current branch is being kept
    bool taken;        // whether some branch has already been kept
    bool seenElse;
    int loc;
};

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
        for (index++;; index++) {
            if (index >= tokens.size()) {
                Fatal(loc, "missing ')' in macro parameters");
            }
            const string& token = tokens[index];
            if (IsSpace(token) || token == ",") {
                continue;
            }
            if (token == ")") {
                break;
            }
            if (token == ".") {
                Fatal(loc, "variadic macros are not supported");
            }
            if (!IsIdentStart(token[0])) {
                Fatal(loc, "bad macro parameter '%s'", token.c_str());
            }
            macro.params.push_back(token);
        }
        index++;
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

void Preprocess(const string& file, const string& rawText, string& out, int depth) {
    string text = StripComments(rawText, file);
    vector<ConditionalBlock> conditionals;
    auto active = [&] {
        return conditionals.empty() || conditionals.back().active;
    };
    int outLine = -1;  // the line the next output line will be taken as
    size_t index = 0;
    int line = 1;
    while (index < text.size()) {
        // one logical line, joining backslash-newline continuations
        int startLine = line;
        string logical;
        for (;;) {
            size_t newline = text.find('\n', index);
            string physical;
            if (newline == string::npos) {
                physical = text.substr(index);
                index = text.size();
            }
            else {
                physical = text.substr(index, newline - index);
                index = newline + 1;
            }
            line++;
            if (!physical.empty() && physical.back() == '\r') {
                physical.pop_back();
            }
            bool continues = !physical.empty() && physical.back() == '\\' && index < text.size();
            if (continues) {
                physical.pop_back();
                logical += physical;
                continue;
            }
            logical += physical;
            break;
        }
        int loc = Location(file, startLine);

        size_t first = logical.find_first_not_of(" \t");
        bool isDirective = first != string::npos && logical[first] == '#';
        if (!isDirective) {
            if (!active()) {
                continue;
            }
            if (outLine != startLine) {
                out += "# " + std::to_string(startLine) + " \"" + file + "\"\n";
                outLine = startLine;
            }
            for (const string& token : Expand(PPTokens(logical), {}, loc)) {
                out += token;
            }
            out += '\n';
            outLine++;
            continue;
        }

        outLine = -1;  // a marker is needed after any directive
        string directive;
        string rest;
        size_t nameStart = logical.find_first_not_of(" \t", first + 1);
        if (nameStart != string::npos) {
            size_t nameEnd = nameStart;
            while (nameEnd < logical.size() && IsIdentChar(logical[nameEnd])) {
                nameEnd++;
            }
            directive = logical.substr(nameStart, nameEnd - nameStart);
            rest = logical.substr(nameEnd);
        }
        vector<string> words;
        for (const string& token : PPTokens(rest)) {
            if (!IsSpace(token)) {
                words.push_back(token);
            }
        }

        if (directive == "ifdef" || directive == "ifndef") {
            if (active() && (words.empty() || !IsIdentStart(words[0][0]))) {
                Fatal(loc, "#%s needs a macro name", directive.c_str());
            }
            bool keep = false;
            if (active()) {
                bool isDefined = macros.count(words[0]) != 0;
                keep = directive == "ifdef" ? isDefined : !isDefined;
            }
            conditionals.push_back({active(), keep, keep, false, loc});
        }
        else if (directive == "if") {
            bool keep = active() && EvaluateIf(PPTokens(rest), loc) != 0;
            conditionals.push_back({active(), keep, keep, false, loc});
        }
        else if (directive == "elif" || directive == "else") {
            if (conditionals.empty()) {
                Fatal(loc, "#%s without #if", directive.c_str());
            }
            ConditionalBlock& block = conditionals.back();
            if (block.seenElse) {
                Fatal(loc, "#%s after #else", directive.c_str());
            }
            bool isElse = directive == "else";
            block.seenElse = isElse;
            // Only one branch is kept: the first whose condition holds, inside kept code.
            if (!block.outerActive || block.taken) {
                block.active = false;
            }
            else if (isElse) {
                block.active = true;
            }
            else {
                block.active = EvaluateIf(PPTokens(rest), loc) != 0;
            }
            if (block.active) {
                block.taken = true;
            }
        }
        else if (directive == "endif") {
            if (conditionals.empty()) {
                Fatal(loc, "#endif without #if");
            }
            conditionals.pop_back();
        }
        else if (!active()) {
            // other directives in a skipped block are ignored
        }
        else if (directive == "include") {
            Include(rest, file, loc, out, depth);
        }
        else if (directive == "define") {
            DefineMacro(rest, loc);
        }
        else if (directive == "undef") {
            if (words.empty()) {
                Fatal(loc, "#undef needs a macro name");
            }
            macros.erase(words[0]);
        }
        else if (directive == "error") {
            size_t messageStart = rest.find_first_not_of(" \t");
            string message;
            if (messageStart != string::npos) {
                message = rest.substr(messageStart);
            }
            Fatal(loc, "#error %s", message.c_str());
        }
        else if (directive == "pragma") {
            if (!words.empty() && words[0] == "once") {
                onceFiles.insert(file);
            }
        }
        else if (!directive.empty()) {
            Fatal(loc, "unknown directive '#%s'", directive.c_str());
        }
    }
    if (!conditionals.empty()) {
        Fatal(conditionals.back().loc, "unterminated #if");
    }
}

void ResetPreprocessor() {
    fileNames.clear();
    includePaths.clear();
    macros.clear();
    onceFiles.clear();
}

}  // namespace Kestrel::Compiler
