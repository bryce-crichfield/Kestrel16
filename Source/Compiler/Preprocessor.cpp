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

static bool IsIdentStart(char c) {
    return isalpha((unsigned char)c) || c == '_';
}
static bool IsIdentChar(char c) {
    return isalnum((unsigned char)c) || c == '_';
}

// Splits a line into pp-tokens: identifiers, numbers, string and character literals, runs of
// whitespace, and single other characters. Concatenating them gives back the line.
vector<string> PPTokens(const string& s) {
    vector<string> out;
    size_t i = 0;
    while (i < s.size()) {
        size_t start = i;
        char c = s[i];
        if (IsIdentStart(c)) {
            while (i < s.size() && IsIdentChar(s[i])) {
                i++;
            }
        }
        else if (isdigit((unsigned char)c)) {
            while (i < s.size() && (IsIdentChar(s[i]) || s[i] == '.')) {
                i++;
            }
        }
        else if (c == '"' || c == '\'') {
            i++;
            while (i < s.size() && s[i] != c) {
                i += s[i] == '\\' ? 2 : 1;
            }
            i = std::min(i + 1, s.size());
        }
        else if (isspace((unsigned char)c)) {
            while (i < s.size() && isspace((unsigned char)s[i])) {
                i++;
            }
        }
        else {
            i++;
        }
        out.push_back(s.substr(start, i - start));
    }
    return out;
}

static bool IsSpace(const string& t) {
    return !t.empty() && isspace((unsigned char)t[0]);
}

// Expands the macros in a list of pp-tokens. A macro is not expanded again inside its own
// expansion, so '#define x x + 1' terminates.
static vector<string> Expand(const vector<string>& in, std::set<string> disabled, int loc) {
    vector<string> out;
    for (size_t i = 0; i < in.size(); i++) {
        const string& t = in[i];
        auto it = IsIdentStart(t[0]) && !disabled.count(t) ? macros.find(t) : macros.end();
        if (it == macros.end()) {
            out.push_back(t);
            continue;
        }
        const Macro& m = it->second;
        std::set<string> inner = disabled;
        inner.insert(t);
        if (!m.function) {
            vector<string> e = Expand(m.body, inner, loc);
            out.insert(out.end(), e.begin(), e.end());
            continue;
        }
        size_t j = i + 1;
        while (j < in.size() && IsSpace(in[j])) {
            j++;
        }
        if (j >= in.size() || in[j] != "(") {
            out.push_back(t);
            continue;
        }  // a function-like name alone
        vector<vector<string>> args(1);
        int depth = 0;
        for (j++;; j++) {
            if (j >= in.size()) {
                Fatal(loc, "unterminated call of macro '%s' (arguments must be on one line)", t.c_str());
            }
            const string& a = in[j];
            if (a == ")" && depth == 0) {
                break;
            }
            if (a == "," && depth == 0) {
                args.emplace_back();
                continue;
            }
            if (a == "(") {
                depth++;
            }
            if (a == ")") {
                depth--;
            }
            args.back().push_back(a);
        }
        i = j;
        if (args.size() == 1 && m.params.empty()) {
            for (const string& a : args[0]) {
                if (!IsSpace(a)) {
                    Fatal(loc, "macro '%s' takes no arguments", t.c_str());
                }
            }
            args.clear();
        }
        if (args.size() != m.params.size()) {
            Fatal(loc, "macro '%s' takes %d arguments, not %d", t.c_str(), (int)m.params.size(), (int)args.size());
        }
        vector<string> body;
        for (const string& b : m.body) {
            size_t p = 0;
            while (p < m.params.size() && m.params[p] != b) {
                p++;
            }
            if (p == m.params.size()) {
                body.push_back(b);
                continue;
            }
            vector<string> e = Expand(args[p], disabled, loc);
            body.insert(body.end(), e.begin(), e.end());
        }
        vector<string> e = Expand(body, inner, loc);
        out.insert(out.end(), e.begin(), e.end());
    }
    return out;
}

// Evaluates the expression of an #if (after defined() and macro expansion), on non-space tokens.
struct IfExpression {
    vector<string> toks;
    size_t pos = 0;
    int loc;

    bool Is(const char* s) { return pos < toks.size() && toks[pos] == s; }
    bool Accept(const char* s) {
        if (!Is(s)) {
            return false;
        }
        pos++;
        return true;
    }

    long long Primary() {
        if (pos >= toks.size()) {
            Fatal(loc, "#if expression ends early");
        }
        string t = toks[pos++];
        if (t == "(") {
            long long v = Conditional();
            if (!Accept(")")) {
                Fatal(loc, "missing ')' in #if");
            }
            return v;
        }
        if (t == "!") {
            return !Primary();
        }
        if (t == "~") {
            return ~Primary();
        }
        if (t == "-") {
            return -Primary();
        }
        if (t == "+") {
            return Primary();
        }
        if (isdigit((unsigned char)t[0])) {
            return strtoll(t.c_str(), nullptr, 0);
        }
        if (t[0] == '\'') {
            return t.size() > 2 ? (unsigned char)t[1] : 0;
        }
        if (IsIdentStart(t[0])) {
            return 0;  // an identifier that isn't a macro is 0
        }
        Fatal(loc, "unexpected '%s' in #if", t.c_str());
    }

    // Binary operators by precedence, loosest first.
    long long Binary(int level) {
        static const vector<vector<string>> levels = {
            {"||"}, {"&&"}, {"|"}, {"^"}, {"&"}, {"==", "!="}, {"<", ">", "<=", ">="}, {"<<", ">>"}, {"+", "-"}, {"*", "/", "%"}};
        if (level == (int)levels.size()) {
            return Primary();
        }
        long long a = Binary(level + 1);
        for (;;) {
            string op;
            for (const string& o : levels[level]) {
                // two-character operators arrive as two tokens
                if (o.size() == 2 && pos + 1 < toks.size() && toks[pos] == o.substr(0, 1) && toks[pos + 1] == o.substr(1)) {
                    op = o;
                    break;
                }
                bool longer = string("<>|&").find(o) != string::npos && pos + 1 < toks.size() && (toks[pos + 1] == o || toks[pos + 1] == "=");
                if (o.size() == 1 && Is(o.c_str()) && !longer) {
                    op = o;
                    break;
                }
            }
            if (op.empty()) {
                return a;
            }
            pos += op.size();
            long long b = Binary(level + 1);
            if ((op == "/" || op == "%") && b == 0) {
                Fatal(loc, "division by zero in #if");
            }
            if (op == "||") {
                a = a || b;
            }
            else if (op == "&&") {
                a = a && b;
            }
            else if (op == "|") {
                a |= b;
            }
            else if (op == "^") {
                a ^= b;
            }
            else if (op == "&") {
                a &= b;
            }
            else if (op == "==") {
                a = a == b;
            }
            else if (op == "!=") {
                a = a != b;
            }
            else if (op == "<") {
                a = a < b;
            }
            else if (op == ">") {
                a = a > b;
            }
            else if (op == "<=") {
                a = a <= b;
            }
            else if (op == ">=") {
                a = a >= b;
            }
            else if (op == "<<") {
                a <<= b;
            }
            else if (op == ">>") {
                a >>= b;
            }
            else if (op == "+") {
                a += b;
            }
            else if (op == "-") {
                a -= b;
            }
            else if (op == "*") {
                a *= b;
            }
            else if (op == "/") {
                a /= b;
            }
            else {
                a %= b;
            }
        }
    }

    long long Conditional() {
        long long c = Binary(0);
        if (!Accept("?")) {
            return c;
        }
        long long a = Conditional();
        if (!Accept(":")) {
            Fatal(loc, "missing ':' in #if");
        }
        long long b = Conditional();
        return c ? a : b;
    }
};

static long long EvaluateIf(const vector<string>& line, int loc) {
    vector<string> replaced;  // defined X and defined(X) become 1 or 0 before expansion
    for (size_t i = 0; i < line.size(); i++) {
        if (line[i] != "defined") {
            replaced.push_back(line[i]);
            continue;
        }
        size_t j = i + 1;
        while (j < line.size() && IsSpace(line[j])) {
            j++;
        }
        bool paren = j < line.size() && line[j] == "(";
        if (paren) {
            j++;
            while (j < line.size() && IsSpace(line[j])) {
                j++;
            }
        }
        if (j >= line.size() || !IsIdentStart(line[j][0])) {
            Fatal(loc, "defined needs a macro name");
        }
        replaced.push_back(macros.count(line[j]) ? "1" : "0");
        if (paren) {
            j++;
            while (j < line.size() && IsSpace(line[j])) {
                j++;
            }
            if (j >= line.size() || line[j] != ")") {
                Fatal(loc, "missing ')' after defined");
            }
        }
        i = j;
    }
    IfExpression e;
    for (const string& t : Expand(replaced, {}, loc)) {
        if (!IsSpace(t)) {
            e.toks.push_back(t);
        }
    }
    e.loc = loc;
    long long v = e.Conditional();
    if (e.pos != e.toks.size()) {
        Fatal(loc, "unexpected '%s' in #if", e.toks[e.pos].c_str());
    }
    return v;
}

// Replaces comments with spaces (keeping their newlines), leaving string and character literals alone.
static string StripComments(const string& s, const string& file) {
    string out;
    int line = 1;
    for (size_t i = 0; i < s.size();) {
        char c = s[i];
        if (c == '\n') {
            line++;
        }
        if (c == '"' || c == '\'') {
            out += s[i++];
            while (i < s.size() && s[i] != c && s[i] != '\n') {
                if (s[i] == '\\' && i + 1 < s.size()) {
                    out += s[i++];
                }
                out += s[i++];
            }
            if (i < s.size() && s[i] == c) {
                out += s[i++];
            }
        }
        else if (c == '/' && i + 1 < s.size() && s[i + 1] == '/') {
            while (i < s.size() && s[i] != '\n') {
                i++;
            }
        }
        else if (c == '/' && i + 1 < s.size() && s[i + 1] == '*') {
            int start = line;
            i += 2;
            while (i + 1 < s.size() && !(s[i] == '*' && s[i + 1] == '/')) {
                if (s[i] == '\n') {
                    out += '\n';
                    line++;
                }
                i++;
            }
            if (i + 1 >= s.size()) {
                Fatal(Location(file, start), "unterminated comment");
            }
            i += 2;
            out += ' ';
        }
        else {
            out += s[i++];
        }
    }
    return out;
}

static string Folder(const string& path) {
    size_t slash = path.find_last_of("/\\");
    return slash == string::npos ? "" : path.substr(0, slash + 1);
}

static void Include(const string& rest, const string& file, int loc, string& out, int depth) {
    size_t open = rest.find_first_of("\"<");
    char close = open != string::npos && rest[open] == '<' ? '>' : '"';
    size_t end = open == string::npos ? string::npos : rest.find(close, open + 1);
    if (end == string::npos) {
        Fatal(loc, "#include expects \"file\" or <file>");
    }
    string name = rest.substr(open + 1, end - open - 1);
    vector<string> folders;
    if (close == '"') {
        folders.push_back(Folder(file));
    }
    for (const string& p : includePaths) {
        folders.push_back(p.empty() || p.back() == '/' || p.back() == '\\' ? p : p + "/");
    }
    for (const string& folder : folders) {
        string text, path = folder + name;
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

void Preprocess(const string& file, const string& rawText, string& out, int depth) {
    string text = StripComments(rawText, file);
    struct Conditional {
        bool outerActive, active, taken, seenElse;
        int loc;
    };
    vector<Conditional> conditionals;
    auto active = [&] { return conditionals.empty() || conditionals.back().active; };
    int outLine = -1;  // the line the next output line will be taken as
    size_t i = 0;
    int line = 1;
    while (i < text.size()) {
        // one logical line, joining backslash-newline continuations
        int startLine = line;
        string logical;
        for (;;) {
            size_t nl = text.find('\n', i);
            string physical = text.substr(i, nl == string::npos ? string::npos : nl - i);
            i = nl == string::npos ? text.size() : nl + 1;
            line++;
            if (!physical.empty() && physical.back() == '\r') {
                physical.pop_back();
            }
            if (!physical.empty() && physical.back() == '\\' && i < text.size()) {
                physical.pop_back();
                logical += physical;
                continue;
            }
            logical += physical;
            break;
        }
        int loc = Location(file, startLine);
        size_t first = logical.find_first_not_of(" \t");
        if (first == string::npos || logical[first] != '#') {
            if (!active()) {
                continue;
            }
            if (outLine != startLine) {
                out += "# " + std::to_string(startLine) + " \"" + file + "\"\n";
                outLine = startLine;
            }
            for (const string& t : Expand(PPTokens(logical), {}, loc)) {
                out += t;
            }
            out += '\n';
            outLine++;
            continue;
        }
        outLine = -1;  // a marker is needed after any directive
        size_t d = logical.find_first_not_of(" \t", first + 1);
        size_t dend = d;
        while (dend < logical.size() && IsIdentChar(logical[dend])) {
            dend++;
        }
        string directive = d == string::npos ? "" : logical.substr(d, dend - d);
        string rest = d == string::npos ? "" : logical.substr(dend);
        vector<string> words;
        for (const string& t : PPTokens(rest)) {
            if (!IsSpace(t)) {
                words.push_back(t);
            }
        }

        if (directive == "ifdef" || directive == "ifndef") {
            if (active() && (words.empty() || !IsIdentStart(words[0][0]))) {
                Fatal(loc, "#%s needs a macro name", directive.c_str());
            }
            bool on = active() && (macros.count(words[0]) != 0) == (directive == "ifdef");
            conditionals.push_back({active(), on, on, false, loc});
        }
        else if (directive == "if") {
            bool on = active() && EvaluateIf(PPTokens(rest), loc) != 0;
            conditionals.push_back({active(), on, on, false, loc});
        }
        else if (directive == "elif" || directive == "else") {
            if (conditionals.empty()) {
                Fatal(loc, "#%s without #if", directive.c_str());
            }
            Conditional& c = conditionals.back();
            if (c.seenElse) {
                Fatal(loc, "#%s after #else", directive.c_str());
            }
            c.seenElse = directive == "else";
            c.active = c.outerActive && !c.taken && (directive == "else" || EvaluateIf(PPTokens(rest), loc) != 0);
            c.taken = c.taken || c.active;
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
            vector<string> t = PPTokens(rest);
            size_t k = 0;
            while (k < t.size() && IsSpace(t[k])) {
                k++;
            }
            if (k >= t.size() || !IsIdentStart(t[k][0])) {
                Fatal(loc, "#define needs a macro name");
            }
            string name = t[k++];
            Macro m;
            if (k < t.size() && t[k] == "(") {  // '(' right after the name: function-like
                m.function = true;
                for (k++;; k++) {
                    if (k >= t.size()) {
                        Fatal(loc, "missing ')' in macro parameters");
                    }
                    if (IsSpace(t[k]) || t[k] == ",") {
                        continue;
                    }
                    if (t[k] == ")") {
                        break;
                    }
                    if (t[k] == ".") {
                        Fatal(loc, "variadic macros are not supported");
                    }
                    if (!IsIdentStart(t[k][0])) {
                        Fatal(loc, "bad macro parameter '%s'", t[k].c_str());
                    }
                    m.params.push_back(t[k]);
                }
                k++;
            }
            while (k < t.size() && IsSpace(t[k])) {
                k++;
            }
            m.body.assign(t.begin() + k, t.end());
            while (!m.body.empty() && IsSpace(m.body.back())) {
                m.body.pop_back();
            }
            for (const string& b : m.body) {
                if (b == "#") {
                    Fatal(loc, "# and ## in macros are not supported");
                }
            }
            macros[name] = m;
        }
        else if (directive == "undef") {
            if (words.empty()) {
                Fatal(loc, "#undef needs a macro name");
            }
            macros.erase(words[0]);
        }
        else if (directive == "error") {
            size_t s = rest.find_first_not_of(" \t");
            Fatal(loc, "#error %s", s == string::npos ? "" : rest.substr(s).c_str());
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
