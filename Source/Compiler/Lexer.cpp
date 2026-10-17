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

static int HexValue(char digit) {
    if (digit >= '0' && digit <= '9') {
        return digit - '0';
    }
    if (digit >= 'a' && digit <= 'f') {
        return digit - 'a' + 10;
    }
    if (digit >= 'A' && digit <= 'F') {
        return digit - 'A' + 10;
    }
    return -1;
}

static int ReadEscape(const string& source, size_t& index, int line) {
    // source[index] is the character after the backslash
    char escape = source[index++];
    switch (escape) {
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
            while (index < source.size() && HexValue(source[index]) >= 0) {
                value = value * 16 + HexValue(source[index++]);
                digits++;
            }
            if (!digits) {
                Fatal(line, "\\x needs hex digits");
            }
            return value & 0xFF;
        }
        default:
            if (escape >= '0' && escape <= '7') {
                int value = escape - '0', digits = 1;
                while (digits < 3 && index < source.size() && source[index] >= '0' && source[index] <= '7') {
                    value = value * 8 + (source[index++] - '0');
                    digits++;
                }
                return value & 0xFF;
            }
            Fatal(line, "unknown escape '\\%c'", escape);
    }
}

vector<Token> Lex(const string& source) {
    vector<Token> tokens;
    size_t index = 0;
    int line = 1;
    bool lineStart = true;

    while (index < source.size()) {
        char character = source[index];
        if (character == '\n') {
            line++;
            index++;
            lineStart = true;
            continue;
        }
        if (isspace((unsigned char)character)) {
            index++;
            continue;
        }
        if (character == '/' && index + 1 < source.size() && source[index + 1] == '/') {
            while (index < source.size() && source[index] != '\n') {
                index++;
            }
            continue;
        }
        if (character == '/' && index + 1 < source.size() && source[index + 1] == '*') {
            int startLine = line;
            index += 2;
            while (index + 1 < source.size() && !(source[index] == '*' && source[index + 1] == '/')) {
                if (source[index] == '\n') {
                    line++;
                }
                index++;
            }
            if (index + 1 >= source.size()) {
                Fatal(startLine, "unterminated comment");
            }
            index += 2;
            continue;
        }
        if (character == '#' && lineStart) {  // a line marker from Preprocess(): # <line> "<file>"
            size_t end = source.find('\n', index);
            if (end == string::npos) {
                end = source.size();
            }
            string marker = source.substr(index, end - index);
            size_t open = marker.find('"');
            size_t close = marker.rfind('"');
            if (open == string::npos || close <= open) {
                Fatal(line, "malformed line marker");
            }
            string file = marker.substr(open + 1, close - open - 1);
            int markerLine = atoi(marker.c_str() + 1);
            line = Location(file, markerLine) - 1;  // the newline ending the marker adds the 1 back
            index = end;
            continue;
        }
        lineStart = false;

        Token token;
        token.line = line;

        if (isdigit((unsigned char)character)) {
            unsigned long long value = 0;
            if (character == '0' && index + 1 < source.size() && (source[index + 1] == 'x' || source[index + 1] == 'X')) {
                index += 2;
                if (HexValue(source[index]) < 0) {
                    Fatal(line, "malformed hex number");
                }
                while (index < source.size() && HexValue(source[index]) >= 0) {
                    value = value * 16 + HexValue(source[index++]);
                }
            }
            else if (character == '0' && index + 1 < source.size() && (source[index + 1] == 'b' || source[index + 1] == 'B')) {
                index += 2;
                while (index < source.size() && (source[index] == '0' || source[index] == '1')) {
                    value = value * 2 + (source[index++] - '0');
                }
            }
            else if (character == '0') {
                while (index < source.size() && source[index] >= '0' && source[index] <= '7') {
                    value = value * 8 + (source[index++] - '0');
                }
            }
            else {
                while (index < source.size() && isdigit((unsigned char)source[index])) {
                    value = value * 10 + (source[index++] - '0');
                }
            }
            if (index < source.size() && (source[index] == '.' || source[index] == 'e' || source[index] == 'E')) {
                Fatal(line, "floating point is not supported");
            }
            bool isUnsigned = false;
            while (index < source.size() && strchr("uUlL", source[index])) {
                if (source[index] == 'u' || source[index] == 'U') {
                    isUnsigned = true;
                }
                index++;
            }
            if (index < source.size() && (isalnum((unsigned char)source[index]) || source[index] == '_')) {
                Fatal(line, "malformed number");
            }
            if (value > 0xFFFF) {
                Fatal(line, "constant %llu does not fit in 16 bits", value);
            }
            token.kind = TokNumber;
            token.value = (long long)value;
            token.isUnsigned = isUnsigned || value > 0x7FFF;  // too big for int, so it is unsigned
            tokens.push_back(token);
            continue;
        }
        if (character == '\'') {
            index++;
            if (index >= source.size() || source[index] == '\'' || source[index] == '\n') {
                Fatal(line, "empty character literal");
            }
            int value;
            if (source[index] == '\\') {
                index++;
                value = ReadEscape(source, index, line);
            }
            else {
                value = (unsigned char)source[index++];
            }
            if (index >= source.size() || source[index] != '\'') {
                Fatal(line, "unterminated character literal");
            }
            index++;
            token.kind = TokNumber;
            token.value = (signed char)value;  // char is signed
            tokens.push_back(token);
            continue;
        }
        if (character == '"') {
            index++;
            string bytes;
            while (index < source.size() && source[index] != '"') {
                if (source[index] == '\n') {
                    Fatal(line, "unterminated string");
                }
                if (source[index] == '\\') {
                    index++;
                    bytes += (char)ReadEscape(source, index, line);
                }
                else {
                    bytes += source[index++];
                }
            }
            if (index >= source.size()) {
                Fatal(line, "unterminated string");
            }
            index++;
            if (!tokens.empty() && tokens.back().kind == TokString) {
                tokens.back().text += bytes;  // "abc" "def" concatenation
                continue;
            }
            token.kind = TokString;
            token.text = bytes;
            tokens.push_back(token);
            continue;
        }
        if (isalpha((unsigned char)character) || character == '_') {
            size_t start = index;
            while (index < source.size() && (isalnum((unsigned char)source[index]) || source[index] == '_')) {
                index++;
            }
            token.text = source.substr(start, index - start);
            token.kind = Keywords.count(token.text) ? TokKeyword : TokIdent;
            tokens.push_back(token);
            continue;
        }
        bool found = false;
        for (const char* punctuator : Punctuators) {
            size_t length = strlen(punctuator);
            if (source.compare(index, length, punctuator) == 0) {
                token.kind = TokPunct;
                token.text = punctuator;
                index += length;
                found = true;
                break;
            }
        }
        if (!found) {
            Fatal(line, "unexpected character '%c'", character);
        }
        tokens.push_back(token);
    }
    Token eof;
    eof.kind = TokEof;
    eof.text = "end of file";
    eof.line = line;
    tokens.push_back(eof);
    return tokens;
}

}  // namespace Kestrel::Compiler
