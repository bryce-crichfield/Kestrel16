// Expressions.h -- expressions, typed as they are built, and constant evaluation (Expressions.cpp).
// Internal to Source/Compiler; Compiler.h is the compiler's interface.

#pragma once

#include "Types.h"

namespace Kestrel::Compiler {

Node* Expression();
Node* Assignment();
Node* Conditional();
long long ConstantExpression();
long long Evaluate(Node* node, string* label = nullptr);
long long Wrap16(long long value, bool isUnsigned);
Node* MakeCast(Node* expr, Type* type);
Node* MakeAssign(Node* lhs, Node* rhs, int line);
void NeedScalar(Node* node, const char* what);
void NeedInteger(Node* node, const char* what);

}  // namespace Kestrel::Compiler
