#include "irgen/irgen.hpp"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include <limits>

IRGen::IRGen(VSLContext& vslContext, llvm::Module& module, std::ostream& errors)
    : vslContext{ vslContext }, module{ module },
    context{ module.getContext() }, builder{ context }, result{ nullptr },
    errors { errors }, errored{ false }
{
}

void IRGen::visitEmpty(EmptyNode& node)
{
    result = nullptr;
}

void IRGen::visitBlock(BlockNode& node)
{
    // create a new scope and visit all the statements inside
    scopeTree.enter();
    for (auto& statement : node.statements)
    {
        statement->accept(*this);
    }
    scopeTree.exit();
    result = nullptr;
}

void IRGen::visitIf(IfNode& node)
{
    // make sure that it's not in the global scope.
    if (scopeTree.isGlobal())
    {
        errors << node.location <<
            ": error: top-level control flow statements are not allowed\n";
    }
    // setup the condition
    scopeTree.enter();
    node.condition->accept(*this);
    const Type* type = node.condition->type;
    llvm::Value* cond;
    if (type == vslContext.getBoolType())
    {
        // take the value as is
        cond = result;
    }
    else if (type == vslContext.getIntType())
    {
        // convert the Int to a Bool
        cond = builder.CreateICmpNE(result,
            llvm::ConstantInt::get(context, llvm::APInt{ 32, 0 }));
    }
    else
    {
        // error, assume false
        errors << node.condition->location <<
            ": error: cannot convert expression of type " << type->toString() <<
            " to type Bool\n";
        errored = true;
        cond = llvm::ConstantInt::getFalse(context);
    }
    // create the necessary basic blocks
    auto& blocks = builder.GetInsertBlock()->getParent()->getBasicBlockList();
    auto* thenBlock = llvm::BasicBlock::Create(context, "if.then");
    auto* elseBlock = llvm::BasicBlock::Create(context, "if.else");
    auto* endBlock = llvm::BasicBlock::Create(context, "if.end");
    // create the branch instruction
    builder.CreateCondBr(cond, thenBlock, elseBlock);
    // generate then block
    scopeTree.enter();
    blocks.push_back(thenBlock);
    builder.SetInsertPoint(thenBlock);
    node.thenCase->accept(*this);
    builder.CreateBr(endBlock);
    scopeTree.exit();
    // generate else block
    scopeTree.enter();
    blocks.push_back(elseBlock);
    builder.SetInsertPoint(elseBlock);
    node.elseCase->accept(*this);
    builder.CreateBr(endBlock);
    scopeTree.exit();
    // setup end block for other code after the IfNode
    blocks.push_back(endBlock);
    builder.SetInsertPoint(endBlock);
    scopeTree.exit();
    result = nullptr;
}

void IRGen::visitVariable(VariableNode& node)
{
    // make sure the type and value are valid and they match
    ExprNode& value = *node.value;
    value.accept(*this);
    if (node.type != vslContext.getIntType())
    {
        errors << node.location << ": error: " << node.type->toString() <<
            " is not a valid type for a variable\n";
    }
    else if (node.type != value.type)
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
    if (!scopeTree.set(node.name, { node.type, alloca }))
    {
        errors << node.location << ": error: variable " <<
            node.name.str() << " was already defined\n";
    }
    result = nullptr;
}

void IRGen::visitFunction(FunctionNode& node)
{
    // fill in the function type
    std::vector<const Type*> paramTypes;
    paramTypes.resize(node.params.size());
    std::transform(node.params.begin(), node.params.end(), paramTypes.begin(),
        [](const auto& param)
        {
            return param->type;
        });
    const auto* vslType = vslContext.getFunctionType(std::move(paramTypes),
        node.returnType);
    // create the llvm function
    auto* ft = static_cast<llvm::FunctionType*>(vslType->toLLVMType(context));
    auto* f = llvm::Function::Create(ft, llvm::GlobalValue::ExternalLinkage,
        node.name, &module);
    // add to current scope
    scopeTree.set(node.name, { vslType, f });
    // setup the parameters to be referenced as mutable variables
    auto* bb = llvm::BasicBlock::Create(context, "entry", f);
    builder.SetInsertPoint(bb);
    scopeTree.enter(node.returnType);
    for (size_t i = 0; i < node.params.size(); ++i)
    {
        const ParamNode& param = *node.params[i];
        llvm::Value* alloca = createEntryAlloca(f, ft->params()[i], param.name);
        builder.CreateStore(&f->arg_begin()[i], alloca);
        scopeTree.set(param.name, { param.type, alloca });
    }
    // generate the body
    node.body->accept(*this);
    scopeTree.exit();
    result = nullptr;
}

void IRGen::visitParam(ParamNode& node)
{
}

void IRGen::visitReturn(ReturnNode& node)
{
    // make sure the type of the value to return matches the actual return type
    node.value->accept(*this);
    const Type* type = node.value->type;
    if (type != scopeTree.getReturnType())
    {
        errors << node.location <<
            ": error: cannot convert expression of type " << type->toString() <<
            " to type " << scopeTree.getReturnType()->toString() << '\n';
    }
    // create the return instruction
    result = builder.CreateRet(result);
}

void IRGen::visitIdent(IdentNode& node)
{
    // lookup the name in the current scope
    Scope::Item i = scopeTree.get(node.name);
    if (i.type == nullptr || i.value == nullptr)
    {
        errors << node.location << ": error: unknown variable " <<
            node.name.str() << '\n';
        node.type = vslContext.getErrorType();
        result = nullptr;
        return;
    }
    node.type = i.type;
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

void IRGen::visitLiteral(LiteralNode& node)
{
    // create an LLVM integer
    unsigned width = node.value.getBitWidth();
    switch (width)
    {
    case 1:
        node.type = vslContext.getBoolType();
        break;
    case 32:
        node.type = vslContext.getIntType();
        break;
    default:
        // should never happen
        errors << node.location << ": error: VSL does not support " << width <<
            "-bit integers\n";
        node.type = vslContext.getErrorType();
    }
    result = llvm::ConstantInt::get(context, node.value);
}

void IRGen::visitUnary(UnaryNode& node)
{
    // verify the contained expression
    node.expr->accept(*this);
    node.type = node.expr->type;
    const Type* type = node.type;
    llvm::Value* value = result;
    result = nullptr;
    // choose the appropriate operator to generate code for
    switch (node.op)
    {
    case TokenKind::MINUS:
        genNeg(type, value);
        break;
    default:
        // should never happen
        ;
    }
    if (result == nullptr)
    {
        errors << node.location << ": error: cannot apply unary operator " <<
            tokenKindName(node.op) << " to type " << type->toString() << '\n';
        errored = true;
    }
}

void IRGen::visitBinary(BinaryNode& node)
{
    // handle assignment operator as a special case
    if (node.op == TokenKind::ASSIGN)
    {
        genAssign(node);
        return;
    }
    // verify the left and right expressions
    node.left->accept(*this);
    llvm::Value* lhs = result;
    node.right->accept(*this);
    llvm::Value* rhs = result;
    result = nullptr;
    // make sure the types match
    if (node.left->type == node.right->type)
    {
        // choose the appropriate operator to generate code for
        const Type* type = node.left->type;
        switch (node.op)
        {
        case TokenKind::PLUS:
            node.type = node.left->type;
            genAdd(type, lhs, rhs);
            break;
        case TokenKind::MINUS:
            node.type = node.left->type;
            genSub(type, lhs, rhs);
            break;
        case TokenKind::STAR:
            node.type = node.left->type;
            genMul(type, lhs, rhs);
            break;
        case TokenKind::SLASH:
            node.type = node.left->type;
            genDiv(type, lhs, rhs);
            break;
        case TokenKind::PERCENT:
            node.type = node.left->type;
            genMod(type, lhs, rhs);
            break;
        case TokenKind::EQUAL:
            node.type = vslContext.getBoolType();
            genEQ(type, lhs, rhs);
            break;
        case TokenKind::NOT_EQUAL:
            node.type = vslContext.getBoolType();
            genNE(type, lhs, rhs);
            break;
        case TokenKind::GREATER:
            node.type = vslContext.getBoolType();
            genGT(type, lhs, rhs);
            break;
        case TokenKind::GREATER_EQUAL:
            node.type = vslContext.getBoolType();
            genGE(type, lhs, rhs);
            break;
        case TokenKind::LESS:
            node.type = vslContext.getBoolType();
            genLT(type, lhs, rhs);
            break;
        case TokenKind::LESS_EQUAL:
            node.type = vslContext.getBoolType();
            genLE(type, lhs, rhs);
            break;
        default:
            // should never happen
            node.type = node.left->type;
        }
    }
    else
    {
        // types mismatch, assume lhs' type
        node.type = node.left->type;
    }
    if (result == nullptr)
    {
        errors << node.location << ": error: cannot apply binary operator " <<
            tokenKindName(node.op) << " to types " <<
            node.left->type->toString() << " and " <<
            node.right->type->toString() << '\n';
        errored = true;
    }
}

void IRGen::visitCall(CallNode& node)
{
    // make sure the callee is an actual function
    node.callee->accept(*this);
    const auto* calleeType =
        static_cast<const FunctionType*>(node.callee->type);
    if (calleeType->kind != Type::FUNCTION)
    {
        errors << node.location << ": error: called object of type " <<
            calleeType->toString() << " is not a function\n";
    }
    // make sure the right amount of arguments is used
    else if (calleeType->params.size() != node.args.size())
    {
        errors << node.location <<
            ": error: mismatched number of arguments " <<
            node.args.size() << " versus parameters " <<
            calleeType->params.size() << '\n';
    }
    else
    {
        llvm::Value* func = result;
        std::vector<llvm::Value*> llvmArgs;
        // verify each argument
        for (size_t i = 0; i < calleeType->params.size(); ++i)
        {
            const Type* paramType = calleeType->params[i];
            ArgNode& arg = *node.args[i];
            arg.accept(*this);
            // check that the types match
            if (arg.value->type != paramType)
            {
                errors << arg.value->location <<
                    ": error: cannot convert type " <<
                    arg.value->type->toString() << " to type " <<
                    paramType->toString() << '\n';
            }
            else
            {
                llvmArgs.push_back(result);
            }
        }
        // create the call instruction if all args were successfully validated
        if (calleeType->params.size() == llvmArgs.size())
        {
            node.type = calleeType->returnType;
            result = builder.CreateCall(func, llvmArgs);
            return;
        }
    }
    // if any sort of error occured, then this happens
    node.type = vslContext.getErrorType();
    result = nullptr;
}

void IRGen::visitArg(ArgNode& node)
{
    node.value->accept(*this);
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

void IRGen::genNeg(const Type* type, llvm::Value* value)
{
    result = (type == vslContext.getIntType()) ?
        builder.CreateNeg(value, "neg") : nullptr;
}

void IRGen::genAssign(BinaryNode& node)
{
    ExprNode& lhs = *node.left;
    ExprNode& rhs = *node.right;
    rhs.accept(*this);
    node.type = vslContext.getVoidType();
    result = nullptr;
    // make sure that the lhs is an identifier
    if (lhs.kind == Node::IDENT)
    {
        // lookup the identifier
        auto& id = static_cast<IdentNode&>(lhs);
        Scope::Item i = scopeTree.get(id.name);
        if (i.type && i.value)
        {
            // make sure the types match up
            if (i.type == rhs.type)
            {
                // finally, create the store instruction
                builder.CreateStore(result, i.value);
                return;
            }
            else
            {
                errors << rhs.location <<
                    ": error: cannot convert expression of type " <<
                    rhs.type->toString() << " to type " << i.type->toString() <<
                    '\n';
            }
        }
        else
        {
            errors << id.location << ": error: unknown variable " <<
                id.name.str() << '\n';
        }
    }
    else
    {
        errors << lhs.location << ": error: lhs must be an identifier\n";
    }
    errored = true;
}

void IRGen::genAdd(const Type* type, llvm::Value* lhs, llvm::Value* rhs)
{
    result = (type == vslContext.getIntType()) ?
        builder.CreateAdd(lhs, rhs, "add") : nullptr;
}

void IRGen::genSub(const Type* type, llvm::Value* lhs, llvm::Value* rhs)
{
    result = (type == vslContext.getIntType()) ?
        builder.CreateSub(lhs, rhs, "sub") : nullptr;
}

void IRGen::genMul(const Type* type, llvm::Value* lhs, llvm::Value* rhs)
{
    result = (type == vslContext.getIntType()) ?
        builder.CreateMul(lhs, rhs, "mul") : nullptr;
}

void IRGen::genDiv(const Type* type, llvm::Value* lhs, llvm::Value* rhs)
{
    result = (type == vslContext.getIntType()) ?
        builder.CreateSDiv(lhs, rhs, "sdiv") : nullptr;
}

void IRGen::genMod(const Type* type, llvm::Value* lhs, llvm::Value* rhs)
{
    result = (type == vslContext.getIntType()) ?
        builder.CreateSRem(lhs, rhs, "srem") : nullptr;
}

void IRGen::genEQ(const Type* type, llvm::Value* lhs, llvm::Value* rhs)
{
    result = (type == vslContext.getIntType() ||
            type == vslContext.getBoolType()) ?
        builder.CreateICmpEQ(lhs, rhs, "cmp") : nullptr;
}

void IRGen::genNE(const Type* type, llvm::Value* lhs, llvm::Value* rhs)
{
    result = (type == vslContext.getIntType() ||
            type == vslContext.getBoolType()) ?
        builder.CreateICmpNE(lhs, rhs, "cmp") : nullptr;
}

void IRGen::genGT(const Type* type, llvm::Value* lhs, llvm::Value* rhs)
{
    result = (type == vslContext.getIntType()) ?
        builder.CreateICmpSGT(lhs, rhs, "cmp") : nullptr;
}

void IRGen::genGE(const Type* type, llvm::Value* lhs, llvm::Value* rhs)
{
    result = (type == vslContext.getIntType()) ?
        builder.CreateICmpSGE(lhs, rhs, "cmp") : nullptr;
}

void IRGen::genLT(const Type* type, llvm::Value* lhs, llvm::Value* rhs)
{
    result = (type == vslContext.getIntType()) ?
        builder.CreateICmpSLT(lhs, rhs, "cmp") : nullptr;
}

void IRGen::genLE(const Type* type, llvm::Value* lhs, llvm::Value* rhs)
{
    result = (type == vslContext.getIntType()) ?
        builder.CreateICmpSLE(lhs, rhs, "cmp") : nullptr;
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
