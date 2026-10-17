// Lexer.cpp -- preprocessed C text to tokens.

#include "CompilerInternal.h"

#include <cctype>
#include <cstdlib>
#include <cstring>

namespace Kestrel::Compiler {

static const std::set<string> Keywords = {
    "auto", "break", "case", "char", "const", "continue", "default", "do", "double",
    "else", "enum", "extern", "float", "for", "goto", "if", "inline", "int", "long",
    "register", "restrict", "return", "short", "signed", "sizeof", "static", "struct",
    "switch", "typedef", "union", "unsigned", "void", "volatile", "while", "_Bool"};

static const char* Punctuators[] = {
    "<<=", ">>=", "...", "->", "++", "--", "<<", ">>", "<=", ">=", "==", "!=", "&&", "||",
    "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=",
    "+", "-", "*", "/", "%", "&", "|", "^", "~", "!", "<", ">", "=", "?", ":", ";", ",",
    ".", "(", ")", "[", "]", "{", "}"};

static int HexValue(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static int ReadEscape(const string& src, size_t& i, int line) {
    // src[i] is the character after the backslash
    char c = src[i++];
    switch (c) {
        case 'n':
            return '\n';
        case 't':
            return '\t';
        case 'r':
            return '\r';
        case 'a':
            return 7;
        case 'b':
            return 8;
        case 'f':
            return 12;
        case 'v':
            return 11;
        case 'e':
            return 27;
        case '\\':
            return '\\';
        case '\'':
            return '\'';
        case '"':
            return '"';
        case '?':
            return '?';
        case 'x': {
            int value = 0, digits = 0;
            while (i < src.size() && HexValue(src[i]) >= 0) {
                value = value * 16 + HexValue(src[i++]);
                digits++;
            }
            if (!digits) {
                Fatal(line, "\\x needs hex digits");
            }
            return value & 0xFF;
        }
        default:
            if (c >= '0' && c <= '7') {
                int value = c - '0', digits = 1;
                while (digits < 3 && i < src.size() && src[i] >= '0' && src[i] <= '7') {
                    value = value * 8 + (src[i++] - '0');
                    digits++;
                }
                return value & 0xFF;
            }
            Fatal(line, "unknown escape '\\%c'", c);
    }
}

vector<Token> Lex(const string& src) {
    vector<Token> tokens;
    size_t i = 0;
    int line = 1;
    bool lineStart = true;

    while (i < src.size()) {
        char c = src[i];
        if (c == '\n') {
            line++;
            i++;
            lineStart = true;
            continue;
        }
        if (isspace((unsigned char)c)) {
            i++;
            continue;
        }
        if (c == '/' && i + 1 < src.size() && src[i + 1] == '/') {
            while (i < src.size() && src[i] != '\n') {
                i++;
            }
            continue;
        }
        if (c == '/' && i + 1 < src.size() && src[i + 1] == '*') {
            int startLine = line;
            i += 2;
            while (i + 1 < src.size() && !(src[i] == '*' && src[i + 1] == '/')) {
                if (src[i] == '\n') {
                    line++;
                }
                i++;
            }
            if (i + 1 >= src.size()) {
                Fatal(startLine, "unterminated comment");
            }
            i += 2;
            continue;
        }
        if (c == '#' && lineStart) {  // a line marker from Preprocess(): # <line> "<file>"
            size_t end = src.find('\n', i);
            string marker = src.substr(i, end == string::npos ? string::npos : end - i);
            size_t open = marker.find('"'), close = marker.rfind('"');
            if (open == string::npos || close <= open) {
                Fatal(line, "malformed line marker");
            }
            line = Location(marker.substr(open + 1, close - open - 1), atoi(marker.c_str() + 1)) - 1;
            i = end == string::npos ? src.size() : end;
            continue;
        }
        lineStart = false;

        Token tok;
        tok.line = line;

        if (isdigit((unsigned char)c)) {
            unsigned long long value = 0;
            if (c == '0' && i + 1 < src.size() && (src[i + 1] == 'x' || src[i + 1] == 'X')) {
                i += 2;
                if (HexValue(src[i]) < 0) {
                    Fatal(line, "malformed hex number");
                }
                while (i < src.size() && HexValue(src[i]) >= 0) {
                    value = value * 16 + HexValue(src[i++]);
                }
            }
            else if (c == '0' && i + 1 < src.size() && (src[i + 1] == 'b' || src[i + 1] == 'B')) {
                i += 2;
                while (i < src.size() && (src[i] == '0' || src[i] == '1')) {
                    value = value * 2 + (src[i++] - '0');
                }
            }
            else if (c == '0') {
                while (i < src.size() && src[i] >= '0' && src[i] <= '7') {
                    value = value * 8 + (src[i++] - '0');
                }
            }
            else {
                while (i < src.size() && isdigit((unsigned char)src[i])) {
                    value = value * 10 + (src[i++] - '0');
                }
            }
            if (i < src.size() && (src[i] == '.' || src[i] == 'e' || src[i] == 'E')) {
                Fatal(line, "floating point is not supported");
            }
            bool isUnsigned = false;
            while (i < src.size() && strchr("uUlL", src[i])) {
                if (src[i] == 'u' || src[i] == 'U') {
                    isUnsigned = true;
                }
                i++;
            }
            if (i < src.size() && (isalnum((unsigned char)src[i]) || src[i] == '_')) {
                Fatal(line, "malformed number");
            }
            if (value > 0xFFFF) {
                Fatal(line, "constant %llu does not fit in 16 bits", value);
            }
            tok.kind = TokNumber;
            tok.value = (long long)value;
            tok.isUnsigned = isUnsigned || value > 0x7FFF;
            tokens.push_back(tok);
            continue;
        }
        if (c == '\'') {
            i++;
            if (i >= src.size() || src[i] == '\'' || src[i] == '\n') {
                Fatal(line, "empty character literal");
            }
            int value;
            if (src[i] == '\\') {
                i++;
                value = ReadEscape(src, i, line);
            }
            else {
                value = (unsigned char)src[i++];
            }
            if (i >= src.size() || src[i] != '\'') {
                Fatal(line, "unterminated character literal");
            }
            i++;
            tok.kind = TokNumber;
            tok.value = (signed char)value;  // char is signed
            tokens.push_back(tok);
            continue;
        }
        if (c == '"') {
            i++;
            string bytes;
            while (i < src.size() && src[i] != '"') {
                if (src[i] == '\n') {
                    Fatal(line, "unterminated string");
                }
                if (src[i] == '\\') {
                    i++;
                    bytes += (char)ReadEscape(src, i, line);
                }
                else {
                    bytes += src[i++];
                }
            }
            if (i >= src.size()) {
                Fatal(line, "unterminated string");
            }
            i++;
            if (!tokens.empty() && tokens.back().kind == TokString) {
                tokens.back().text += bytes;  // "abc" "def" concatenation
                continue;
            }
            tok.kind = TokString;
            tok.text = bytes;
            tokens.push_back(tok);
            continue;
        }
        if (isalpha((unsigned char)c) || c == '_') {
            size_t start = i;
            while (i < src.size() && (isalnum((unsigned char)src[i]) || src[i] == '_')) {
                i++;
            }
            tok.text = src.substr(start, i - start);
            tok.kind = Keywords.count(tok.text) ? TokKeyword : TokIdent;
            tokens.push_back(tok);
            continue;
        }
        bool found = false;
        for (const char* p : Punctuators) {
            size_t length = strlen(p);
            if (src.compare(i, length, p) == 0) {
                tok.kind = TokPunct;
                tok.text = p;
                i += length;
                found = true;
                break;
            }
        }
        if (!found) {
            Fatal(line, "unexpected character '%c'", c);
        }
        tokens.push_back(tok);
    }
    Token eof;
    eof.kind = TokEof;
    eof.text = "end of file";
    eof.line = line;
    tokens.push_back(eof);
    return tokens;
}

}  // namespace Kestrel::Compiler
