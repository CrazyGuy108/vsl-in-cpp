#include "ast/vslContext.hpp"
#include "diag/diag.hpp"
#include "irgen/irgen.hpp"
#include "lexer/vsllexer.hpp"
#include "parser/vslparser.hpp"
#include "gtest/gtest.h"

#define valid(src) EXPECT_TRUE(validate(src))
#define invalid(src) EXPECT_FALSE(validate(src))

// returns true if semantically valid, false otherwise
static bool validate(const char* src)
{
    VSLContext vslCtx;
    Diag diag{ llvm::nulls() };
    VSLLexer lexer{ diag, src };
    VSLParser parser{ vslCtx, lexer };
    parser.parse();
    llvm::LLVMContext llvmContext;
    auto module = std::make_unique<llvm::Module>("test", llvmContext);
    IRGen irgen{ vslCtx, diag, *module };
    irgen.run();
    return !diag.getNumErrors();
}

TEST(IRGenTest, Functions)
{
    valid("public func f() -> Void {}");
    valid("public func f(x: Int) -> Void { return; }");
    valid("public func f(x: Int) -> Int { return x + 1; }");
    valid("public func f() -> Void { g(); } "
        "public func g() -> Void external(h);");
    invalid("public func f() -> Void { h(); } "
        "public func g() -> Void external(h);");
    // can't have void parameters
    invalid("public func f(x: Void) -> Void { return x; }");
    // can't return a void expression
    invalid("public func f() -> Void { return f(); }");
    // able to call a function ahead of its definition
    valid("public func f() -> Void { g(); } private func g() -> Void {}");
    // can't call non-functions
    invalid("public func f(x: Int) -> Int { return x(); }");
}

TEST(IRGenTest, Conditionals)
{
    // else case is optional
    valid("public func f(x: Int) -> Int { if (x % 2 == 0) return 1337; "
        "return x; }");
    // if/else can be nested
    valid("public func f(x: Int) -> Int "
        "{ "
            "if (x > 0) "
                "if (x > 1337) "
                    "x = 5; "
                "else "
                    "return 1; "
            "else "
                "return 2; "
            "return x; "
        "}");
    // if/else can be chained
    valid("public func f(x: Int) -> Int { if (x == 0) return 0; "
        "else if (x == 1) return 1; else return x; }");
    valid("public func fibonacci(x: Int) -> Int "
        "{ "
            "if (x <= 0) return 0; "
            "else if (x == 1) return 1; "
            "else return fibonacci(x: x - 1) + fibonacci(x: x - 2); "
        "}");
    // make sure ternary works
    valid("public func f(x: Int) -> Int { return x == 4 ? x : x + 1; }");
    // ternaries can also be chained on both sides of the colon without parens
    valid("public func f(x: Int) -> Int { return x == 4 ? "
        "x == 3 ? x : x + 1 :"
            "x == 2 ? x + 2 : x + 3; }");
}

TEST(IRGenTest, Variables)
{
    valid("public func f(x: Int) -> Int { let y: Int = x * 2; y = y / x; "
        "return y; }");
    valid("public var x: Int = f(); private func f() -> Int { return 2; }");
    valid("public var x: Int = 4; public func f() -> Int { return x + 1; }");
    valid("private var x: Int = 3; public var y: Int = x + 2;");
    // can't access a global variable before it gets initialized
    invalid("public var x: Int = z; public var z: Int = 1;");
    // not implemented yet
    invalid("public func f() -> Int { return x + 1; } public var x: Int = 2;");
}
