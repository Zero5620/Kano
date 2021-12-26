﻿#include "Common.h"
#include "CodeNode.h"
#include "Parser.h"
#include "Printer.h"
#include "Interp.h"

#include <stdlib.h>

void handle_assertion(const char *reason, const char *file, int line, const char *proc)
{
    fprintf(stderr, "%s. File: %s(%d)\n", reason, file, line);
    DebugTriggerbreakpoint();
}

String read_entire_file(const char *file)
{
    FILE *f = fopen(file, "rb");
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t *string = (uint8_t *)malloc(fsize + 1);
    fread(string, 1, fsize, f);
    fclose(f);

    string[fsize] = 0;
    return String(string, (int64_t)fsize);
}

static inline uint32_t murmur_32_scramble(uint32_t k)
{
    k *= 0xcc9e2d51;
    k = (k << 15) | (k >> 17);
    k *= 0x1b873593;
    return k;
}

uint32_t murmur3_32(const uint8_t *key, size_t len, uint32_t seed)
{
    uint32_t h = seed;
    uint32_t k;
    /* Read in groups of 4. */
    for (size_t i = len >> 2; i; i--)
    {
        // Here is a source of differing results across endiannesses.
        // A swap here has no effects on hash properties though.
        memcpy(&k, key, sizeof(uint32_t));
        key += sizeof(uint32_t);
        h ^= murmur_32_scramble(k);
        h = (h << 13) | (h >> 19);
        h = h * 5 + 0xe6546b64;
    }
    /* Read the rest. */
    k = 0;
    for (size_t i = len & 3; i; i--)
    {
        k <<= 8;
        k |= key[i - 1];
    }
    // A swap is *not* necessary here because the preceding loop already
    // places the low bytes in the low places according to whatever endianness
    // we use. Swaps only apply when the memory is copied in a chunk.
    h ^= murmur_32_scramble(k);
    /* Finalize. */
    h ^= len;
    h ^= h >> 16;
    h *= 0x85ebca6b;
    h ^= h >> 13;
    h *= 0xc2b2ae35;
    h ^= h >> 16;
    return h;
}

//
//
//

Unary_Operator_Kind token_to_unary_operator(Token_Kind kind)
{
    switch (kind)
    {
    case TOKEN_KIND_PLUS:
        return UNARY_OPERATOR_PLUS;
    case TOKEN_KIND_MINUS:
        return UNARY_OPERATOR_MINUS;
    case TOKEN_KIND_BITWISE_NOT:
        return UNARY_OPERATOR_BITWISE_NOT;
    case TOKEN_KIND_LOGICAL_NOT:
        return UNARY_OPERATOR_LOGICAL_NOT;
    case TOKEN_KIND_ASTERISK:
        return UNARY_OPERATOR_POINTER_TO;
    case TOKEN_KIND_DEREFERENCE:
        return UNARY_OPERATOR_DEREFERENCE;
        NoDefaultCase();
    }

    Unreachable();
    return _UNARY_OPERATOR_COUNT;
}

Binary_Operator_Kind token_to_binary_operator(Token_Kind kind)
{
    switch (kind)
    {
    case TOKEN_KIND_PLUS:
        return BINARY_OPERATOR_ADDITION;
    case TOKEN_KIND_MINUS:
        return BINARY_OPERATOR_SUBTRACTION;
    case TOKEN_KIND_ASTERISK:
        return BINARY_OPERATOR_MULTIPLICATION;
    case TOKEN_KIND_DIVISION:
        return BINARY_OPERATOR_DIVISION;
    case TOKEN_KIND_REMAINDER:
        return BINARY_OPERATOR_REMAINDER;
    case TOKEN_KIND_BITWISE_SHIFT_RIGHT:
        return BINARY_OPERATOR_BITWISE_SHIFT_RIGHT;
    case TOKEN_KIND_BITWISE_SHIFT_LEFT:
        return BINARY_OPERATOR_BITWISE_SHIFT_LEFT;
    case TOKEN_KIND_BITWISE_AND:
        return BINARY_OPERATOR_BITWISE_AND;
    case TOKEN_KIND_BITWISE_XOR:
        return BINARY_OPERATOR_BITWISE_XOR;
    case TOKEN_KIND_BITWISE_OR:
        return BINARY_OPERATOR_BITWISE_OR;
    case TOKEN_KIND_RELATIONAL_GREATER:
        return BINARY_OPERATOR_RELATIONAL_GREATER;
    case TOKEN_KIND_RELATIONAL_LESS:
        return BINARY_OPERATOR_RELATIONAL_LESS;
    case TOKEN_KIND_RELATIONAL_GREATER_EQUAL:
        return BINARY_OPERATOR_RELATIONAL_GREATER_EQUAL;
    case TOKEN_KIND_RELATIONAL_LESS_EQUAL:
        return BINARY_OPERATOR_RELATIONAL_LESS_EQUAL;
    case TOKEN_KIND_COMPARE_EQUAL:
        return BINARY_OPERATOR_COMPARE_EQUAL;
    case TOKEN_KIND_COMPARE_NOT_EQUAL:
        return BINARY_OPERATOR_COMPARE_NOT_EQUAL;
    case TOKEN_KIND_COMPOUND_PLUS:
        return BINARY_OPERATOR_COMPOUND_ADDITION;
    case TOKEN_KIND_COMPOUND_MINUS:
        return BINARY_OPERATOR_COMPOUND_SUBTRACTION;
    case TOKEN_KIND_COMPOUND_MULTIPLY:
        return BINARY_OPERATOR_COMPOUND_MULTIPLICATION;
    case TOKEN_KIND_COMPOUND_DIVIDE:
        return BINARY_OPERATOR_COMPOUND_DIVISION;
    case TOKEN_KIND_COMPOUND_REMAINDER:
        return BINARY_OPERATOR_COMPOUND_REMAINDER;
    case TOKEN_KIND_COMPOUND_BITWISE_SHIFT_RIGHT:
        return BINARY_OPERATOR_COMPOUND_BITWISE_SHIFT_RIGHT;
    case TOKEN_KIND_COMPOUND_BITWISE_SHIFT_LEFT:
        return BINARY_OPERATOR_COMPOUND_BITWISE_SHIFT_LEFT;
    case TOKEN_KIND_COMPOUND_BITWISE_AND:
        return BINARY_OPERATOR_COMPOUND_BITWISE_AND;
    case TOKEN_KIND_COMPOUND_BITWISE_XOR:
        return BINARY_OPERATOR_COMPOUND_BITWISE_XOR;
    case TOKEN_KIND_COMPOUND_BITWISE_OR:
        return BINARY_OPERATOR_COMPOUND_BITWISE_OR;
        NoDefaultCase();
    }

    Unreachable();
    return _BINARY_OPERATOR_COUNT;
}

static inline uint32_t next_power2(uint32_t n)
{
    n--;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
    n++;
    return n;
}

static Symbol *symbol_table_put(Symbol_Table *table, const Symbol &sym)
{
    String name      = sym.name;
    auto   hash      = murmur3_32(name.data, name.length, HASH_SEED) + 1;

    auto   pos       = hash & (SYMBOL_TABLE_BUCKET_COUNT - 1);
    auto   buk_index = pos >> SYMBOL_INDEX_SHIFT;

    for (auto bucket = &table->lookup.buckets[buk_index]; bucket; bucket = bucket->next)
    {
        uint32_t count = 0;
        for (auto index = pos & SYMBOL_INDEX_MASK; count < SYMBOL_INDEX_BUCKET_SIZE;
             ++count, index = (index + 1) & SYMBOL_INDEX_MASK)
        {
            auto found_hash = bucket->hash[index];
            if (found_hash == hash)
            {
                auto sym_index = bucket->index[index];
                return &table->buffer[sym_index];
            }
            else if (found_hash == 0)
            {
                uint32_t offset      = (uint32_t)table->buffer.count;
                bucket->hash[index]  = hash;
                bucket->index[index] = offset;
                table->buffer.add(sym);
                return &table->buffer[offset];
            }
        }

        if (!bucket->next)
        {
            bucket->next = new Symbol_Index;
        }
    }

    return 0;
}

static const Symbol *symbol_table_get(Symbol_Table *root_table, String name, bool recursive = true)
{
    auto hash      = murmur3_32(name.data, name.length, HASH_SEED) + 1;

    auto pos       = hash & (SYMBOL_TABLE_BUCKET_COUNT - 1);
    auto buk_index = pos >> SYMBOL_INDEX_SHIFT;

    for (auto table = root_table; table; table = table->parent)
    {
        for (auto bucket = &table->lookup.buckets[buk_index]; bucket; bucket = bucket->next)
        {
            uint32_t count = 0;
            for (auto index = pos & SYMBOL_INDEX_MASK; count < SYMBOL_INDEX_BUCKET_SIZE;
                 ++count, index = (index + 1) & SYMBOL_INDEX_MASK)
            {
                auto found_hash = bucket->hash[index];
                if (found_hash == hash)
                {
                    auto sym_index = bucket->index[index];
                    return &table->buffer[sym_index];
                }
            }
        }

        if (!recursive)
            break;
    }

    return 0;
}

struct Code_Type_Resolver
{
    Symbol_Table                     symbols;
    uint64_t                         vstack = 0;

    Array<Code_Type *>               return_stack;

    Bucket_Array<Unary_Operator, 8>  unary_operators[_UNARY_OPERATOR_COUNT];
    Bucket_Array<Binary_Operator, 8> binary_operators[_BINARY_OPERATOR_COUNT];
};

static Code_Node_Type_Cast *code_implicit_cast(Code_Node *node, Code_Type *to_type)
{
    bool cast_success = false;

    switch (to_type->kind)
    {
    case CODE_TYPE_INTEGER: {
        auto from_type = node->type->kind;
        cast_success   = (from_type == CODE_TYPE_BOOL);
    }
    break;

    case CODE_TYPE_REAL: {
        auto from_type = node->type->kind;
        cast_success   = (from_type == CODE_TYPE_INTEGER);
    }
    break;

    case CODE_TYPE_BOOL: {
        auto from_type = node->type->kind;
        cast_success   = (from_type == CODE_TYPE_INTEGER || from_type == CODE_TYPE_REAL || from_type == CODE_TYPE_REAL);
    }
    break;
    }

    if (cast_success)
    {
        auto cast   = new Code_Node_Type_Cast;
        cast->child = node;
        cast->type  = to_type;
        return cast;
    }

    return nullptr;
}

static bool code_type_are_same(Code_Type *a, Code_Type *b)
{
    if (a == b)
        return true;

    if (a->kind == b->kind && a->runtime_size == b->runtime_size)
    {
        switch (a->kind)
        {
        case CODE_TYPE_POINTER: {
            auto a_ptr = (Code_Type_Pointer *)a;
            auto b_ptr = (Code_Type_Pointer *)b;
            return code_type_are_same(a_ptr->base_type, b_ptr->base_type);
        }
        break;

        case CODE_TYPE_PROCEDURE: {
            auto a_proc = (Code_Type_Procedure *)a;
            auto b_proc = (Code_Type_Procedure *)b;

            if (a_proc->return_type && b_proc->return_type)
            {
                if (!code_type_are_same(a_proc->return_type, b_proc->return_type))
                    return false;
            }
            else if (!a_proc->return_type && b_proc->return_type)
                return false;
            else if (!b_proc->return_type && a_proc->return_type)
                return false;

            if (a_proc->argument_count != b_proc->argument_count)
                return false;

            auto a_args    = a_proc->arguments;
            auto b_args    = b_proc->arguments;
            auto arg_count = a_proc->argument_count;

            for (uint64_t arg_index = 0; arg_index < arg_count; ++arg_index)
            {
                if (!code_type_are_same(a_args[arg_index], b_args[arg_index]))
                    return false;
            }

            return true;
        }
        break;
        }
        return true;
    }

    return false;
}

Code_Node_Literal *code_resolve_literal(Code_Type_Resolver *resolver, Symbol_Table *symbols, Syntax_Node_Literal *root);
Code_Node_Address *code_resolve_identifier(Code_Type_Resolver *resolver, Symbol_Table *symbols,
                                           Syntax_Node_Identifier *root);
Code_Node *        code_resolve_expression(Code_Type_Resolver *resolver, Symbol_Table *symbols, Syntax_Node *root);
Code_Node_Unary_Operator * code_resolve_unary_operator(Code_Type_Resolver *resolver, Symbol_Table *symbols,
                                                       Syntax_Node_Unary_Operator *root);
Code_Node_Binary_Operator *code_resolve_binary_operator(Code_Type_Resolver *resolver, Symbol_Table *symbols,
                                                        Syntax_Node_Binary_Operator *root);
Code_Node_Expression *     code_resolve_root_expression(Code_Type_Resolver *resolver, Symbol_Table *symbols,
                                                        Syntax_Node_Expression *root);

Code_Node_Assignment *     code_resolve_assignment(Code_Type_Resolver *resolver, Symbol_Table *symbols,
                                                   Syntax_Node_Assignment *root);
Code_Type *           code_resolve_type(Code_Type_Resolver *resolver, Symbol_Table *symbols, Syntax_Node_Type *root);

Code_Node_Assignment *code_resolve_declaration(Code_Type_Resolver *resolver, Symbol_Table *symbols,
                                               Syntax_Node_Declaration *root, Code_Type **out_type = nullptr);
Code_Node_Statement * code_resolve_statement(Code_Type_Resolver *resolver, Symbol_Table *symbols,
                                             Syntax_Node_Statement *root);
Code_Node_Block *     code_resolve_block(Code_Type_Resolver *resolver, Symbol_Table *parent_symbols,
                                         Syntax_Node_Block *root);

//
//
//

Code_Node_Literal *code_resolve_literal(Code_Type_Resolver *resolver, Symbol_Table *symbols, Syntax_Node_Literal *root)
{
    auto node = new Code_Node_Literal;

    switch (root->value.kind)
    {
    case Literal::INTEGER: {
        auto symbol = symbol_table_get(symbols, "int");
        Assert(symbol->flags & SYMBOL_BIT_TYPE);

        node->type               = symbol->type;
        node->data.integer.value = root->value.data.integer;
    }
    break;

    case Literal::REAL: {
        auto symbol = symbol_table_get(symbols, "float");
        Assert(symbol->flags & SYMBOL_BIT_TYPE);

        node->type            = symbol->type;
        node->data.real.value = root->value.data.real;
    }
    break;

    case Literal::BOOL: {
        auto symbol = symbol_table_get(symbols, "bool");
        Assert(symbol->flags & SYMBOL_BIT_TYPE);

        node->type               = symbol->type;
        node->data.boolean.value = root->value.data.boolean;
    }
    break;

        NoDefaultCase();
    }

    return node;
}

Code_Node_Address *code_resolve_identifier(Code_Type_Resolver *resolver, Symbol_Table *symbols,
                                           Syntax_Node_Identifier *root)
{
    auto symbol = symbol_table_get(symbols, root->name);

    if (symbol)
    {
        auto address    = new Code_Node_Address;
        address->offset = symbol->address;
        address->flags  = symbol->flags;
        
        if (!(symbol->flags & SYMBOL_BIT_CONSTANT))
            address->flags |= SYMBOL_BIT_LVALUE;

        address->type = symbol->type;
        return address;
    }

    Unimplemented();
    return nullptr;
}

Code_Node_Return *code_resolve_return(Code_Type_Resolver *resolver, Symbol_Table *symbols, Syntax_Node_Return *root)
{
    auto node = new Code_Node_Return;

    if (root->expression)
    {
        node->expression = code_resolve_expression(resolver, symbols, root->expression);
        node->flags      = node->expression->flags;
        node->type       = node->expression->type;
    }

    if (resolver->return_stack.count)
    {
        auto return_type = *resolver->return_stack.last();
        if (!code_type_are_same(return_type, node->type))
        {
            Unimplemented();
        }
    }
    else
    {
        Unimplemented();
    }

    return node;
}

Code_Node_Procedure_Call *code_resolve_procedure_call(Code_Type_Resolver *resolver, Symbol_Table *symbols, Syntax_Node_Procedure_Call *root)
{
    auto procedure = code_resolve_root_expression(resolver, symbols, root->procedure);
    
    if (procedure->type->kind == CODE_TYPE_PROCEDURE)
    {
        auto proc = (Code_Type_Procedure *)procedure->type;

        if (proc->argument_count == root->parameter_count)
        {
            auto node       = new Code_Node_Procedure_Call;
            node->procedure = procedure;
            node->type      = proc->return_type;

            node->parameter_count = root->parameter_count;
            node->paraments       = new Code_Node_Expression *[node->parameter_count];

            uint32_t param_index = 0;
            for (auto param = root->parameters; param; param = param->next, ++param_index)
            {
                auto code_param = code_resolve_root_expression(resolver, symbols, param->expression);

                if (!code_type_are_same(proc->arguments[param_index], code_param->type))
                {
                    auto cast = code_implicit_cast(code_param->child, proc->arguments[param_index]);

                    if (cast)
                    {
                        code_param->child = cast;
                    }
                    else
                    {
                        Unimplemented();
                    }
                }

                node->paraments[param_index] = code_param;
            }

            node->stack_top = resolver->vstack;

            return node;
        }
        else
        {
            Unimplemented();
        }
    }

    Unimplemented();

    return nullptr;
}

Code_Node *code_resolve_expression(Code_Type_Resolver *resolver, Symbol_Table *symbols, Syntax_Node *root)
{
    switch (root->kind)
    {
    case SYNTAX_NODE_LITERAL:
        return code_resolve_literal(resolver, symbols, (Syntax_Node_Literal *)root);

    case SYNTAX_NODE_IDENTIFIER:
        return code_resolve_identifier(resolver, symbols, (Syntax_Node_Identifier *)root);

    case SYNTAX_NODE_UNARY_OPERATOR:
        return code_resolve_unary_operator(resolver, symbols, (Syntax_Node_Unary_Operator *)root);

    case SYNTAX_NODE_BINARY_OPERATOR:
        return code_resolve_binary_operator(resolver, symbols, (Syntax_Node_Binary_Operator *)root);

    case SYNTAX_NODE_ASSIGNMENT:
        return code_resolve_assignment(resolver, symbols, (Syntax_Node_Assignment *)root);

    case SYNTAX_NODE_RETURN:
        return code_resolve_return(resolver, symbols, (Syntax_Node_Return *)root);

    case SYNTAX_NODE_PROCEDURE_CALL:
        return code_resolve_procedure_call(resolver, symbols, (Syntax_Node_Procedure_Call *)root);

        NoDefaultCase();
    }
    return nullptr;
}

Code_Node_Unary_Operator *code_resolve_unary_operator(Code_Type_Resolver *resolver, Symbol_Table *symbols,
                                                      Syntax_Node_Unary_Operator *root)
{
    auto  child     = code_resolve_expression(resolver, symbols, root->child);

    auto  op_kind   = token_to_unary_operator(root->op);

    auto &operators = resolver->unary_operators[op_kind];
    ForBucketArray(bucket, operators)
    {
        ForBucket(index, bucket, operators)
        {
            auto op    = &bucket->data[index];

            bool found = false;
            if (code_type_are_same(op->parameter, child->type))
            {
                found = true;
            }
            else
            {
                auto cast = code_implicit_cast(child, op->output);
                if (cast)
                {
                    child = cast;
                    found = true;
                }
            }

            if (found)
            {
                auto node     = new Code_Node_Unary_Operator;
                node->type    = op->output;
                node->child   = child;
                node->op_kind = op_kind;
                return node;
            }
        }
    }

    if (op_kind == UNARY_OPERATOR_POINTER_TO && (child->flags & SYMBOL_BIT_LVALUE))
    {
        auto node       = new Code_Node_Unary_Operator;
        auto type       = new Code_Type_Pointer;

        type->base_type = child->type;

        node->type      = type;
        node->child     = child;
        node->op_kind   = op_kind;

        return node;
    }

    else if (op_kind == UNARY_OPERATOR_DEREFERENCE && (child->type->kind == CODE_TYPE_POINTER))
    {
        auto node     = new Code_Node_Unary_Operator;
        auto type     = ((Code_Type_Pointer *)child->type)->base_type;

        node->type    = type;
        node->child   = child;
        node->op_kind = op_kind;

        return node;
    }

    Unimplemented();

    return nullptr;
}

Code_Node_Binary_Operator *code_resolve_binary_operator(Code_Type_Resolver *resolver, Symbol_Table *symbols,
                                                        Syntax_Node_Binary_Operator *root)
{
    auto  left      = code_resolve_expression(resolver, symbols, root->left);
    auto  right     = code_resolve_expression(resolver, symbols, root->right);

    auto  op_kind   = token_to_binary_operator(root->op);

    auto &operators = resolver->binary_operators[op_kind];

    ForBucketArray(bucket, operators)
    {
        ForBucket(index, bucket, operators)
        {
            auto op          = &bucket->data[index];

            bool left_match  = false;
            bool right_match = false;

            if (code_type_are_same(op->parameters[0], left->type))
            {
                left_match = true;
            }
            else
            {
                auto cast_left = code_implicit_cast(left, op->parameters[0]);
                if (cast_left)
                {
                    left       = cast_left;
                    left_match = true;
                }
            }

            if (code_type_are_same(op->parameters[1], right->type))
            {
                right_match = true;
            }
            else
            {
                auto cast_right = code_implicit_cast(right, op->parameters[1]);
                if (cast_right)
                {
                    right       = cast_right;
                    right_match = true;
                }
            }

            if (left_match && right_match && (!op->compound || (op->compound && (left->flags & SYMBOL_BIT_LVALUE))))
            {
                auto node     = new Code_Node_Binary_Operator;

                node->type    = op->output;
                node->left    = left;
                node->right   = right;
                node->flags   = left->flags | right->flags;
                node->op_kind = op_kind;

                return node;
            }
        }
    }

    Unimplemented();

    return nullptr;
}

Code_Node_Expression *code_resolve_root_expression(Code_Type_Resolver *resolver, Symbol_Table *symbols,
                                                   Syntax_Node_Expression *root)
{
    auto                  child      = code_resolve_expression(resolver, symbols, root->child);

    Code_Node_Expression *expression = new Code_Node_Expression;
    expression->child                = child;
    expression->flags                = child->flags;
    expression->type                 = child->type;

    return expression;
}

Code_Node_Assignment *code_resolve_assignment(Code_Type_Resolver *resolver, Symbol_Table *symbols,
                                              Syntax_Node_Assignment *root)
{
    auto destination = code_resolve_root_expression(resolver, symbols, root->left);
    auto value       = code_resolve_root_expression(resolver, symbols, root->right);

    if (destination->flags & SYMBOL_BIT_LVALUE)
    {
        if (!(destination->flags & SYMBOL_BIT_CONSTANT))
        {
            bool match = false;

            if (code_type_are_same(destination->type, value->type))
            {
                match = true;
            }
            else
            {
                auto cast = code_implicit_cast(value->child, destination->type);
                if (cast)
                {
                    value->child = cast;
                    match        = true;
                }
            }

            if (match)
            {
                auto node         = new Code_Node_Assignment;
                node->destination = destination;
                node->value       = value;
                node->type        = destination->type;
                return node;
            }
        }
    }

    Unimplemented();
    return nullptr;
}

Code_Type *code_resolve_type(Code_Type_Resolver *resolver, Symbol_Table *symbols, Syntax_Node_Type *root)
{
    Code_Type *type = new Code_Type;

    switch (root->token_type)
    {
    case TOKEN_KIND_INT: {
        auto symbol = symbol_table_get(symbols, "int");
        return symbol->type;
    }
    break;

    case TOKEN_KIND_FLOAT: {
        auto symbol = symbol_table_get(symbols, "float");
        return symbol->type;
    }
    break;

    case TOKEN_KIND_BOOL: {
        auto symbol = symbol_table_get(symbols, "bool");
        return symbol->type;
    }
    break;

    case TOKEN_KIND_ASTERISK: {
        auto type       = new Code_Type_Pointer;
        type->base_type = code_resolve_type(resolver, symbols, (Syntax_Node_Type *)root->type);
        return type;
    }
    break;

    case TOKEN_KIND_PROC: {
        auto node            = (Syntax_Node_Procedure_Prototype *)root->type;

        auto type            = new Code_Type_Procedure;

        type->argument_count = node->argument_count;
        type->arguments      = new Code_Type *[type->argument_count];

        uint64_t arg_index   = 0;
        for (auto arg = node->arguments_type; arg; arg = arg->next, ++arg_index)
        {
            type->arguments[arg_index] = code_resolve_type(resolver, symbols, arg->type);
        }

        if (node->return_type)
        {
            type->return_type = code_resolve_type(resolver, symbols, node->return_type);
        }

        return type;
    }
    break;

    default: {
        Unimplemented();
    }
    }

    return type;
}

Code_Node_Assignment *code_resolve_declaration(Code_Type_Resolver *resolver, Symbol_Table *symbols,
                                               Syntax_Node_Declaration *root, Code_Type **out_type)
{
    String sym_name = root->identifier;

    Assert(root->type || root->initializer);

    if (!symbol_table_get(symbols, sym_name, false))
    {
        Symbol *symbol = nullptr;

        {
            Symbol input;
            input.name     = sym_name;
            input.type     = root->type ? code_resolve_type(resolver, symbols, root->type) : nullptr;
            input.flags    = root->flags;
            input.location = root->location;

            symbol         = symbol_table_put(symbols, input);
        }

        if (root->flags & SYMBOL_BIT_CONSTANT && !root->initializer)
        {
            // Error: Constanst expression must be initialized
            Unimplemented();
        }

        bool                  infer_type       = symbol->type == nullptr;

        Code_Node_Block *     procedure_body   = nullptr;
        Code_Node_Expression *expression       = nullptr;

        Code_Type *           type             = nullptr;

        auto                  ResolveProcedure = 
            [&resolver, &type, &procedure_body, symbols, symbol](Syntax_Node_Procedure *proc) {
            auto proc_type            = new Code_Type_Procedure;

            proc_type->argument_count = proc->argument_count;
            proc_type->arguments      = new Code_Type *[proc_type->argument_count];

            if (proc->return_type)
            {
                proc_type->return_type = code_resolve_type(resolver, &resolver->symbols, proc->return_type);
            }

            auto proc_symbols    = new Symbol_Table;
            proc_symbols->parent = symbols;

            auto stack_top = resolver->vstack;

            if (proc_type->return_type)
                resolver->vstack = proc_type->return_type->runtime_size;
            else
                resolver->vstack = 0;

            uint64_t arg_index   = 0;
            for (auto arg = proc->arguments; arg; arg = arg->next, ++arg_index)
            {
                auto assign = code_resolve_declaration(resolver, proc_symbols, arg->declaration,
                                                       &proc_type->arguments[arg_index]);
                Assert(assign == nullptr);
            }

            type = proc_type;
            if (!symbol->type)
                symbol->type = proc_type;

            resolver->return_stack.add(proc_type->return_type);
            procedure_body = code_resolve_block(resolver, proc_symbols, proc->body);
            resolver->return_stack.count -= 1;

            resolver->vstack = stack_top;
        };

        if (infer_type)
        {
            Assert(root->initializer);

            if (root->initializer->kind == SYNTAX_NODE_PROCEDURE)
            {
                ResolveProcedure((Syntax_Node_Procedure *)root->initializer);
            }
            else
            {
                Assert(root->initializer->kind == SYNTAX_NODE_EXPRESSION);
                expression =
                    code_resolve_root_expression(resolver, symbols, (Syntax_Node_Expression *)root->initializer);
                type = expression->type;
            }
        }
        else if (root->initializer)
        {
            if (root->initializer->kind == SYNTAX_NODE_PROCEDURE)
            {
                ResolveProcedure((Syntax_Node_Procedure *)root->initializer);
            }
            else
            {
                Assert(root->initializer->kind == SYNTAX_NODE_EXPRESSION);
                expression =
                    code_resolve_root_expression(resolver, symbols, (Syntax_Node_Expression *)root->initializer);
                type = expression->type;
            }
        }

        if (!infer_type && root->initializer)
        {
            if (!code_type_are_same(symbol->type, type))
            {
                if (expression)
                {
                    auto cast = code_implicit_cast(expression->child, symbol->type);
                    if (cast)
                    {
                        expression->child = cast;
                    }
                    else
                    {
                        Unimplemented();
                    }
                }
                else
                {
                    Unimplemented();
                }
            }
        }

        if (!symbol->type)
            symbol->type = type;

        if (out_type)
            *out_type = symbol->type;

        if (symbol->type->kind != CODE_TYPE_PROCEDURE)
        {
            uint64_t size    = symbol->type->runtime_size;
            resolver->vstack = AlignPower2Up(resolver->vstack, size);
            symbol->address  = (uint8_t *)resolver->vstack;
            resolver->vstack += size;

            if (root->initializer)
            {
                auto address    = new Code_Node_Address;
                address->offset = symbol->address;
                address->flags  = symbol->flags;
                address->flags |= SYMBOL_BIT_LVALUE;
                address->type           = symbol->type;

                auto destination        = new Code_Node_Expression;
                destination->child      = address;
                destination->flags      = address->flags;
                destination->type       = address->type;

                auto assignment         = new Code_Node_Assignment;
                assignment->type        = destination->type;
                assignment->destination = destination;
                assignment->value       = expression;
                assignment->flags |= destination->flags;

                return assignment;
            }

            return nullptr;
        }
        else
        {
            if (root->flags & SYMBOL_BIT_CONSTANT)
            {
                symbol->address = (uint8_t *)procedure_body;
            }
            else
            {
                uint64_t size    = symbol->type->runtime_size;
                resolver->vstack = AlignPower2Up(resolver->vstack, size);
                symbol->address  = (uint8_t *)resolver->vstack;
                resolver->vstack += size;

                auto address    = new Code_Node_Address;
                address->offset = symbol->address;
                address->flags  = symbol->flags;
                address->flags |= SYMBOL_BIT_LVALUE;
                address->type           = symbol->type;

                auto destination        = new Code_Node_Expression;
                destination->child      = address;
                destination->flags      = address->flags;
                destination->type       = address->type;

                auto source             = new Code_Node_Address;
                source->type            = symbol->type;
                source->flags           = symbol->flags;
                source->offset          = (uint8_t *)procedure_body;

                auto value              = new Code_Node_Expression;
                value->flags            = source->flags;
                value->type             = source->type;
                value->child            = source;

                auto assignment         = new Code_Node_Assignment;
                assignment->type        = destination->type;
                assignment->destination = destination;
                assignment->value       = value;
                assignment->flags |= destination->flags;

                return assignment;
            }

            return nullptr;
        }
    }

    // Already defined in this scope previously
    Unimplemented();

    return nullptr;
}

Code_Node_Statement *code_resolve_statement(Code_Type_Resolver *resolver, Symbol_Table *symbols,
                                            Syntax_Node_Statement *root)
{
    auto node = root->node;

    switch (node->kind)
    {
    case SYNTAX_NODE_EXPRESSION: {
        auto expression = code_resolve_root_expression(resolver, symbols, (Syntax_Node_Expression *)node);
        Code_Node_Statement *statement = new Code_Node_Statement;
        statement->node                = expression;
        statement->type                = expression->type;
        return statement;
    }
    break;

    case SYNTAX_NODE_IF: {
        auto if_node   = (Syntax_Node_If *)node;

        auto condition = code_resolve_root_expression(resolver, symbols, if_node->condition);

        auto boolean   = symbol_table_get(symbols, "bool");
        if (!code_type_are_same(condition->child->type, boolean->type))
        {
            auto cast = code_implicit_cast(condition->child, boolean->type);
            if (cast)
            {
                condition->child = cast;
            }
            else
            {
                Unimplemented();
            }
        }

        auto if_code            = new Code_Node_If;
        if_code->condition      = condition;
        if_code->true_statement = code_resolve_statement(resolver, symbols, if_node->true_statement);

        if (if_node->false_statement)
        {
            if_code->false_statement = code_resolve_statement(resolver, symbols, if_node->false_statement);
        }

        Code_Node_Statement *statement = new Code_Node_Statement;
        statement->node                = if_code;
        return statement;
    }
    break;

    case SYNTAX_NODE_FOR: {
        auto for_node            = (Syntax_Node_For *)node;

        auto for_code            = new Code_Node_For;
        for_code->symbols.parent = symbols;

        auto stack_top           = resolver->vstack;

        for_code->initialization = code_resolve_statement(resolver, &for_code->symbols, for_node->initialization);

        auto condition           = code_resolve_root_expression(resolver, &for_code->symbols, for_node->condition);

        auto boolean             = symbol_table_get(&for_code->symbols, "bool");
        if (!code_type_are_same(condition->child->type, boolean->type))
        {
            auto cast = code_implicit_cast(condition->child, boolean->type);
            if (cast)
            {
                condition->child = cast;
            }
            else
            {
                Unimplemented();
            }
        }

        for_code->condition = condition;
        for_code->increment = code_resolve_root_expression(resolver, &for_code->symbols, for_node->increment);
        for_code->body      = code_resolve_statement(resolver, &for_code->symbols, for_node->body);

        resolver->vstack    = stack_top;

        Code_Node_Statement *statement = new Code_Node_Statement;
        statement->node                = for_code;
        return statement;
    }
    break;

    case SYNTAX_NODE_WHILE: {
        auto while_node = (Syntax_Node_While *)node;

        auto condition  = code_resolve_root_expression(resolver, symbols, while_node->condition);

        auto boolean    = symbol_table_get(symbols, "bool");
        if (!code_type_are_same(condition->child->type, boolean->type))
        {
            auto cast = code_implicit_cast(condition->child, boolean->type);
            if (cast)
            {
                condition->child = cast;
            }
            else
            {
                Unimplemented();
            }
        }

        auto while_code                = new Code_Node_While;
        while_code->condition          = condition;
        while_code->body               = code_resolve_statement(resolver, symbols, while_node->body);

        Code_Node_Statement *statement = new Code_Node_Statement;
        statement->node                = while_code;
        return statement;
    }
    break;

    case SYNTAX_NODE_DO: {
        auto do_node    = (Syntax_Node_Do *)node;

        auto body       = code_resolve_statement(resolver, symbols, do_node->body);

        auto do_symbols = symbols;
        if (body->node->kind == CODE_NODE_BLOCK)
        {
            auto block = (Code_Node_Block *)body->node;
            do_symbols = &block->symbols;
        }

        auto condition = code_resolve_root_expression(resolver, do_symbols, do_node->condition);

        auto boolean   = symbol_table_get(do_symbols, "bool");
        if (!code_type_are_same(condition->child->type, boolean->type))
        {
            auto cast = code_implicit_cast(condition->child, boolean->type);
            if (cast)
            {
                condition->child = cast;
            }
            else
            {
                Unimplemented();
            }
        }

        auto do_code                   = new Code_Node_Do;
        do_code->body                  = body;
        do_code->condition             = condition;

        Code_Node_Statement *statement = new Code_Node_Statement;
        statement->node                = do_code;
        return statement;
    }
    break;

    case SYNTAX_NODE_DECLARATION: {
        auto initialization = code_resolve_declaration(resolver, symbols, (Syntax_Node_Declaration *)node);

        if (initialization)
        {
            Code_Node_Statement *statement = new Code_Node_Statement;
            statement->node                = initialization;
            return statement;
        }

        return nullptr;
    }
    break;

    case SYNTAX_NODE_STATEMENT: {
        // This happens when a block is added inside another block
        auto statement = (Syntax_Node_Statement *)node;
        return code_resolve_statement(resolver, symbols, statement);
    }
    break;

    case SYNTAX_NODE_BLOCK: {
        auto                 block     = code_resolve_block(resolver, symbols, (Syntax_Node_Block *)node);
        Code_Node_Statement *statement = new Code_Node_Statement;
        statement->node                = block;
        statement->type                = nullptr;
        return statement;
    }
    break;

        NoDefaultCase();
    }

    return nullptr;
}

Code_Node_Block *code_resolve_block(Code_Type_Resolver *resolver, Symbol_Table *parent_symbols, Syntax_Node_Block *root)
{
    Code_Node_Block *block = new Code_Node_Block;

    block->type            = nullptr;
    block->symbols.parent  = parent_symbols;

    Code_Node_Statement  statement_stub_head;
    Code_Node_Statement *parent_statement = &statement_stub_head;

    auto                 stack_top        = resolver->vstack;

    uint32_t             statement_count  = 0;
    for (auto statement = root->statement_head; statement; statement = statement->next)
    {
        auto code_statement = code_resolve_statement(resolver, &block->symbols, statement);
        if (code_statement)
        {
            parent_statement->next = code_statement;
            parent_statement       = code_statement;
            statement_count += 1;
        }
    }

    resolver->vstack       = stack_top;

    block->statement_head  = statement_stub_head.next;
    block->statement_count = statement_count;

    return block;
}

int main()
{
    String content = read_entire_file("Simple.kano");

    auto   builder = new String_Builder;

    Parser parser;
    parser_init(&parser, content, builder);

    auto node = parse_block(&parser);

    if (parser.error_count)
    {
        for (auto error = parser.error.first.next; error; error = error->next)
        {
            auto row     = error->location.start_row;
            auto column  = error->location.start_column;
            auto message = error->message.data;
            fprintf(stderr, "%zu,%zu: %s\n", row, column, message);
        }
        return 1;
    }

    {
        auto fp = fopen("syntax.txt", "wb");
        print_syntax(node, fp);
        fclose(fp);
    }

    Code_Type_Resolver resolver;

    Code_Type *        CompilerTypes[_CODE_TYPE_COUNT];

    {
        CompilerTypes[CODE_TYPE_NULL]    = new Code_Type;
        CompilerTypes[CODE_TYPE_INTEGER] = new Code_Type_Integer;
        CompilerTypes[CODE_TYPE_REAL]    = new Code_Type_Real;
        CompilerTypes[CODE_TYPE_BOOL]    = new Code_Type_Bool;
    }

    {
        Symbol sym;
        sym.name  = "int";
        sym.type  = CompilerTypes[CODE_TYPE_INTEGER];
        sym.flags = SYMBOL_BIT_CONSTANT | SYMBOL_BIT_TYPE;
        symbol_table_put(&resolver.symbols, sym);
    }

    {
        Symbol sym;
        sym.name  = "float";
        sym.type  = CompilerTypes[CODE_TYPE_REAL];
        sym.flags = SYMBOL_BIT_CONSTANT | SYMBOL_BIT_TYPE;
        symbol_table_put(&resolver.symbols, sym);
    }

    {
        Symbol sym;
        sym.name  = "bool";
        sym.type  = CompilerTypes[CODE_TYPE_BOOL];
        sym.flags = SYMBOL_BIT_CONSTANT | SYMBOL_BIT_TYPE;
        symbol_table_put(&resolver.symbols, sym);
    }

    {
        Unary_Operator unary_operator_int;
        unary_operator_int.parameter = CompilerTypes[CODE_TYPE_INTEGER];
        ;
        unary_operator_int.output = CompilerTypes[CODE_TYPE_INTEGER];
        ;
        resolver.unary_operators[UNARY_OPERATOR_PLUS].add(unary_operator_int);
        resolver.unary_operators[UNARY_OPERATOR_MINUS].add(unary_operator_int);
        resolver.unary_operators[UNARY_OPERATOR_BITWISE_NOT].add(unary_operator_int);
    }

    {
        Unary_Operator unary_operator_real;
        unary_operator_real.parameter = CompilerTypes[CODE_TYPE_REAL];
        ;
        unary_operator_real.output = CompilerTypes[CODE_TYPE_REAL];
        ;
        resolver.unary_operators[UNARY_OPERATOR_PLUS].add(unary_operator_real);
        resolver.unary_operators[UNARY_OPERATOR_MINUS].add(unary_operator_real);
    }

    {
        Unary_Operator unary_operator_bool;
        unary_operator_bool.parameter = CompilerTypes[CODE_TYPE_BOOL];
        unary_operator_bool.output    = CompilerTypes[CODE_TYPE_BOOL];
        resolver.unary_operators[UNARY_OPERATOR_LOGICAL_NOT].add(unary_operator_bool);
    }

    {
        Binary_Operator binary_operator_int;
        binary_operator_int.parameters[0] = CompilerTypes[CODE_TYPE_INTEGER];
        binary_operator_int.parameters[1] = CompilerTypes[CODE_TYPE_INTEGER];
        binary_operator_int.output        = CompilerTypes[CODE_TYPE_INTEGER];
        resolver.binary_operators[BINARY_OPERATOR_ADDITION].add(binary_operator_int);
        resolver.binary_operators[BINARY_OPERATOR_SUBTRACTION].add(binary_operator_int);
        resolver.binary_operators[BINARY_OPERATOR_MULTIPLICATION].add(binary_operator_int);
        resolver.binary_operators[BINARY_OPERATOR_DIVISION].add(binary_operator_int);
        resolver.binary_operators[BINARY_OPERATOR_REMAINDER].add(binary_operator_int);
        resolver.binary_operators[BINARY_OPERATOR_BITWISE_SHIFT_RIGHT].add(binary_operator_int);
        resolver.binary_operators[BINARY_OPERATOR_BITWISE_SHIFT_LEFT].add(binary_operator_int);
        resolver.binary_operators[BINARY_OPERATOR_BITWISE_AND].add(binary_operator_int);
        resolver.binary_operators[BINARY_OPERATOR_BITWISE_XOR].add(binary_operator_int);
        resolver.binary_operators[BINARY_OPERATOR_BITWISE_OR].add(binary_operator_int);
    }

    {
        Binary_Operator binary_operator_int;
        binary_operator_int.parameters[0] = CompilerTypes[CODE_TYPE_INTEGER];
        binary_operator_int.parameters[1] = CompilerTypes[CODE_TYPE_INTEGER];
        binary_operator_int.output        = CompilerTypes[CODE_TYPE_INTEGER];
        binary_operator_int.compound      = true;
        resolver.binary_operators[BINARY_OPERATOR_COMPOUND_ADDITION].add(binary_operator_int);
        resolver.binary_operators[BINARY_OPERATOR_COMPOUND_SUBTRACTION].add(binary_operator_int);
        resolver.binary_operators[BINARY_OPERATOR_COMPOUND_MULTIPLICATION].add(binary_operator_int);
        resolver.binary_operators[BINARY_OPERATOR_COMPOUND_DIVISION].add(binary_operator_int);
        resolver.binary_operators[BINARY_OPERATOR_COMPOUND_REMAINDER].add(binary_operator_int);

        resolver.binary_operators[BINARY_OPERATOR_COMPOUND_BITWISE_SHIFT_RIGHT].add(binary_operator_int);
        resolver.binary_operators[BINARY_OPERATOR_COMPOUND_BITWISE_SHIFT_LEFT].add(binary_operator_int);
        resolver.binary_operators[BINARY_OPERATOR_COMPOUND_BITWISE_AND].add(binary_operator_int);
        resolver.binary_operators[BINARY_OPERATOR_COMPOUND_BITWISE_XOR].add(binary_operator_int);
        resolver.binary_operators[BINARY_OPERATOR_COMPOUND_BITWISE_OR].add(binary_operator_int);
    }

    {
        Binary_Operator binary_operator_int;
        binary_operator_int.parameters[0] = CompilerTypes[CODE_TYPE_INTEGER];
        binary_operator_int.parameters[1] = CompilerTypes[CODE_TYPE_INTEGER];
        binary_operator_int.output        = CompilerTypes[CODE_TYPE_BOOL];
        resolver.binary_operators[BINARY_OPERATOR_RELATIONAL_GREATER].add(binary_operator_int);
        resolver.binary_operators[BINARY_OPERATOR_RELATIONAL_LESS].add(binary_operator_int);
        resolver.binary_operators[BINARY_OPERATOR_RELATIONAL_GREATER_EQUAL].add(binary_operator_int);
        resolver.binary_operators[BINARY_OPERATOR_RELATIONAL_LESS_EQUAL].add(binary_operator_int);
        resolver.binary_operators[BINARY_OPERATOR_COMPARE_EQUAL].add(binary_operator_int);
        resolver.binary_operators[BINARY_OPERATOR_COMPARE_NOT_EQUAL].add(binary_operator_int);
    }

    {
        Binary_Operator binary_operator_real;
        binary_operator_real.parameters[0] = CompilerTypes[CODE_TYPE_REAL];
        binary_operator_real.parameters[1] = CompilerTypes[CODE_TYPE_REAL];
        binary_operator_real.output        = CompilerTypes[CODE_TYPE_REAL];
        resolver.binary_operators[BINARY_OPERATOR_ADDITION].add(binary_operator_real);
        resolver.binary_operators[BINARY_OPERATOR_SUBTRACTION].add(binary_operator_real);
        resolver.binary_operators[BINARY_OPERATOR_MULTIPLICATION].add(binary_operator_real);
        resolver.binary_operators[BINARY_OPERATOR_DIVISION].add(binary_operator_real);
    }

    {
        Binary_Operator binary_operator_real;
        binary_operator_real.parameters[0] = CompilerTypes[CODE_TYPE_REAL];
        binary_operator_real.parameters[1] = CompilerTypes[CODE_TYPE_REAL];
        binary_operator_real.output        = CompilerTypes[CODE_TYPE_REAL];
        binary_operator_real.compound      = true;
        resolver.binary_operators[BINARY_OPERATOR_COMPOUND_ADDITION].add(binary_operator_real);
        resolver.binary_operators[BINARY_OPERATOR_COMPOUND_SUBTRACTION].add(binary_operator_real);
        resolver.binary_operators[BINARY_OPERATOR_COMPOUND_MULTIPLICATION].add(binary_operator_real);
        resolver.binary_operators[BINARY_OPERATOR_COMPOUND_DIVISION].add(binary_operator_real);
    }

    {
        Binary_Operator binary_operator_real;
        binary_operator_real.parameters[0] = CompilerTypes[CODE_TYPE_REAL];
        binary_operator_real.parameters[1] = CompilerTypes[CODE_TYPE_REAL];
        binary_operator_real.output        = CompilerTypes[CODE_TYPE_BOOL];
        resolver.binary_operators[BINARY_OPERATOR_RELATIONAL_GREATER].add(binary_operator_real);
        resolver.binary_operators[BINARY_OPERATOR_RELATIONAL_LESS].add(binary_operator_real);
        resolver.binary_operators[BINARY_OPERATOR_RELATIONAL_GREATER_EQUAL].add(binary_operator_real);
        resolver.binary_operators[BINARY_OPERATOR_RELATIONAL_LESS_EQUAL].add(binary_operator_real);
        resolver.binary_operators[BINARY_OPERATOR_COMPARE_EQUAL].add(binary_operator_real);
        resolver.binary_operators[BINARY_OPERATOR_COMPARE_NOT_EQUAL].add(binary_operator_real);
    }

    auto code = code_resolve_block(&resolver, &resolver.symbols, node);

    {
        auto fp = fopen("code.txt", "wb");
        print_code(code, fp);
        fclose(fp);
    }

    Interp interp;
    interp_init(&interp, 1024 * 1024 * 4);
	evaluate_node_block(code,&interp);

    return 0;
}
