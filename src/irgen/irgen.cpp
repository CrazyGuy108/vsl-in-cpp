#include "irgen/irgen.hpp"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include <limits>

IRGen::IRGen(llvm::Module& module, std::ostream& errors)
    : module{ module }, context{ module.getContext() }, builder{ context },
    result{ nullptr }, errors { errors }, errored{ false }
{
}

void IRGen::visit(ErrorNode& node)
{
    node.type = std::make_unique<SimpleType>(Type::ERROR);
    result = nullptr;
}

void IRGen::visit(EmptyNode& node)
{
    node.type = std::make_unique<SimpleType>(Type::ERROR);
    result = nullptr;
}

void IRGen::visit(BlockNode& node)
{
    // this just creates a new scope and visits all the statements inside
    node.type = std::make_unique<SimpleType>(Type::ERROR);
    scopeTree.enter();
    for (auto& statement : node.statements)
    {
        statement->accept(*this);
    }
    scopeTree.exit();
    result = nullptr;
}

void IRGen::visit(ConditionalNode& node)
{
    node.type = std::make_unique<SimpleType>(Type::ERROR);
    // make sure that it's not in the global scope.
    if (scopeTree.isGlobal())
    {
        errors << node.location <<
            ": error: top-level control flow statements are not allowed\n";
    }
    // visit the condition and make sure it's an Int
    scopeTree.enter();
    node.condition->accept(*this);
    Type::Kind k = node.condition->type->kind;
    if (k != Type::INT)
    {
        errors << node.condition->location <<
            ": error: cannot convert expression of type " <<
            Type::kindToString(k) << " to type Int\n";
        result = nullptr;
    }
    // create the condition
    llvm::Value* cond = builder.CreateICmpNE(result,
        llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), 0));
    // create the necessary basic blocks
    llvm::Function* f = builder.GetInsertBlock()->getParent();
    llvm::BasicBlock* thenBlock = llvm::BasicBlock::Create(context, "if.then");
    llvm::BasicBlock* elseBlock = llvm::BasicBlock::Create(context, "if.else");
    llvm::BasicBlock* mergeBlock = llvm::BasicBlock::Create(context, "if.end");
    // create the branch instruction
    builder.CreateCondBr(cond, thenBlock, elseBlock);
    // generate then block
    scopeTree.enter();
    f->getBasicBlockList().push_back(thenBlock);
    builder.SetInsertPoint(thenBlock);
    node.thenCase->accept(*this);
    builder.CreateBr(mergeBlock);
    scopeTree.exit();
    // generate else block
    scopeTree.enter();
    f->getBasicBlockList().push_back(elseBlock);
    builder.SetInsertPoint(elseBlock);
    node.elseCase->accept(*this);
    builder.CreateBr(mergeBlock);
    scopeTree.exit();
    // setup merge block for other code after the ConditionalNode
    f->getBasicBlockList().push_back(mergeBlock);
    builder.SetInsertPoint(mergeBlock);
    scopeTree.exit();
    result = nullptr;
}

void IRGen::visit(AssignmentNode& node)
{
    // make sure the type and value are valid and they match
    Node& value = *node.value;
    value.accept(*this);
    if (node.type->kind != Type::INT)
    {
        errors << node.location << ": error: " << node.type->toString() <<
            " is not a valid type for a variable\n";
    }
    else if (node.type->kind != value.type->kind)
    {
        errors << value.location <<
            ": error: mismatching types when initializing variable " <<
            node.name.str() << '\n';
    }
    // create the alloca instruction
    auto initialValue = result;
    llvm::Function* f = builder.GetInsertBlock()->getParent();
    auto alloca = createEntryAlloca(f, node.type->toLLVMType(context),
        node.name);
    // create the store instruction
    builder.CreateStore(initialValue, alloca);
    // add to current scope
    if (!scopeTree.set(node.name, { node.type.get(), alloca }))
    {
        errors << node.location << ": error: variable " <<
            node.name.str() << " was already defined\n";
    }
    result = nullptr;
}

void IRGen::visit(FunctionNode& node)
{
    // create the function
    auto& type = static_cast<FunctionType&>(*node.type);
    auto ft = type.toLLVMType(context);
    auto f = llvm::Function::Create(static_cast<llvm::FunctionType*>(ft),
        llvm::GlobalValue::ExternalLinkage, node.name, &module);
    // add to current scope
    scopeTree.set(node.name, { &type, f });
    // setup the parameters to be referenced as mutable variables
    auto bb = llvm::BasicBlock::Create(context, "entry", f);
    builder.SetInsertPoint(bb);
    scopeTree.enter(type.returnType.get());
    auto argIt = f->arg_begin();
    for (size_t i = 0; i < node.paramNames.size(); ++i, ++argIt)
    {
        llvm::StringRef paramName = node.paramNames[i].str;
        Type* paramType = type.params[i].get();
        llvm::Value* alloca = createEntryAlloca(f,
            paramType->toLLVMType(context), paramName);
        builder.CreateStore(argIt, alloca);
        scopeTree.set(paramName, { paramType, alloca });
    }
    // generate the body
    node.body->accept(*this);
    scopeTree.exit();
    result = nullptr;
}

void IRGen::visit(ReturnNode& node)
{
    // make sure the type of the value to return matches the actual return type
    node.value->accept(*this);
    node.type = node.value->type->clone();
    if (node.type->kind != scopeTree.getReturnType()->kind)
    {
        errors << node.location <<
            ": error: cannot convert expression of type " <<
            node.type->toString() << " to type " <<
            scopeTree.getReturnType()->toString() << '\n';
    }
    // create the return instruction
    result = builder.CreateRet(result);
}

void IRGen::visit(IdentExprNode& node)
{
    // lookup the name in the current scope
    Scope::Item i = scopeTree.get(node.name);
    if (i.type == nullptr || i.value == nullptr)
    {
        errors << node.location << ": error: unknown variable " <<
            node.name.str() << '\n';
        node.type = std::make_unique<SimpleType>(Type::ERROR);
        result = nullptr;
        return;
    }
    node.type = i.type->clone();
    // an identifier can reference either a variable or a function
    if (node.type->kind == Type::FUNCTION)
    {
        result = i.value;
    }
    else
    {
        result = builder.CreateLoad(i.value);
    }
}

void IRGen::visit(NumberExprNode& node)
{
    // create an LLVM integer
    node.type = std::make_unique<SimpleType>(Type::INT);
    // TODO: more than 1 type of integer
    result = llvm::ConstantInt::get(context, llvm::APInt{ 32,
            static_cast<uint64_t>(node.value) });
}

void IRGen::visit(UnaryExprNode& node)
{
    // validate the inner expression
    Node& expr = *node.expr;
    expr.accept(*this);
    Type::Kind k = expr.type->kind;
    switch (k)
    {
    // valid types go here
    case Type::INT:
        node.type = std::make_unique<SimpleType>(k);
        break;
    // errors are propagated down the expression tree
    default:
        errors << expr.location <<
            ": error: cannot apply unary operator " <<
            getTokenKindName(node.op) << " to type " <<
            expr.type->toString() << '\n';
        // fallthrough
    case Type::ERROR:
        node.type = std::make_unique<SimpleType>(Type::ERROR);
        result = nullptr;
        return;
    }
    // create instruction based on corresponding operator
    switch (node.op)
    {
    case TokenKind::MINUS:
        result = builder.CreateNeg(result);
        break;
    default:
        errors << node.location <<
            ": error: invalid unary operator for type Int\n";
        result = nullptr;
    }
}

void IRGen::visit(BinaryExprNode& node)
{
    // handle assignment operator as a special case
    if (node.op == TokenKind::ASSIGN)
    {
        // make sure that the lhs is an identifier
        if (node.left->kind != Node::ID_EXPR)
        {
            errors << node.location << ": error: lhs must be an identifier\n";
        }
        else
        {
            // lookup the identifier
            auto& id = static_cast<IdentExprNode&>(*node.left);
            Scope::Item i = scopeTree.get(id.name);
            if (i.type == nullptr || i.value == nullptr)
            {
                errors << id.location << ": error: unknown variable " <<
                    id.name.str() << '\n';
            }
            else
            {
                // make sure the types match up
                node.right->accept(*this);
                if (i.type->kind != node.right->type->kind)
                {
                    errors << node.right->location <<
                        ": error: cannot convert expression of type " <<
                        node.right->type->toString() << " to type " <<
                        i.type->toString() << '\n';
                }
                else
                {
                    // finally, create the store instruction
                    result = builder.CreateStore(result, i.value);
                    return;
                }
            }
        }
        node.type = std::make_unique<SimpleType>(Type::ERROR);
        result = nullptr;
        return;
    }
    // verify the left and right expressions
    node.left->accept(*this);
    llvm::Value* left = result;
    node.right->accept(*this);
    llvm::Value* right = result;
    if (left == nullptr || right == nullptr)
    {
        node.type = std::make_unique<SimpleType>(Type::ERROR);
        return;
    }
    if (node.left->type->kind != Type::INT)
    {
        errors << node.left->location <<
            ": error: cannot convert expression of type " <<
            Type::kindToString(node.left->type->kind) << " to type Int\n";
        result = nullptr;
    }
    if (node.right->type->kind != Type::INT)
    {
        errors << node.right->location <<
            ": error: cannot convert expression of type " <<
            Type::kindToString(node.right->type->kind) << " to type Int\n";
        result = nullptr;
    }
    if (result == nullptr)
    {
        node.type = std::make_unique<SimpleType>(Type::ERROR);
        return;
    }
    node.type = std::make_unique<SimpleType>(Type::INT);
    // create the corresponding instruction based on the operator
    switch (node.op)
    {
    case TokenKind::PLUS:
        result = builder.CreateAdd(left, right, "");
        break;
    case TokenKind::MINUS:
        result = builder.CreateSub(left, right, "");
        break;
    case TokenKind::STAR:
        result = builder.CreateMul(left, right, "");
        break;
    case TokenKind::SLASH:
        result = builder.CreateSDiv(left, right, "");
        break;
    case TokenKind::PERCENT:
        result = builder.CreateSRem(left, right, "");
        break;
    case TokenKind::EQUALS:
        result = builder.CreateICmpEQ(left, right, "");
        break;
    case TokenKind::GREATER:
        result = builder.CreateICmpSGT(left, right, "");
        break;
    case TokenKind::GREATER_EQUAL:
        result = builder.CreateICmpSGE(left, right, "");
        break;
    case TokenKind::LESS:
        result = builder.CreateICmpSLT(left, right, "");
        break;
    case TokenKind::LESS_EQUAL:
        result = builder.CreateICmpSLE(left, right, "");
        break;
    default:
        errors << node.location <<
            ": error: invalid binary operator on types Int and Int\n";
        result = nullptr;
    }
}

void IRGen::visit(CallExprNode& node)
{
    // make sure the callee is an actual function
    node.callee->accept(*this);
    Type& t = *node.callee->type;
    FunctionType& f = static_cast<FunctionType&>(t);
    if (t.kind != Type::FUNCTION)
    {
        errors << node.location << ": error: called object of type " <<
            t.toString() << " is not a function\n";
    }
    // make sure the right amount of arguments is used
    else if (f.params.size() != node.args.size())
    {
        errors << node.location <<
            ": error: mismatched number of arguments " <<
            node.args.size() << " versus parameters " <<
            f.params.size() << '\n';
    }
    else
    {
        llvm::Value* func = result;
        std::vector<llvm::Value*> llvmArgs;
        // verify each argument
        for (size_t i = 0; i < f.params.size(); ++i)
        {
            Type& param = *f.params[i];
            Node& arg = *node.args[i];
            arg.accept(*this);
            // check that the types match
            if (arg.type->kind != param.kind)
            {
                errors << arg.location << ": error: cannot convert type " <<
                    arg.type->toString() << " to type " << param.toString() <<
                    '\n';
            }
            else
            {
                llvmArgs.push_back(result);
            }
        }
        // create the call instruction if all args were successfully validated
        if (f.params.size() == llvmArgs.size())
        {
            node.type = f.returnType->clone();
            result = builder.CreateCall(func, llvmArgs);
            return;
        }
    }
    // if any sort of error occured, then this happens
    node.type = std::make_unique<SimpleType>(Type::ERROR);
    result = nullptr;
}

void IRGen::visit(ArgNode& node)
{
    // nothing special here
    node.value->accept(*this);
    node.type = node.value->type->clone();
}

std::string IRGen::getIR() const
{
    // llvm::Module only supports printing to a llvm::raw_ostream
    // thankfully there's llvm::raw_string_ostream that writes to an std::string
    std::string s;
    llvm::raw_string_ostream os{ s };
    os << module;
    os.flush();
    return s;
}

bool IRGen::hasError() const
{
    return errored;
}

llvm::Value* IRGen::createEntryAlloca(llvm::Function* f, llvm::Type* type,
    llvm::StringRef name)
{
    // construct a temporary llvm::IRBuilder to create the alloca instruction
    // i don't want to modify the builder field because it may be inserting at a
    //  different block
    // it might be better to save a temporary llvm::BasicBlock reference to
    //  restore the current block after creating the alloca but oh well
    auto& entry = f->getEntryBlock();
    llvm::IRBuilder<> b{ &entry, entry.begin() };
    return b.CreateAlloca(type, nullptr, name);
}
