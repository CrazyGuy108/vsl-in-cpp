#include "parser/vslparser.hpp"

VSLParser::VSLParser(VSLContext& vslContext, Lexer& lexer, std::ostream& errors)
    : vslContext{ vslContext }, lexer{ lexer }, errors{ errors },
    errored{ false }
{
}

// program -> statements eof
std::unique_ptr<BlockNode> VSLParser::parse()
{
    // it's assumed that the token cache is empty, so calling next() should get
    //  the very first token from the lexer
    Location savedLocation = next().location;
    auto statements = parseStatements();
    if (current().kind != TokenKind::END)
    {
        errorExpected("eof");
    }
    return std::make_unique<BlockNode>(std::move(statements), savedLocation);
}

bool VSLParser::hasError() const
{
    return errored;
}

const Token& VSLParser::next()
{
    cache.emplace_back(lexer.nextToken());
    return current();
}

const Token& VSLParser::current()
{
    return cache.back();
}

std::unique_ptr<EmptyNode> VSLParser::errorExpected(const char* s)
{
    const Token& t = current();
    errors << t.location << ": error: expected " << s << " but found " <<
        getTokenKindName(t.kind) << '\n';
    errored = true;
    return std::make_unique<EmptyNode>(t.location);
}

std::unique_ptr<EmptyNode> VSLParser::errorUnexpected(const Token& token)
{
    errors << token.location << ": error: unexpected token " <<
        getTokenKindName(token.kind) << '\n';
    errored = true;
    return std::make_unique<EmptyNode>(token.location);
}

// statements -> statement*
std::vector<std::unique_ptr<Node>> VSLParser::parseStatements()
{
    std::vector<std::unique_ptr<Node>> statements;
    while (true)
    {
        // the cases here are in the follow set, telling when to stop expanding
        //  the statements production
        switch (current().kind)
        {
        case TokenKind::RBRACE:
        case TokenKind::END:
            return statements;
        default:
            statements.emplace_back(parseStatement());
        }
    }
}

// statement -> assignment | function | return | conditional | expr semicolon
//            | block | empty
std::unique_ptr<Node> VSLParser::parseStatement()
{
    // the cases here are first sets, distinguishing which production to go for
    //  based on a token of lookahead
    switch (current().kind)
    {
    case TokenKind::KW_VAR:
    case TokenKind::KW_LET:
        return parseVariable();
    case TokenKind::KW_FUNC:
        return parseFunction();
    case TokenKind::KW_RETURN:
        return parseReturn();
    case TokenKind::KW_IF:
        return parseIf();
    case TokenKind::IDENTIFIER:
    case TokenKind::NUMBER:
    case TokenKind::KW_TRUE:
    case TokenKind::KW_FALSE:
    case TokenKind::PLUS:
    case TokenKind::MINUS:
    case TokenKind::LPAREN:
        {
            auto expr = parseExpr();
            if (current().kind != TokenKind::SEMICOLON)
            {
                return errorExpected("';'");
            }
            next();
            return expr;
        }
    case TokenKind::LBRACE:
        return parseBlock();
    case TokenKind::SEMICOLON:
        return parseEmptyStatement();
    default:
        ;
    }
    auto e = errorUnexpected(current());
    next();
    return e;
}

// empty -> semicolon
std::unique_ptr<EmptyNode> VSLParser::parseEmptyStatement()
{
    const Token& semicolon = current();
    if (semicolon.kind != TokenKind::SEMICOLON)
    {
        return errorExpected("';'");
    }
    next();
    return std::make_unique<EmptyNode>(semicolon.location);
}

// block -> lbrace statements rbrace
std::unique_ptr<Node> VSLParser::parseBlock()
{
    const Token& lbrace = current();
    if (lbrace.kind != TokenKind::LBRACE)
    {
        return errorExpected("'{'");
    }
    next();
    auto statements = parseStatements();
    if (current().kind != TokenKind::RBRACE)
    {
        return errorExpected("'}'");
    }
    next();
    return std::make_unique<BlockNode>(std::move(statements), lbrace.location);
}

// conditional -> if lparen expr rparen statement (else statement)?
std::unique_ptr<Node> VSLParser::parseIf()
{
    Location savedLocation = current().location;
    if (current().kind != TokenKind::KW_IF)
    {
        return errorExpected("'if'");
    }
    if (next().kind != TokenKind::LPAREN)
    {
        return errorExpected("'('");
    }
    next();
    auto condition = parseExpr();
    if (current().kind != TokenKind::RPAREN)
    {
        return errorExpected("')'");
    }
    next();
    auto thenCase = parseStatement();
    std::unique_ptr<Node> elseCase;
    if (current().kind == TokenKind::KW_ELSE)
    {
        next();
        elseCase = parseStatement();
    }
    else
    {
        elseCase = std::make_unique<EmptyNode>(current().location);
    }
    return std::make_unique<IfNode>(std::move(condition), std::move(thenCase),
        std::move(elseCase), savedLocation);
}

// assignment -> (var | let) identifier colon type assign expr semicolon
std::unique_ptr<Node> VSLParser::parseVariable()
{
    bool isConst;
    TokenKind k = current().kind;
    if (k == TokenKind::KW_VAR)
    {
        isConst = false;
    }
    else if (k == TokenKind::KW_LET)
    {
        isConst = true;
    }
    else
    {
        return errorExpected("'let' or 'var'");
    }
    Location savedLocation = current().location;
    next();
    const Token& id = current();
    if (id.kind != TokenKind::IDENTIFIER)
    {
        return errorExpected("identifier");
    }
    next();
    if (current().kind != TokenKind::COLON)
    {
        return errorExpected("':'");
    }
    next();
    const Type* type = parseType();
    if (current().kind != TokenKind::ASSIGN)
    {
        return errorExpected("'='");
    }
    next();
    auto value = parseExpr();
    if (current().kind != TokenKind::SEMICOLON)
    {
        return errorExpected("';'");
    }
    next();
    return std::make_unique<VariableNode>(id.text, type, std::move(value),
        isConst, savedLocation);
}

// function -> func identifier lparen param (comma param)* arrow type block
std::unique_ptr<Node> VSLParser::parseFunction()
{
    const Token& func = current();
    if (func.kind != TokenKind::KW_FUNC)
    {
        return errorExpected("'func'");
    }
    Location savedLocation = func.location;
    const Token& id = next();
    if (id.kind != TokenKind::IDENTIFIER)
    {
        return errorExpected("identifier");
    }
    if (next().kind != TokenKind::LPAREN)
    {
        return errorExpected("'('");
    }
    next();
    std::vector<std::unique_ptr<ParamNode>> params;
    if (current().kind != TokenKind::RPAREN)
    {
        while (true)
        {
            params.emplace_back(parseParam());
            if (current().kind != TokenKind::COMMA)
            {
                break;
            }
            next();
        }
    }
    if (current().kind != TokenKind::RPAREN)
    {
        return errorExpected("')'");
    }
    if (next().kind != TokenKind::ARROW)
    {
        return errorExpected("'->'");
    }
    next();
    const Type* returnType = parseType();
    auto body = parseBlock();
    return std::make_unique<FunctionNode>(id.text, std::move(params),
        returnType, std::move(body), savedLocation);
}

// param -> identifier ':' type
std::unique_ptr<ParamNode> VSLParser::parseParam()
{
    const Token& id = current();
    llvm::StringRef name;
    if (id.kind != TokenKind::IDENTIFIER)
    {
        errorExpected("identifier");
        name = "";
    }
    else
    {
        name = id.text;
    }
    if (next().kind != TokenKind::COLON)
    {
        errorExpected("':'");
    }
    else
    {
        next();
    }
    const Type* type = parseType();
    return std::make_unique<ParamNode>(name, type, id.location);
}

// return -> 'return' expr ';'
std::unique_ptr<Node> VSLParser::parseReturn()
{
    const Token& ret = current();
    if (ret.kind != TokenKind::KW_RETURN)
    {
        return errorExpected("'return'");
    }
    Location savedLocation = ret.location;
    next();
    auto value = parseExpr();
    if (current().kind != TokenKind::SEMICOLON)
    {
        return errorExpected("';'");
    }
    next();
    return std::make_unique<ReturnNode>(std::move(value), savedLocation);
}

// Pratt parsing/TDOP (Top Down Operator Precedence) is used for expressions
std::unique_ptr<ExprNode> VSLParser::parseExpr(int rbp)
{
    auto left = parseNud();
    while (rbp < getLbp(current()))
    {
        left = parseLed(std::move(left));
    }
    return left;
}

std::unique_ptr<ExprNode> VSLParser::parseNud()
{
    const Token& token = current();
    next();
    switch (token.kind)
    {
    case TokenKind::IDENTIFIER:
        return std::make_unique<IdentNode>(token.text, token.location);
    case TokenKind::NUMBER:
        return parseNumber(token);
    case TokenKind::KW_TRUE:
        return std::make_unique<LiteralNode>(llvm::APInt{ 1, 1 },
            token.location);
    case TokenKind::KW_FALSE:
        return std::make_unique<LiteralNode>(llvm::APInt{ 1, 0 },
            token.location);
    case TokenKind::MINUS:
        return std::make_unique<UnaryNode>(token.kind, parseExpr(100),
            token.location);
    case TokenKind::LPAREN:
        {
            auto expr = parseExpr();
            if (current().kind != TokenKind::RPAREN)
            {
                errorExpected("')'");
            }
            next();
            return expr;
        }
    default:
        errorExpected("unary operator, identifier, or number");
        return std::make_unique<LiteralNode>(llvm::APInt{ 32, 0 },
            token.location);
    }
}

std::unique_ptr<ExprNode> VSLParser::parseLed(std::unique_ptr<ExprNode> left)
{
    const Token& token = current();
    TokenKind k = token.kind;
    switch (k)
    {
    case TokenKind::STAR:
    case TokenKind::SLASH:
    case TokenKind::PERCENT:
    case TokenKind::PLUS:
    case TokenKind::MINUS:
    case TokenKind::GREATER:
    case TokenKind::GREATER_EQUAL:
    case TokenKind::LESS:
    case TokenKind::LESS_EQUAL:
    case TokenKind::EQUALS:
        // left associative
        next();
        return std::make_unique<BinaryNode>(k, std::move(left),
            parseExpr(getLbp(token)), token.location);
    case TokenKind::ASSIGN:
        // right associative
        next();
        return std::make_unique<BinaryNode>(k, std::move(left),
            parseExpr(getLbp(token) - 1), token.location);
    case TokenKind::LPAREN:
        return parseCall(std::move(left));
    default:
        errorExpected("binary operator");
        return left;
    }
}

// lbp basically means operator precedence
// higher numbers mean higher precedence
int VSLParser::getLbp(const Token& token) const
{
    switch (token.kind)
    {
    case TokenKind::LPAREN:
        return 6;
    case TokenKind::STAR:
    case TokenKind::SLASH:
    case TokenKind::PERCENT:
        return 5;
    case TokenKind::PLUS:
    case TokenKind::MINUS:
        return 4;
    case TokenKind::GREATER:
    case TokenKind::GREATER_EQUAL:
    case TokenKind::LESS:
    case TokenKind::LESS_EQUAL:
        return 3;
    case TokenKind::EQUALS:
        return 2;
    case TokenKind::ASSIGN:
        return 1;
    default:
        return 0;
    }
}

// call -> callee lparen arg (comma arg)* rparen
// callee is passed as an argument from parseLed()
std::unique_ptr<CallNode> VSLParser::parseCall(std::unique_ptr<ExprNode> callee)
{
    const Token& lparen = current();
    if (lparen.kind != TokenKind::LPAREN)
    {
        errorExpected("'('");
    }
    next();
    std::vector<std::unique_ptr<ArgNode>> args;
    if (current().kind != TokenKind::RPAREN)
    {
        while (true)
        {
            args.emplace_back(parseCallArg());
            if (current().kind != TokenKind::COMMA)
            {
                break;
            }
            next();
        }
    }
    if (current().kind != TokenKind::RPAREN)
    {
        errorExpected("')'");
    }
    next();
    return std::make_unique<CallNode>(std::move(callee), std::move(args),
        lparen.location);
}

// arg -> identifier ':' expr
std::unique_ptr<ArgNode> VSLParser::parseCallArg()
{
    const Token& id = current();
    llvm::StringRef name;
    if (id.kind != TokenKind::IDENTIFIER)
    {
        errorExpected("identifier");
        name = "";
    }
    else
    {
        name = id.text;
    }
    next();
    if (current().kind != TokenKind::COLON)
    {
        errorExpected("':'");
    }
    else
    {
        next();
    }
    auto value = parseExpr();
    return std::make_unique<ArgNode>(name, std::move(value), id.location);
}

std::unique_ptr<LiteralNode> VSLParser::parseNumber(const Token& token)
{
    llvm::APInt value;
    if (token.text.getAsInteger(10, value))
    {
        errors << token.location << ": error: invalid integer '" <<
            token.text.str() << "'\n";
        value = 0;
        errored = true;
    }
    else if (value.getActiveBits() > 32)
    {
        errors << token.location << ": error: overflow detected in number '" <<
            token.text.str() << "'\n";
        errored = true;
    }
    value = value.zextOrTrunc(32);
    return std::make_unique<LiteralNode>(std::move(value), token.location);
}

// type -> 'Bool' | 'Int' | 'Void'
const Type* VSLParser::parseType()
{
    const Token& name = current();
    Type::Kind kind;
    switch (name.kind)
    {
    case TokenKind::KW_BOOL:
        kind = Type::BOOL;
        break;
    case TokenKind::KW_INT:
        kind = Type::INT;
        break;
    case TokenKind::KW_VOID:
        kind = Type::VOID;
        break;
    default:
        errorExpected("a type name");
        kind = Type::ERROR;
    }
    next();
    return vslContext.getSimpleType(kind);
}
