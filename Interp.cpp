#include "Interp.h"
#include "CodeNode.h"

#include "Resolver.h"

#include <stdlib.h>

struct Evaluation_Value
{
	struct Kano_Array
	{
		Kano_Int length;
		uint8_t *data;
	};
	
	union {
		Kano_Char  char_value;
		Kano_Int   int_value;
		Kano_Real  real_value;
		Kano_Bool  bool_value;
		uint8_t *  pointer_value;
		Kano_Array array_value;
		Code_Value_Procedure procedure_value;
	} imm;
	
	uint8_t *  from_address = nullptr;
	Code_Type *type         = nullptr;
};

template <typename T> T evaluation_value_get(Evaluation_Value &val)
{
	if (val.from_address)
		return *(T *)val.from_address;
	return *(T *)&val.imm;
}
#define EvaluationTypeValue(val, type) evaluation_value_get<type>(val)

template <typename T> T *evaluation_value_pointer(Evaluation_Value &val)
{
	if (val.from_address)
		return (T *)val.from_address;
	return (T *)&val.imm;
}
#define EvaluationTypePointer(val, type) evaluation_value_pointer<type>(val)

static inline uint64_t interp_push_into_stack(Interpreter *interp, Evaluation_Value var, uint64_t offset)
{
	memmove(interp->stack + interp->stack_top + offset, EvaluationTypePointer(var, void *), var.type->runtime_size);
	offset += var.type->runtime_size;
	return offset;
}

static inline uint64_t interp_push_into_stack_reduced(Interpreter *interp, Evaluation_Value var, uint64_t offset)
{
	offset -= var.type->runtime_size;
	memmove(interp->stack + interp->stack_top + offset, EvaluationTypePointer(var, void *), var.type->runtime_size);
	return offset;
}

//
//
//

static Evaluation_Value interp_eval_root_expression(Interpreter *interp, Code_Node_Expression *expression);

static Evaluation_Value interp_eval_address(Interpreter *interp, Code_Node_Address *node)
{
	if (node->subscript)
	{
		Assert(node->address == nullptr);

		auto expression = interp_eval_root_expression(interp, node->subscript->expression);
		auto subscript = interp_eval_root_expression(interp, node->subscript->subscript);

		Assert(subscript.type->kind == CODE_TYPE_INTEGER || subscript.type->kind == CODE_TYPE_CHARACTER);

		uint8_t *address = nullptr;

		auto expr_type = expression.type->kind;
		if (expr_type == CODE_TYPE_STATIC_ARRAY)
		{
			address = EvaluationTypePointer(expression, uint8_t);
		}
		else if (expr_type == CODE_TYPE_ARRAY_VIEW)
		{
			auto arr = EvaluationTypeValue(expression, Array_View<uint8_t>);
			address = arr.data;
		}
		else
		{
			Assert(expr_type == CODE_TYPE_STRUCT);
			auto arr = EvaluationTypeValue(expression, String);
			address = arr.data;
		}

		Assert(address);

		auto index = EvaluationTypeValue(subscript, Kano_Int);

		address += node->offset;
		address += node->type->runtime_size * index;

		Evaluation_Value type_value;
		type_value.type = node->type;
		type_value.from_address = address;
		return type_value;
	}
	else
	{
		auto offset = node->offset;
		auto memory = interp->stack;

		if (node->address)
		{
			offset += node->address->offset;
			auto address_kind = node->address->kind;
			
			if (address_kind == Symbol_Address::STACK)
			{
				offset += interp->stack_top;
			}
			else if (address_kind == Symbol_Address::GLOBAL)
			{
				memory = interp->global;
			}
			else if (address_kind == Symbol_Address::CODE)
			{
				Evaluation_Value type_value;
				type_value.type = node->type;
				type_value.imm.procedure_value.block = node->address->code;
				type_value.imm.procedure_value.ccall = nullptr;
				return type_value;
			}
			else if (address_kind == Symbol_Address::CCALL)
			{
				Evaluation_Value type_value;
				type_value.type = node->type;
				type_value.imm.procedure_value.ccall = node->address->ccall;
				type_value.imm.procedure_value.block = nullptr;
				return type_value;
			}
		}
		else
		{
			offset += interp->stack_top;
		}

		Evaluation_Value type_value;
		type_value.type = node->type;
		type_value.from_address = memory + offset;
		return type_value;
	}
}

static Evaluation_Value interp_eval_root_expression(Interpreter *interp, Code_Node_Expression *root);

static Evaluation_Value interp_eval_offset(Interpreter *interp, Code_Node_Offset *root)
{
	auto dest = interp_eval_root_expression(interp, root->expression);
	dest.from_address += root->offset;
	dest.type = root->type;
	return dest;
}

static Evaluation_Value interp_eval_type_cast(Interpreter *interp, Code_Node_Type_Cast *cast)
{
	auto value = interp_eval_root_expression(interp, cast->child);

	Evaluation_Value type_value;
	type_value.type = cast->type;
	
	switch (cast->type->kind)
	{
		case CODE_TYPE_REAL: {
			Assert(value.type->kind == CODE_TYPE_INTEGER || value.type->kind == CODE_TYPE_CHARACTER);
			type_value.imm.real_value = (Kano_Real)EvaluationTypeValue(value, Kano_Int);
		}
		break;
		
		case CODE_TYPE_INTEGER: {
			if (value.type->kind == CODE_TYPE_BOOL)
			{
				type_value.imm.int_value = EvaluationTypeValue(value, Kano_Bool);
			}
			else if (value.type->kind == CODE_TYPE_REAL)
			{
				type_value.imm.int_value = (Kano_Int)EvaluationTypeValue(value, Kano_Real);
			}
			else if (value.type->kind == CODE_TYPE_CHARACTER) 
			{
				type_value.imm.int_value = (Kano_Int)EvaluationTypeValue(value, Kano_Char);
			}
			else
			{
				Unreachable();
			}
		}
		break;
		
		case CODE_TYPE_BOOL: {
			if (value.type->kind == CODE_TYPE_INTEGER)
			{
				type_value.imm.bool_value = EvaluationTypeValue(value, Kano_Int) != 0;
			}
			else if (value.type->kind == CODE_TYPE_REAL)
			{
				type_value.imm.bool_value = EvaluationTypeValue(value, Kano_Real) != 0.0;
			}
			else if (value.type->kind == CODE_TYPE_CHARACTER) 
			{
				type_value.imm.bool_value = EvaluationTypeValue(value, Kano_Char) != 0;
			}
			else
			{
				Unreachable();
			}
		}
		break;
		
		case CODE_TYPE_POINTER: {
			Assert(value.type->kind == CODE_TYPE_POINTER);
			type_value.imm.pointer_value = EvaluationTypeValue(value, uint8_t *);
		}
		break;
		
		case CODE_TYPE_ARRAY_VIEW: {
			Assert(value.type->kind == CODE_TYPE_STATIC_ARRAY);
			type_value.imm.array_value.data   = EvaluationTypePointer(value, uint8_t);
			auto type                         = (Code_Type_Static_Array *)value.type;
			type_value.imm.array_value.length = type->element_count;
		}
		break;
		
		NoDefaultCase();
	}
	
	return type_value;
}

static Evaluation_Value interp_eval_expression(Interpreter *interp, Code_Node *root);

static Evaluation_Value interp_eval_return(Interpreter *interp, Code_Node_Return *node)
{
	if (node->expression)
	{
		auto result = interp_eval_expression(interp, node->expression);
		interp_push_into_stack(interp, result, 0);
		interp->return_count += 1;
		return result;
	}
	interp->return_count += 1;
	return Evaluation_Value{};
}

static void interp_eval_break(Interpreter *interp, Code_Node_Break *node)
{
	interp->break_count += 1;
}

static void interp_eval_continue(Interpreter *interp, Code_Node_Continue *node)
{
	interp->continue_count += 1;
}

static Evaluation_Value interp_eval_literal(Interpreter *interp, Code_Node_Literal *node)
{
	Evaluation_Value type_value;
	type_value.from_address = (uint8_t *)&node->data;
	type_value.type    = node->type;
	return type_value;
}

static Evaluation_Value interp_eval_expression(Interpreter *interp, Code_Node *root);

static Evaluation_Value interp_eval_unary_operator(Interpreter *interp, Code_Node_Unary_Operator *root)
{
	switch (root->op_kind)
	{
		case UNARY_OPERATOR_LOGICAL_NOT: {
			auto type_value = interp_eval_expression(interp, root->child);
			auto ref        = EvaluationTypePointer(type_value, bool);
			*ref            = !*ref;
			return type_value;
		}
		break;
		
		case UNARY_OPERATOR_BITWISE_NOT: {
			auto type_value = interp_eval_expression(interp, root->child);
			auto ref        = EvaluationTypePointer(type_value, int64_t);
			*ref            = ~*ref;
			return type_value;
		}
		break;
		
		case UNARY_OPERATOR_PLUS: {
			auto type_value = interp_eval_expression(interp, root->child);
			return type_value;
		}
		break;
		
		case UNARY_OPERATOR_MINUS: {
			auto value = interp_eval_expression(interp, root->child);
			
			if (value.type->kind == CODE_TYPE_INTEGER)
			{
				
				Evaluation_Value r;
				r.imm.int_value = -EvaluationTypeValue(value, Kano_Int);
				r.type          = root->type;
				return r;
			}
			else if (value.type->kind == CODE_TYPE_REAL)
			{
				Evaluation_Value r;
				r.imm.real_value = -EvaluationTypeValue(value, Kano_Real);
				r.type           = root->type;
				return r;
			}
			
			Unreachable();
		}
		break;
		
		case UNARY_OPERATOR_DEREFERENCE:  {
			Evaluation_Value pointer = interp_eval_expression(interp, root->child);

			Evaluation_Value type_value;
			type_value.type    = root->type;
			type_value.from_address = EvaluationTypeValue(pointer, uint8_t *);
			
			return type_value;
		}
		break;

		case UNARY_OPERATOR_POINTER_TO: {
			Assert(root->child->kind == CODE_NODE_ADDRESS);
			
			auto address = (Code_Node_Address *)root->child;

			auto pointer = interp_eval_address(interp, address);
			Assert(pointer.from_address);
			
			Evaluation_Value type_value;
			type_value.type = root->type;
			type_value.imm.pointer_value = pointer.from_address;
			
			return type_value;
		}
		break;

		NoDefaultCase();
	}
	
	Unreachable();
	return Evaluation_Value{};
}

typedef Evaluation_Value (*BinaryOperatorProc)(Evaluation_Value a, Evaluation_Value b, Code_Type *type);

typedef Kano_Int (*BinaryOperatorIntProc)(Kano_Int a, Kano_Int b);

static Evaluation_Value binary_add(Evaluation_Value a, Evaluation_Value b, Code_Type *type)
{
	Evaluation_Value r;
	r.type = type;
	
	switch (type->kind)
	{
		case CODE_TYPE_CHARACTER: {
			Assert(a.type->kind == CODE_TYPE_CHARACTER && b.type->kind == CODE_TYPE_CHARACTER);
			r.imm.char_value = EvaluationTypeValue(a, Kano_Char) + EvaluationTypeValue(b, Kano_Char);
			return r;
		}
		break;

		case CODE_TYPE_INTEGER: {
			Assert(a.type->kind == CODE_TYPE_INTEGER && b.type->kind == CODE_TYPE_INTEGER);
			r.imm.int_value = EvaluationTypeValue(a, Kano_Int) + EvaluationTypeValue(b, Kano_Int);
			return r;
		}
		break;
		
		case CODE_TYPE_REAL: {
			Assert(a.type->kind == CODE_TYPE_REAL && b.type->kind == CODE_TYPE_REAL);
			r.imm.real_value = EvaluationTypeValue(a, Kano_Real) + EvaluationTypeValue(b, Kano_Real);
			return r;
		}
		break;
		case CODE_TYPE_POINTER: {
			Assert(a.type->kind == CODE_TYPE_POINTER && b.type->kind == CODE_TYPE_INTEGER);
			r.imm.pointer_value = EvaluationTypeValue(a, uint8_t *) + EvaluationTypeValue(b, Kano_Int);
			return r;
		}
		break;
	}
	
	Unreachable();
	return r;
}

static Evaluation_Value binary_sub(Evaluation_Value a, Evaluation_Value b, Code_Type *type)
{
	Evaluation_Value r;
	r.type = type;
	
	switch (type->kind)
	{
		case CODE_TYPE_CHARACTER: {
			Assert(a.type->kind == CODE_TYPE_CHARACTER && b.type->kind == CODE_TYPE_CHARACTER);
			r.imm.char_value = EvaluationTypeValue(a, Kano_Char) - EvaluationTypeValue(b, Kano_Char);
			return r;
		}
							break;
		case CODE_TYPE_INTEGER: {
			Assert(a.type->kind == CODE_TYPE_INTEGER && b.type->kind == CODE_TYPE_INTEGER);
			r.imm.int_value = EvaluationTypeValue(a, Kano_Int) - EvaluationTypeValue(b, Kano_Int);
			return r;
		}
		break;
		
		case CODE_TYPE_REAL: {
			Assert(a.type->kind == CODE_TYPE_REAL && b.type->kind == CODE_TYPE_REAL);
			r.imm.real_value = EvaluationTypeValue(a, Kano_Real) - EvaluationTypeValue(b, Kano_Real);
			return r;
		}
		break;
		case CODE_TYPE_POINTER: {
			Assert(a.type->kind == CODE_TYPE_POINTER && b.type->kind == CODE_TYPE_INTEGER);
			r.imm.pointer_value = EvaluationTypeValue(a, uint8_t *) - EvaluationTypeValue(b, Kano_Int);
			return r;
		}
		break;
	}
	
	Unreachable();
	return r;
}

static Evaluation_Value binary_mul(Evaluation_Value a, Evaluation_Value b, Code_Type *type)
{
	Evaluation_Value r;
	r.type = type;
	
	switch (type->kind)
	{
		case CODE_TYPE_CHARACTER: {
			Assert(a.type->kind == CODE_TYPE_CHARACTER && b.type->kind == CODE_TYPE_CHARACTER);
			r.imm.char_value = EvaluationTypeValue(a, Kano_Char) * EvaluationTypeValue(b, Kano_Char);
			return r;
		}
		break;

		case CODE_TYPE_INTEGER: {
			Assert(a.type->kind == CODE_TYPE_INTEGER && b.type->kind == CODE_TYPE_INTEGER);
			r.imm.int_value = EvaluationTypeValue(a, Kano_Int) * EvaluationTypeValue(b, Kano_Int);
			return r;
		}
		break;
		
		case CODE_TYPE_REAL: {
			Assert(a.type->kind == CODE_TYPE_REAL && b.type->kind == CODE_TYPE_REAL);
			r.imm.real_value = EvaluationTypeValue(a, Kano_Real) * EvaluationTypeValue(b, Kano_Real);
			return r;
		}
		break;
	}
	
	Unreachable();
	return r;
}

static Evaluation_Value binary_div(Evaluation_Value a, Evaluation_Value b, Code_Type *type)
{
	Evaluation_Value r;
	r.type = type;
	
	switch (type->kind)
	{
		case CODE_TYPE_CHARACTER: {
			Assert(a.type->kind == CODE_TYPE_CHARACTER && b.type->kind == CODE_TYPE_CHARACTER);
			r.imm.char_value = EvaluationTypeValue(a, Kano_Char) / EvaluationTypeValue(b, Kano_Char);
			return r;
		}
		break;

		case CODE_TYPE_INTEGER: {
			Assert(a.type->kind == CODE_TYPE_INTEGER && b.type->kind == CODE_TYPE_INTEGER);
			r.imm.int_value = EvaluationTypeValue(a, Kano_Int) / EvaluationTypeValue(b, Kano_Int);
			return r;
		}
		break;
		
		case CODE_TYPE_REAL: {
			Assert(a.type->kind == CODE_TYPE_REAL && b.type->kind == CODE_TYPE_REAL);
			r.imm.real_value = EvaluationTypeValue(a, Kano_Real) / EvaluationTypeValue(b, Kano_Real);
			return r;
		}
		break;
	}
	
	Unreachable();
	return r;
}

static Evaluation_Value binary_mod(Evaluation_Value a, Evaluation_Value b, Code_Type *type)
{
	Evaluation_Value r;
	r.type = type;
	Assert((a.type->kind == CODE_TYPE_INTEGER && b.type->kind == CODE_TYPE_INTEGER) ||
		(a.type->kind == CODE_TYPE_CHARACTER && b.type->kind == CODE_TYPE_CHARACTER));

	if (a.type->kind == CODE_TYPE_CHARACTER)
		r.imm.char_value = EvaluationTypeValue(a, Kano_Char) % EvaluationTypeValue(b, Kano_Char);
	else
		r.imm.int_value = EvaluationTypeValue(a, Kano_Int) % EvaluationTypeValue(b, Kano_Int);
	return r;
}

static Evaluation_Value binary_rs(Evaluation_Value a, Evaluation_Value b, Code_Type *type)
{
	Evaluation_Value r;
	r.type = type;
	Assert((a.type->kind == CODE_TYPE_INTEGER && b.type->kind == CODE_TYPE_INTEGER) ||
		(a.type->kind == CODE_TYPE_CHARACTER && b.type->kind == CODE_TYPE_CHARACTER));

	if (a.type->kind == CODE_TYPE_CHARACTER)
		r.imm.char_value = EvaluationTypeValue(a, Kano_Char) >> EvaluationTypeValue(b, Kano_Char);
	else
		r.imm.int_value = EvaluationTypeValue(a, Kano_Int) >> EvaluationTypeValue(b, Kano_Int);
	return r;
}

static Evaluation_Value binary_ls(Evaluation_Value a, Evaluation_Value b, Code_Type *type)
{
	Evaluation_Value r;
	r.type = type;
	Assert((a.type->kind == CODE_TYPE_INTEGER && b.type->kind == CODE_TYPE_INTEGER) ||
		(a.type->kind == CODE_TYPE_CHARACTER && b.type->kind == CODE_TYPE_CHARACTER));

	if (a.type->kind == CODE_TYPE_CHARACTER)
		r.imm.char_value = EvaluationTypeValue(a, Kano_Char) << EvaluationTypeValue(b, Kano_Char);
	else
		r.imm.int_value = EvaluationTypeValue(a, Kano_Int) << EvaluationTypeValue(b, Kano_Int);
	return r;
}

static Evaluation_Value binary_and(Evaluation_Value a, Evaluation_Value b, Code_Type *type)
{
	Evaluation_Value r;
	r.type = type;
	Assert((a.type->kind == CODE_TYPE_INTEGER && b.type->kind == CODE_TYPE_INTEGER) ||
		(a.type->kind == CODE_TYPE_CHARACTER && b.type->kind == CODE_TYPE_CHARACTER));

	if (a.type->kind == CODE_TYPE_CHARACTER)
		r.imm.char_value = EvaluationTypeValue(a, Kano_Char) & EvaluationTypeValue(b, Kano_Char);
	else
		r.imm.int_value = EvaluationTypeValue(a, Kano_Int) & EvaluationTypeValue(b, Kano_Int);
	return r;
}

static Evaluation_Value binary_xor(Evaluation_Value a, Evaluation_Value b, Code_Type *type)
{
	Evaluation_Value r;
	r.type = type;
	Assert((a.type->kind == CODE_TYPE_INTEGER && b.type->kind == CODE_TYPE_INTEGER) ||
		(a.type->kind == CODE_TYPE_CHARACTER && b.type->kind == CODE_TYPE_CHARACTER));

	if (a.type->kind == CODE_TYPE_CHARACTER)
		r.imm.char_value = EvaluationTypeValue(a, Kano_Char) ^ EvaluationTypeValue(b, Kano_Char);
	else
		r.imm.int_value = EvaluationTypeValue(a, Kano_Int) ^ EvaluationTypeValue(b, Kano_Int);
	return r;
}

static Evaluation_Value binary_or(Evaluation_Value a, Evaluation_Value b, Code_Type *type)
{
	Evaluation_Value r;
	r.type = type;
	Assert((a.type->kind == CODE_TYPE_INTEGER && b.type->kind == CODE_TYPE_INTEGER) ||
		(a.type->kind == CODE_TYPE_CHARACTER && b.type->kind == CODE_TYPE_CHARACTER));

	if (a.type->kind == CODE_TYPE_CHARACTER)
		r.imm.char_value = EvaluationTypeValue(a, Kano_Char) | EvaluationTypeValue(b, Kano_Char);
	else
		r.imm.int_value = EvaluationTypeValue(a, Kano_Int) | EvaluationTypeValue(b, Kano_Int);
	return r;
}

static Evaluation_Value binary_gt(Evaluation_Value a, Evaluation_Value b, Code_Type *type)
{
	Evaluation_Value r;
	r.type = type;
	
	switch (a.type->kind)
	{
		case CODE_TYPE_CHARACTER: {
			Assert(b.type->kind == CODE_TYPE_CHARACTER);
			r.imm.bool_value = EvaluationTypeValue(a, Kano_Char) > EvaluationTypeValue(b, Kano_Char);
			return r;
		}
		break;

		case CODE_TYPE_INTEGER: {
			Assert(b.type->kind == CODE_TYPE_INTEGER);
			r.imm.bool_value = EvaluationTypeValue(a, Kano_Int) > EvaluationTypeValue(b, Kano_Int);
			return r;
		}
		break;
		
		case CODE_TYPE_REAL: {
			Assert(b.type->kind == CODE_TYPE_REAL);
			r.imm.bool_value = EvaluationTypeValue(a, Kano_Real) > EvaluationTypeValue(b, Kano_Real);
			return r;
		}
		break;
	}
	
	Unreachable();
	return r;
}

static Evaluation_Value binary_lt(Evaluation_Value a, Evaluation_Value b, Code_Type *type)
{
	Evaluation_Value r;
	r.type = type;
	
	switch (a.type->kind)
	{
		case CODE_TYPE_CHARACTER: {
			Assert(b.type->kind == CODE_TYPE_CHARACTER);
			r.imm.bool_value = EvaluationTypeValue(a, Kano_Char) < EvaluationTypeValue(b, Kano_Char);
			return r;
		}
		break;

		case CODE_TYPE_INTEGER: {
			Assert(b.type->kind == CODE_TYPE_INTEGER);
			r.imm.bool_value = EvaluationTypeValue(a, Kano_Int) < EvaluationTypeValue(b, Kano_Int);
			return r;
		}
		break;
		
		case CODE_TYPE_REAL: {
			Assert(b.type->kind == CODE_TYPE_REAL);
			r.imm.bool_value = EvaluationTypeValue(a, Kano_Real) < EvaluationTypeValue(b, Kano_Real);
			return r;
		}
		break;
	}
	
	Unreachable();
	return r;
}

static Evaluation_Value binary_ge(Evaluation_Value a, Evaluation_Value b, Code_Type *type)
{
	Evaluation_Value r;
	r.type = type;
	
	switch (a.type->kind)
	{
		case CODE_TYPE_CHARACTER: {
			Assert(b.type->kind == CODE_TYPE_CHARACTER);
			r.imm.bool_value = EvaluationTypeValue(a, Kano_Char) >= EvaluationTypeValue(b, Kano_Char);
			return r;
		}
		break;

		case CODE_TYPE_INTEGER: {
			Assert(b.type->kind == CODE_TYPE_INTEGER);
			r.imm.bool_value = EvaluationTypeValue(a, Kano_Int) >= EvaluationTypeValue(b, Kano_Int);
			return r;
		}
		break;
		
		case CODE_TYPE_REAL: {
			Assert(b.type->kind == CODE_TYPE_REAL);
			r.imm.bool_value = EvaluationTypeValue(a, Kano_Real) >= EvaluationTypeValue(b, Kano_Real);
			return r;
		}
		break;
	}
	
	Unreachable();
	return r;
}

static Evaluation_Value binary_le(Evaluation_Value a, Evaluation_Value b, Code_Type *type)
{
	Evaluation_Value r;
	r.type = type;
	
	switch (a.type->kind)
	{
		case CODE_TYPE_CHARACTER: {
			Assert(b.type->kind == CODE_TYPE_CHARACTER);
			r.imm.bool_value = EvaluationTypeValue(a, Kano_Char) <= EvaluationTypeValue(b, Kano_Char);
			return r;
		}
		break;

		case CODE_TYPE_INTEGER: {
			Assert(b.type->kind == CODE_TYPE_INTEGER);
			r.imm.bool_value = EvaluationTypeValue(a, Kano_Int) <= EvaluationTypeValue(b, Kano_Int);
			return r;
		}
		break;
		
		case CODE_TYPE_REAL: {
			Assert(b.type->kind == CODE_TYPE_REAL);
			r.imm.bool_value = EvaluationTypeValue(a, Kano_Real) <= EvaluationTypeValue(b, Kano_Real);
			return r;
		}
		break;
	}
	
	Unreachable();
	return r;
}

static Evaluation_Value binary_cmp(Evaluation_Value a, Evaluation_Value b, Code_Type *type)
{
	Evaluation_Value r;
	r.type = type;
	
	switch (a.type->kind)
	{
		case CODE_TYPE_CHARACTER: {
			Assert(b.type->kind == CODE_TYPE_CHARACTER);
			r.imm.bool_value = EvaluationTypeValue(a, Kano_Char) == EvaluationTypeValue(b, Kano_Char);
			return r;
		}
		break;

		case CODE_TYPE_INTEGER: {
			Assert(b.type->kind == CODE_TYPE_INTEGER);
			r.imm.bool_value = EvaluationTypeValue(a, Kano_Int) == EvaluationTypeValue(b, Kano_Int);
			return r;
		}
		break;
		
		case CODE_TYPE_REAL: {
			Assert(b.type->kind == CODE_TYPE_REAL);
			r.imm.bool_value = EvaluationTypeValue(a, Kano_Real) == EvaluationTypeValue(b, Kano_Real);
			return r;
		}
		break;
		
		case CODE_TYPE_BOOL: {
			Assert(b.type->kind == CODE_TYPE_BOOL);
			r.imm.bool_value = EvaluationTypeValue(a, Kano_Bool) == EvaluationTypeValue(b, Kano_Bool);
			return r;
		}
		break;

		case CODE_TYPE_POINTER: {
			r.imm.bool_value = (EvaluationTypeValue(a, uint8_t *) == EvaluationTypeValue(b, uint8_t *));
			return r;
		}
		break;
	}
	
	Unreachable();
	return r;
}

static Evaluation_Value binary_ncmp(Evaluation_Value a, Evaluation_Value b, Code_Type *type)
{
	Evaluation_Value r;
	r.type = type;
	
	switch (a.type->kind)
	{
		case CODE_TYPE_CHARACTER: {
			Assert(b.type->kind == CODE_TYPE_CHARACTER);
			r.imm.bool_value = EvaluationTypeValue(a, Kano_Char) != EvaluationTypeValue(b, Kano_Char);
			return r;
		}
		break;

		case CODE_TYPE_INTEGER: {
			Assert(b.type->kind == CODE_TYPE_INTEGER);
			r.imm.bool_value = EvaluationTypeValue(a, Kano_Int) != EvaluationTypeValue(b, Kano_Int);
			return r;
		}
		break;
		
		case CODE_TYPE_REAL: {
			Assert(b.type->kind == CODE_TYPE_REAL);
			r.imm.bool_value = EvaluationTypeValue(a, Kano_Real) != EvaluationTypeValue(b, Kano_Real);
			return r;
		}
		break;
		
		case CODE_TYPE_BOOL: {
			Assert(b.type->kind == CODE_TYPE_BOOL);
			r.imm.bool_value = EvaluationTypeValue(a, Kano_Bool) != EvaluationTypeValue(b, Kano_Bool);
			return r;
		}
		break;

		case CODE_TYPE_POINTER: {
			r.imm.bool_value = (EvaluationTypeValue(a, uint8_t *) != EvaluationTypeValue(b, uint8_t *));
			return r;
		}
		break;
	}
	
	Unreachable();
	return r;
}

static Evaluation_Value binary_cadd(Evaluation_Value a, Evaluation_Value b, Code_Type *type)
{
	a.type = type;
	
	switch (type->kind)
	{
		case CODE_TYPE_CHARACTER: {
			Assert(b.type->kind == CODE_TYPE_CHARACTER && b.type->kind == CODE_TYPE_CHARACTER);
			auto ref = EvaluationTypePointer(a, Kano_Char);
			*ref += EvaluationTypeValue(b, Kano_Char);
			return a;
		}
		break;

		case CODE_TYPE_INTEGER: {
			Assert(a.type->kind == CODE_TYPE_INTEGER && b.type->kind == CODE_TYPE_INTEGER);
			auto ref = EvaluationTypePointer(a, Kano_Int);
			*ref += EvaluationTypeValue(b, Kano_Int);
			return a;
		}
		break;
		
		case CODE_TYPE_REAL: {
			Assert(a.type->kind == CODE_TYPE_REAL && b.type->kind == CODE_TYPE_REAL);
			auto ref = EvaluationTypePointer(a, Kano_Real);
			*ref += EvaluationTypeValue(b, Kano_Real);
			return a;
		}
		break;
		
		case CODE_TYPE_POINTER: {
			Assert(a.type->kind == CODE_TYPE_POINTER && b.type->kind == CODE_TYPE_INTEGER);
			auto ref = EvaluationTypePointer(a, uint8_t *);
			*ref += EvaluationTypeValue(b, Kano_Int);
			return a;
		}
		break;
	}
	
	Unreachable();
	return Evaluation_Value{};
}

static Evaluation_Value binary_csub(Evaluation_Value a, Evaluation_Value b, Code_Type *type)
{
	a.type = type;
	
	switch (type->kind)
	{
		case CODE_TYPE_CHARACTER: {
			Assert(b.type->kind == CODE_TYPE_CHARACTER && b.type->kind == CODE_TYPE_CHARACTER);
			auto ref = EvaluationTypePointer(a, Kano_Char);
			*ref -= EvaluationTypeValue(b, Kano_Char);
			return a;
		}
		break;

		case CODE_TYPE_INTEGER: {
			Assert(a.type->kind == CODE_TYPE_INTEGER && b.type->kind == CODE_TYPE_INTEGER);
			auto ref = EvaluationTypePointer(a, Kano_Int);
			*ref -= EvaluationTypeValue(b, Kano_Int);
			return a;
		}
		break;
		
		case CODE_TYPE_REAL: {
			Assert(a.type->kind == CODE_TYPE_REAL && b.type->kind == CODE_TYPE_REAL);
			auto ref = EvaluationTypePointer(a, Kano_Real);
			*ref -= EvaluationTypeValue(b, Kano_Real);
			return a;
		}
		break;
		case CODE_TYPE_POINTER: {
			Assert(a.type->kind == CODE_TYPE_POINTER && b.type->kind == CODE_TYPE_INTEGER);
			auto ref = EvaluationTypePointer(a, uint8_t *);
			*ref -= EvaluationTypeValue(b, Kano_Int);
			return a;
		}
		break;
	}
	
	Unreachable();
	return Evaluation_Value{};
}

static Evaluation_Value binary_cmul(Evaluation_Value a, Evaluation_Value b, Code_Type *type)
{
	a.type = type;
	
	switch (type->kind)
	{
		case CODE_TYPE_CHARACTER: {
			Assert(b.type->kind == CODE_TYPE_CHARACTER && b.type->kind == CODE_TYPE_CHARACTER);
			auto ref = EvaluationTypePointer(a, Kano_Char);
			*ref *= EvaluationTypeValue(b, Kano_Char);
			return a;
		}
		break;

		case CODE_TYPE_INTEGER: {
			Assert(a.type->kind == CODE_TYPE_INTEGER && b.type->kind == CODE_TYPE_INTEGER);
			auto ref = EvaluationTypePointer(a, Kano_Int);
			*ref *= EvaluationTypeValue(b, Kano_Int);
			return a;
		}
		break;
		
		case CODE_TYPE_REAL: {
			Assert(a.type->kind == CODE_TYPE_REAL && b.type->kind == CODE_TYPE_REAL);
			auto ref = EvaluationTypePointer(a, Kano_Real);
			*ref *= EvaluationTypeValue(b, Kano_Real);
			return a;
		}
		break;
	}
	
	Unreachable();
	return Evaluation_Value{};
}

static Evaluation_Value binary_cdiv(Evaluation_Value a, Evaluation_Value b, Code_Type *type)
{
	a.type = type;
	
	switch (type->kind)
	{
		case CODE_TYPE_CHARACTER: {
			Assert(b.type->kind == CODE_TYPE_CHARACTER && b.type->kind == CODE_TYPE_CHARACTER);
			auto ref = EvaluationTypePointer(a, Kano_Char);
			*ref /= EvaluationTypeValue(b, Kano_Char);
			return a;
		}
		break;

		case CODE_TYPE_INTEGER: {
			Assert(a.type->kind == CODE_TYPE_INTEGER && b.type->kind == CODE_TYPE_INTEGER);
			auto ref = EvaluationTypePointer(a, Kano_Int);
			*ref /= EvaluationTypeValue(b, Kano_Int);
			return a;
		}
		break;
		
		case CODE_TYPE_REAL: {
			Assert(a.type->kind == CODE_TYPE_REAL && b.type->kind == CODE_TYPE_REAL);
			auto ref = EvaluationTypePointer(a, Kano_Real);
			*ref /= EvaluationTypeValue(b, Kano_Real);
			return a;
		}
		break;
	}
	
	Unreachable();
	return Evaluation_Value{};
}

static Evaluation_Value binary_cmod(Evaluation_Value a, Evaluation_Value b, Code_Type *type)
{
	a.type = type;
	Assert((a.type->kind == CODE_TYPE_INTEGER && b.type->kind == CODE_TYPE_INTEGER) ||
		(a.type->kind == CODE_TYPE_CHARACTER && b.type->kind == CODE_TYPE_CHARACTER));
	if (a.type->kind == CODE_TYPE_CHARACTER) {
		auto ref = EvaluationTypePointer(a, Kano_Char);
		*ref %= EvaluationTypeValue(b, Kano_Char);
	} else {
		auto ref = EvaluationTypePointer(a, Kano_Int);
		*ref %= EvaluationTypeValue(b, Kano_Int);
	}

	return a;
}

static Evaluation_Value binary_crs(Evaluation_Value a, Evaluation_Value b, Code_Type *type)
{
	a.type = type;
	Assert((a.type->kind == CODE_TYPE_INTEGER && b.type->kind == CODE_TYPE_INTEGER) ||
		(a.type->kind == CODE_TYPE_CHARACTER && b.type->kind == CODE_TYPE_CHARACTER));
	if (a.type->kind == CODE_TYPE_CHARACTER) {
		auto ref = EvaluationTypePointer(a, Kano_Char);
		*ref >>= EvaluationTypeValue(b, Kano_Char);
	}
	else {
		auto ref = EvaluationTypePointer(a, Kano_Int);
		*ref >>= EvaluationTypeValue(b, Kano_Int);
	}
	return a;
}

static Evaluation_Value binary_cls(Evaluation_Value a, Evaluation_Value b, Code_Type *type)
{
	a.type = type;
	Assert((a.type->kind == CODE_TYPE_INTEGER && b.type->kind == CODE_TYPE_INTEGER) ||
		(a.type->kind == CODE_TYPE_CHARACTER && b.type->kind == CODE_TYPE_CHARACTER));
	if (a.type->kind == CODE_TYPE_CHARACTER) {
		auto ref = EvaluationTypePointer(a, Kano_Char);
		*ref <<= EvaluationTypeValue(b, Kano_Char);
	}
	else {
		auto ref = EvaluationTypePointer(a, Kano_Int);
		*ref <<= EvaluationTypeValue(b, Kano_Int);
	}	return a;
}

static Evaluation_Value binary_cand(Evaluation_Value a, Evaluation_Value b, Code_Type *type)
{
	a.type = type;
	Assert((a.type->kind == CODE_TYPE_INTEGER && b.type->kind == CODE_TYPE_INTEGER) ||
		(a.type->kind == CODE_TYPE_CHARACTER && b.type->kind == CODE_TYPE_CHARACTER));
	if (a.type->kind == CODE_TYPE_CHARACTER) {
		auto ref = EvaluationTypePointer(a, Kano_Char);
		*ref &= EvaluationTypeValue(b, Kano_Char);
	}
	else {
		auto ref = EvaluationTypePointer(a, Kano_Int);
		*ref &= EvaluationTypeValue(b, Kano_Int);
	}	return a;
}

static Evaluation_Value binary_cxor(Evaluation_Value a, Evaluation_Value b, Code_Type *type)
{
	a.type = type;
	Assert((a.type->kind == CODE_TYPE_INTEGER && b.type->kind == CODE_TYPE_INTEGER) ||
		(a.type->kind == CODE_TYPE_CHARACTER && b.type->kind == CODE_TYPE_CHARACTER));
	if (a.type->kind == CODE_TYPE_CHARACTER) {
		auto ref = EvaluationTypePointer(a, Kano_Char);
		*ref ^= EvaluationTypeValue(b, Kano_Char);
	}
	else {
		auto ref = EvaluationTypePointer(a, Kano_Int);
		*ref ^= EvaluationTypeValue(b, Kano_Int);
	}	return a;
}

static Evaluation_Value binary_cor(Evaluation_Value a, Evaluation_Value b, Code_Type *type)
{
	a.type = type;
	Assert((a.type->kind == CODE_TYPE_INTEGER && b.type->kind == CODE_TYPE_INTEGER) ||
		(a.type->kind == CODE_TYPE_CHARACTER && b.type->kind == CODE_TYPE_CHARACTER));
	if (a.type->kind == CODE_TYPE_CHARACTER) {
		auto ref = EvaluationTypePointer(a, Kano_Char);
		*ref |= EvaluationTypeValue(b, Kano_Char);
	}
	else {
		auto ref = EvaluationTypePointer(a, Kano_Int);
		*ref |= EvaluationTypeValue(b, Kano_Int);
	}
	return a;
}

static Evaluation_Value binary_land(Evaluation_Value a, Evaluation_Value b, Code_Type *type)
{
	Assert(type->kind == CODE_TYPE_BOOL && a.type->kind == b.type->kind);
	Evaluation_Value r;
	r.type = type;
	r.from_address = nullptr;
	if (a.type->kind == CODE_TYPE_BOOL)
		r.imm.bool_value = (EvaluationTypeValue(a, Kano_Bool) && EvaluationTypeValue(b, Kano_Bool));
	else if (a.type->kind == CODE_TYPE_CHARACTER)
		r.imm.bool_value = (EvaluationTypeValue(a, Kano_Char) && EvaluationTypeValue(b, Kano_Char));
	else if (a.type->kind == CODE_TYPE_INTEGER)
		r.imm.bool_value = (EvaluationTypeValue(a, Kano_Int) && EvaluationTypeValue(b, Kano_Int));
	else if (a.type->kind == CODE_TYPE_POINTER)
		r.imm.bool_value = (EvaluationTypeValue(a, void *) && EvaluationTypeValue(b, void *));
	else if (a.type->kind == CODE_TYPE_REAL)
		r.imm.bool_value = (EvaluationTypeValue(a, Kano_Real) && EvaluationTypeValue(b, Kano_Real));
	else
		Unreachable();
	return r;
}

static Evaluation_Value binary_lor(Evaluation_Value a, Evaluation_Value b, Code_Type *type)
{
	Assert(type->kind == CODE_TYPE_BOOL && a.type->kind == b.type->kind);
	Evaluation_Value r;
	r.type = type;
	r.from_address = nullptr;
	if (a.type->kind == CODE_TYPE_BOOL)
		r.imm.bool_value = (EvaluationTypeValue(a, Kano_Bool) || EvaluationTypeValue(b, Kano_Bool));
	else if (a.type->kind == CODE_TYPE_CHARACTER)
		r.imm.bool_value = (EvaluationTypeValue(a, Kano_Char) || EvaluationTypeValue(b, Kano_Char));
	else if (a.type->kind == CODE_TYPE_INTEGER)
		r.imm.bool_value = (EvaluationTypeValue(a, Kano_Int) || EvaluationTypeValue(b, Kano_Int));
	else if (a.type->kind == CODE_TYPE_POINTER)
		r.imm.bool_value = (EvaluationTypeValue(a, void *) || EvaluationTypeValue(b, void *));
	else if (a.type->kind == CODE_TYPE_REAL)
		r.imm.bool_value = (EvaluationTypeValue(a, Kano_Real) || EvaluationTypeValue(b, Kano_Real));
	else
		Unreachable();
	return r;
}

static BinaryOperatorProc BinaryOperators[] = {
	binary_add,  binary_sub,  binary_mul,  binary_div, binary_mod, binary_rs,   binary_ls,   binary_add,  binary_xor,
	binary_or,   binary_gt,   binary_lt,   binary_ge,  binary_le,  binary_cmp,  binary_ncmp, binary_cadd, binary_csub,
	binary_cmul, binary_cdiv, binary_cmod, binary_crs, binary_cls, binary_cadd, binary_cxor, binary_cor,
	binary_land, binary_lor };

static Evaluation_Value interp_eval_expression(Interpreter *interp, Code_Node *root);
static Evaluation_Value interp_eval_root_expression(Interpreter *interp, Code_Node_Expression *root);

static Evaluation_Value interp_eval_binary_operator(Interpreter *interp, Code_Node_Binary_Operator *node)
{
	auto b = interp_eval_expression(interp, node->right);

	// Copy to imm value, so that it doesn't change changed if procedures are being called when solving for a
	if (b.from_address)
	{
		memcpy(&b.imm, b.from_address, b.type->runtime_size);
		b.from_address = nullptr;
	}

	auto a = interp_eval_expression(interp, node->left);

	Assert(node->op_kind < ArrayCount(BinaryOperators));
	
	return BinaryOperators[node->op_kind](a, b, node->type);
}

static Evaluation_Value interp_eval_assignment(Interpreter *interp, Code_Node_Assignment *node)
{
	auto value = interp_eval_root_expression(interp, (Code_Node_Expression *)node->value);
	
	auto dst = interp_eval_root_expression(interp, node->destination);
	
	if (dst.from_address)
	{
		uint64_t *source = (uint64_t *)EvaluationTypePointer(value, void *);
		memcpy(dst.from_address, source, value.type->runtime_size);
	}
	else if (dst.type->kind == CODE_TYPE_POINTER)
	{
		memcpy(dst.imm.pointer_value, EvaluationTypePointer(value, void *), value.type->runtime_size);
	}
	else
	{
		Unreachable();
	}
	
	return value;
}

static void interp_eval_block(Interpreter *interp, Code_Node_Block *root, bool isproc);

static inline void interp_push_aligned_parameter(Interpreter *interp, Code_Node_Procedure_Call *root, uint64_t prev_top, uint64_t new_top, uint64_t offset)
{
	for (int64_t index = 0; index < root->parameter_count; ++index)
	{
		auto param = root->parameters[index];
		offset = AlignPower2Up(offset, (uint64_t)param->type->alignment);
		interp->stack_top = prev_top;
		auto var = interp_eval_root_expression(interp, param);
		interp->stack_top = new_top;
		offset = interp_push_into_stack(interp, var, offset);
	}
}

static Evaluation_Value interp_make_type_value(Interpreter *interp, Code_Type *type) {
	Evaluation_Value value;
	value.imm.pointer_value = (uint8_t *)type;
	value.type = code_type_resolver_find_type(interp->resolver, "*void");
	return value;
}

static Evaluation_Value interp_eval_procedure_call(Interpreter *interp, Code_Node_Procedure_Call *root)
{
	auto prev_top = interp->stack_top;

	auto new_top = root->stack_top + prev_top;

	// @Note: We do not align variadics to simplify the variadics routine

	uint64_t variadics_args_size = 0;
	for (int64_t i = 0; i < root->variadic_count; ++i)
	{
		variadics_args_size += root->variadics[i]->type->runtime_size;
		variadics_args_size += sizeof(Code_Type *);
	}

	{
		// @Note: Variadics are evaluated in reverse order because we add Code_Type * as well
		// If this were done in forward order, the result placed by a function call in the top
		// of the stack will be erased when we push Code_Type * into the stack
		uint64_t offset = variadics_args_size;
		for (int64_t i = root->variadic_count - 1; i >= 0; --i)
		{
			interp->stack_top = prev_top;
			auto param = root->variadics[i];
			auto var = interp_eval_root_expression(interp, param);
			interp->stack_top = new_top;
			offset = interp_push_into_stack_reduced(interp, var, offset);
			offset = interp_push_into_stack_reduced(interp, interp_make_type_value(interp, param->type), offset);
		}
	}
	
	new_top += variadics_args_size;

	uint64_t return_type_size = 0;
	if (root->type)
	{
		new_top = AlignPower2Up(new_top, (uint64_t)root->type->alignment);
		return_type_size = root->type->runtime_size;
	}
	else if (root->parameter_count)
	{
		new_top = AlignPower2Up(new_top, (uint64_t)root->parameters[0]->type->alignment);
	}

	interp_push_aligned_parameter(interp, root, prev_top, new_top, return_type_size);

	interp->stack_top = prev_top;
	auto proc_expr = interp_eval_root_expression(interp, root->procedure);
	auto procedure = EvaluationTypeValue(proc_expr, Code_Value_Procedure);
	
	auto prev_proc = interp->current_procedure;
	interp->stack_top = new_top;
	interp->current_procedure = root->procedure_type;

	if (procedure.block)
		interp_eval_block(interp, procedure.block, true);
	else
		procedure.ccall(interp);

	Evaluation_Value result;
	if (root->type)
	{
		result.from_address = (uint8_t *)(interp->stack + interp->stack_top);
		result.type    = root->type;
	}
	else
	{
		result.from_address = nullptr;
		result.type    = nullptr;
	}

	interp->current_procedure = prev_proc;
	interp->stack_top = prev_top;
	
	return result;
}

static Evaluation_Value interp_eval_expression(Interpreter *interp, Code_Node *root)
{
	switch (root->kind)
	{
		case CODE_NODE_LITERAL: return interp_eval_literal(interp, (Code_Node_Literal *)root);
		case CODE_NODE_UNARY_OPERATOR: return interp_eval_unary_operator(interp, (Code_Node_Unary_Operator *)root);
		case CODE_NODE_BINARY_OPERATOR: return interp_eval_binary_operator(interp, (Code_Node_Binary_Operator *)root);
		case CODE_NODE_ADDRESS: return interp_eval_address(interp, (Code_Node_Address *)root);
		case CODE_NODE_OFFSET: return interp_eval_offset(interp, (Code_Node_Offset *)root);
		case CODE_NODE_ASSIGNMENT: return interp_eval_assignment(interp, (Code_Node_Assignment *)root);
		case CODE_NODE_TYPE_CAST: return interp_eval_type_cast(interp, (Code_Node_Type_Cast *)root);
		case CODE_NODE_IF: return interp_eval_expression(interp, (Code_Node *)root);
		case CODE_NODE_PROCEDURE_CALL: return interp_eval_procedure_call(interp, (Code_Node_Procedure_Call *)root);
		case CODE_NODE_RETURN: return interp_eval_return(interp, (Code_Node_Return *)root);
		case CODE_NODE_BREAK: interp_eval_break(interp, (Code_Node_Break *)root); return Evaluation_Value{};
		case CODE_NODE_CONTINUE: interp_eval_continue(interp, (Code_Node_Continue *)root); return Evaluation_Value{};
		
		NoDefaultCase();
	}
	
	Unreachable();
	return Evaluation_Value{};
}

static Evaluation_Value interp_eval_root_expression(Interpreter *interp, Code_Node_Expression *root)
{
	return interp_eval_expression(interp, root->child);
}

int64_t interp_evaluate_constant_expression(Code_Node_Expression *root) {
	Interpreter interp;
	interp_init(&interp, nullptr, 0, 0);

	Assert(root->type->kind == CODE_TYPE_INTEGER || root->type->kind == CODE_TYPE_CHARACTER);

	auto value = interp_eval_root_expression(&interp, root);

	if (value.type->kind == CODE_TYPE_INTEGER)
		return (int64_t)EvaluationTypeValue(value, Kano_Int);
	else if (value.type->kind == CODE_TYPE_CHARACTER)
		return (int64_t)EvaluationTypeValue(value, Kano_Char);
	else
		Unreachable();
	return 0;
}

static bool interp_eval_statement(Interpreter *interp, Code_Node_Statement *root, Evaluation_Value *value);

static void interp_eval_do(Interpreter *interp, Code_Node_Do *root)
{
	auto do_cond = root->condition;
	auto do_body = root->body;
	
	Evaluation_Value cond;
	
	auto return_index = interp->return_count;
	auto break_index = interp->break_count;
	auto continue_index = interp->continue_count;
	do
	{
		interp_eval_statement(interp, do_body, nullptr);
		if (return_index != interp->return_count)
			break;
		if (continue_index != interp->continue_count)
		{
			interp->continue_count = continue_index;
			continue;
		}
		if (break_index != interp->break_count)
		{
			interp->break_count = break_index;
			break;
		}
		bool value = interp_eval_statement(interp, do_cond, &cond);
		Assert(value);
	} while (EvaluationTypeValue(cond, bool));
}

static void interp_eval_while(Interpreter *interp, Code_Node_While *root)
{
	auto while_cond = root->condition;
	auto while_body = root->body;
	
	Evaluation_Value cond;
	bool value = interp_eval_statement(interp, while_cond, &cond);
	Assert(value);
	
	auto return_index = interp->return_count;
	auto break_index = interp->break_count;
	auto continue_index = interp->continue_count;
	while (EvaluationTypeValue(cond, bool))
	{
		interp_eval_statement(interp, while_body, nullptr);
		if (return_index != interp->return_count)
			break;
		if (continue_index != interp->continue_count)
		{
			interp->continue_count = continue_index;
			continue;
		}
		if (break_index != interp->break_count)
		{
			interp->break_count = break_index;
			break;
		}
		bool value = interp_eval_statement(interp, while_cond, &cond);
		Assert(value);
	}
}

static void interp_eval_if(Interpreter *interp, Code_Node_If *root)
{
	auto cond = interp_eval_root_expression(interp, (Code_Node_Expression *)root->condition);
	if (EvaluationTypeValue(cond, bool))
	{
		interp_eval_statement(interp, (Code_Node_Statement *)root->true_statement, nullptr);
	}
	else
	{
		if (root->false_statement)
			interp_eval_statement(interp, (Code_Node_Statement *)root->false_statement, nullptr);
	}
}

static void interp_eval_for(Interpreter *interp, Code_Node_For *root)
{
	auto for_init = root->initialization;
	auto for_cond = root->condition;
	auto for_incr = root->increment;
	auto for_body = root->body;
	
	Evaluation_Value cond;
	
	interp_eval_statement(interp, for_init, nullptr);
	bool value = interp_eval_statement(interp, for_cond, &cond);
	Assert(value);
	
	auto return_index = interp->return_count;
	auto break_index = interp->break_count;
	auto continue_index = interp->continue_count;
	while (EvaluationTypeValue(cond, bool))
	{
		interp_eval_statement(interp, for_body, nullptr);
		if (return_index != interp->return_count)
			break;
		if (continue_index != interp->continue_count)
		{
			interp->continue_count = continue_index;
			continue;
		}
		if (break_index != interp->break_count)
		{
			interp->break_count = break_index;
			break;
		}
		bool value;
		value = interp_eval_statement(interp, for_incr, nullptr);
		Assert(value);
		value = interp_eval_statement(interp, for_cond, &cond);
		Assert(value);
	}
}

static bool interp_eval_statement(Interpreter *interp, Code_Node_Statement *root, Evaluation_Value *out_value)
{
	Assert(root->symbol_table);

	interp->current_row = root->source_row;
	interp->intercept(interp, INTERCEPT_STATEMENT, root);

	Evaluation_Value value;
	Evaluation_Value *dst = out_value ? out_value : &value;

	switch (root->node->kind)
	{
		case CODE_NODE_EXPRESSION: 
			*dst = interp_eval_root_expression(interp, (Code_Node_Expression *)root->node);
			return true;
		
		case CODE_NODE_ASSIGNMENT:
			*dst = interp_eval_assignment(interp, (Code_Node_Assignment *)root->node);
			return true;
		
		case CODE_NODE_BLOCK:
			interp_eval_block(interp, (Code_Node_Block *)root->node, false);
			return false;
		
		case CODE_NODE_IF:
			interp_eval_if(interp, (Code_Node_If *)root->node);
			return false;
		
		case CODE_NODE_FOR:
			interp_eval_for(interp, (Code_Node_For *)root->node);
			return false;
		
		case CODE_NODE_WHILE:
			interp_eval_while(interp, (Code_Node_While *)root->node);
			return false;
		
		case CODE_NODE_DO:
			interp_eval_do(interp, (Code_Node_Do *)root->node);
			return false;
		
		NoDefaultCase();
	}

	Unreachable();
	
	return false;
}

static void interp_eval_block(Interpreter *interp, Code_Node_Block *root, bool isproc)
{
	if (isproc)
	{
		interp->intercept(interp, INTERCEPT_PROCEDURE_CALL, root);
	}

	auto return_index = interp->return_count;
	auto break_index = interp->break_count;
	auto continue_index = interp->continue_count;
	for (auto statement = root->statement_head; statement; statement = statement->next)
	{
		interp_eval_statement(interp, statement, nullptr);

		if (return_index != interp->return_count)
		{
			if (isproc)
				interp->return_count -= 1;
			break;
		}

		if (break_index != interp->break_count || continue_index != interp->continue_count)
			break;
	}

	if (isproc)
	{
		interp->intercept(interp, INTERCEPT_PROCEDURE_RETURN, root);
	}
}

//
//
//

void interp_init(Interpreter *interp, Code_Type_Resolver *resolver, size_t stack_size, size_t bss_size)
{
	interp->stack  = new uint8_t[stack_size];
	interp->global = new uint8_t[bss_size];
	interp->stack_size = stack_size;
	interp->global_size = bss_size;
	interp->resolver = resolver;
	memset(interp->stack, 0, stack_size);
	memset(interp->global, 0, bss_size);
}

void interp_eval_globals(Interpreter *interp, Array_View<Code_Node_Assignment *> exprs)
{
	for (auto expr : exprs)
		interp_eval_assignment(interp, expr);
}

#include "JsonWriter.h"

Code_Node_Procedure_Call *interp_find_main(Interpreter *interp) {
	auto resolver = interp->resolver;
	auto main_proc = code_type_resolver_find(resolver, "main");

	if (!main_proc) {
		auto error = code_type_resolver_error_stream(resolver);
		Write(error, "\'main\' procedure not defined!");
		return nullptr;
	}

	if (!(main_proc->flags & SYMBOL_BIT_CONSTANT) || main_proc->address.kind != Symbol_Address::CODE) {
		auto error = code_type_resolver_error_stream(resolver);
		Write(error, "The \'main\' procedure must be constant!\n");
		return nullptr;
	}

	if (main_proc->type->kind != CODE_TYPE_PROCEDURE) {
		auto error = code_type_resolver_error_stream(resolver);
		Write(error, "The \'main\' symbol must be a procedure!\n");
		return nullptr;
	}

	auto proc_type = (Code_Type_Procedure *)main_proc->type;
	if (proc_type->argument_count != 0 || proc_type->return_type || proc_type->is_variadic) {
		auto error = code_type_resolver_error_stream(resolver);
		Write(error, "The \"main\" procedure must not take any arguments and should return nothing!\n");
		return nullptr;
	}

	auto proc = (Code_Type_Procedure *)main_proc->type;

	auto address = new Code_Node_Address;
	address->type = proc;
	address->address = &main_proc->address;

	auto expr = new Code_Node_Expression;
	expr->type = proc;
	expr->child = address;

	auto proc_call = new Code_Node_Procedure_Call;
	proc_call->type = proc->return_type;
	proc_call->procedure_type = proc_type;
	//proc_call->source_row = main_proc->location.start_row;
	proc_call->flags = main_proc->flags;
	proc_call->procedure = expr;

	return proc_call;
}

void interp_evaluate_procedure(Interpreter *interp, Code_Node_Procedure_Call *proc) {
	interp_eval_procedure_call(interp, proc);
}
