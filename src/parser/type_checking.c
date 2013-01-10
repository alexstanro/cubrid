/*
 * Copyright (C) 2008 Search Solution Corporation. All rights reserved by Search Solution.
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *
 */

/*
 * type_checking.c - auxiliary functions for parse tree translation
 */

#ident "$Id$"

#include "config.h"

#include <assert.h>
#include <stdarg.h>
#include <ctype.h>
#include <float.h>
#include <math.h>
#include <limits.h>

#if defined(WINDOWS)
#include "porting.h"
#include "wintcp.h"
#else /* ! WINDOWS */
#include <sys/time.h>
#endif /* ! WINDOWS */

#include "error_manager.h"
#include "parser.h"
#include "parser_message.h"
#include "set_object.h"
#include "arithmetic.h"
#include "string_opfunc.h"
#include "object_domain.h"
#include "semantic_check.h"
#include "xasl_generation.h"
#include "language_support.h"
#include "schema_manager.h"
#include "system_parameter.h"
#include "network_interface_cl.h"
#include "object_template.h"

/* this must be the last header file included!!! */
#include "dbval.h"

#define SET_EXPECTED_DOMAIN(node, dom)                                         \
            do {                                                               \
		(node)->expected_domain = (dom);                               \
		if ((node)->or_next) {                                         \
		    PT_NODE *_or_next = (node)->or_next;                       \
		    while (_or_next) {                                         \
		        if (_or_next->type_enum == PT_TYPE_MAYBE               \
			    && _or_next->expected_domain == NULL) {            \
		            _or_next->expected_domain = (dom);                 \
		        }                                                      \
			_or_next = _or_next->or_next;                          \
		    }                                                          \
		}                                                              \
	    } while (0)

#if defined(ENABLE_UNUSED_FUNCTION)
typedef struct generic_function_record
{
  const char *function_name;
  const char *return_type;
  int func_ptr_offset;
} GENERIC_FUNCTION_RECORD;

static int pt_Generic_functions_sorted = 0;

static GENERIC_FUNCTION_RECORD pt_Generic_functions[] = {
  /* Make sure that this table is in synch with the generic function table.
   * Don't remove the first dummy position.  It's a place holder.
   */
  {"AAA_DUMMY", "integer", 0}
};
#endif /* ENABLE_UNUSED_FUNCTION */

#define PT_ARE_COMPARABLE_CHAR_TYPE(typ1, typ2)				      \
   ((PT_IS_SIMPLE_CHAR_STRING_TYPE (typ1) &&				      \
    PT_IS_SIMPLE_CHAR_STRING_TYPE (typ2)) ||				      \
   (PT_IS_NATIONAL_CHAR_STRING_TYPE (typ1) &&				      \
    PT_IS_NATIONAL_CHAR_STRING_TYPE (typ2)))

#define PT_ARE_COMPARABLE_NUMERIC_TYPE(typ1, typ2)			      \
   ((PT_IS_NUMERIC_TYPE (typ1) && PT_IS_NUMERIC_TYPE (typ2)) ||		      \
    (PT_IS_NUMERIC_TYPE (typ1) && typ2 == PT_TYPE_MAYBE) ||		      \
    (typ1 == PT_TYPE_MAYBE && PT_IS_NUMERIC_TYPE (typ2)))

/* Two types are comparable if they are NUMBER types or same CHAR type */
#define PT_ARE_COMPARABLE(typ1, typ2)					      \
  ((typ1 == typ2) ||							      \
   PT_ARE_COMPARABLE_CHAR_TYPE (typ1, typ2) ||				      \
   PT_ARE_COMPARABLE_NUMERIC_TYPE (typ1, typ2))

#define PT_IS_RECURSIVE_EXPRESSION(node)				      \
  ((node)->node_type == PT_EXPR &&					      \
    (PT_IS_LEFT_RECURSIVE_EXPRESSION(node) ||				      \
     PT_IS_RIGHT_RECURSIVE_EXPRESSION(node)))

#define PT_IS_LEFT_RECURSIVE_EXPRESSION(node)				      \
    ((node)->info.expr.op == PT_GREATEST ||				      \
    (node)->info.expr.op == PT_LEAST ||					      \
    (node)->info.expr.op == PT_COALESCE)

#define PT_IS_RIGHT_RECURSIVE_EXPRESSION(node)				      \
    ((node)->info.expr.op == PT_CASE ||					      \
     (node)->info.expr.op == PT_DECODE)

typedef struct compare_between_operator
{
  PT_OP_TYPE left;
  PT_OP_TYPE right;
  PT_OP_TYPE between;
} COMPARE_BETWEEN_OPERATOR;

static COMPARE_BETWEEN_OPERATOR pt_Compare_between_operator_table[] = {
  {PT_GE, PT_LE, PT_BETWEEN_GE_LE},
  {PT_GE, PT_LT, PT_BETWEEN_GE_LT},
  {PT_GT, PT_LE, PT_BETWEEN_GT_LE},
  {PT_GT, PT_LT, PT_BETWEEN_GT_LT},
  {PT_EQ, PT_EQ, PT_BETWEEN_EQ_NA},
  {PT_GT_INF, PT_LE, PT_BETWEEN_INF_LE},
  {PT_GT_INF, PT_EQ, PT_BETWEEN_INF_LE},
  {PT_GT_INF, PT_LT, PT_BETWEEN_INF_LT},
  {PT_GE, PT_LT_INF, PT_BETWEEN_GE_INF},
  {PT_EQ, PT_LT_INF, PT_BETWEEN_GE_INF},
  {PT_GT, PT_LT_INF, PT_BETWEEN_GT_INF}
};

#define COMPARE_BETWEEN_OPERATOR_COUNT \
        sizeof(pt_Compare_between_operator_table) / \
        sizeof(COMPARE_BETWEEN_OPERATOR)

/* maximum number of overloads for an expression */
#define MAX_OVERLOADS 16

/* generic types */
typedef enum pt_generic_type_enum
{
  PT_GENERIC_TYPE_NONE,
  PT_GENERIC_TYPE_STRING,	/* any type of string */
  PT_GENERIC_TYPE_STRING_VARYING,	/* VARCHAR or VARNCHAR */
  PT_GENERIC_TYPE_CHAR,		/* VARCHAR or CHAR */
  PT_GENERIC_TYPE_NCHAR,	/* VARNCHAR or NCHAR */
  PT_GENERIC_TYPE_BIT,		/* BIT OR VARBIT */
  PT_GENERIC_TYPE_DISCRETE_NUMBER,	/*SMALLINT, INTEGER, BIGINTEGER */
  PT_GENERIC_TYPE_NUMBER,	/* any number type */
  PT_GENERIC_TYPE_DATE,		/* date, datetime or timestamp */
  PT_GENERIC_TYPE_DATETIME,	/* any date or time type */
  PT_GENERIC_TYPE_SEQUENCE,	/* any type of sequence */
  PT_GENERIC_TYPE_LOB,		/* BLOB or CLOB */
  PT_GENERIC_TYPE_QUERY,	/* Sub query (for range operators) */
  PT_GENERIC_TYPE_PRIMITIVE,	/* primitive types */
  PT_GENERIC_TYPE_ANY		/* any type */
} PT_GENERIC_TYPE_ENUM;

/* expression argument type description */
typedef union pt_arg_type_val
{
  PT_TYPE_ENUM type;
  PT_GENERIC_TYPE_ENUM generic_type;
} PT_ARG_TYPE_VAL;

/* expression argument type */
typedef struct pt_arg_type
{
  PT_ARG_TYPE_VAL val;
  bool is_generic;
} PT_ARG_TYPE;

/* SQL expression signature */
typedef struct expression_signature
{
  PT_ARG_TYPE return_type;
  PT_ARG_TYPE arg1_type;
  PT_ARG_TYPE arg2_type;
  PT_ARG_TYPE arg3_type;
} EXPRESSION_SIGNATURE;

/* SQL expression definition */
typedef struct expression_definition
{
  PT_OP_TYPE op;
  int overloads_count;
  EXPRESSION_SIGNATURE overloads[MAX_OVERLOADS];
} EXPRESSION_DEFINITION;

static PT_TYPE_ENUM pt_infer_common_type (const PT_OP_TYPE op,
					  PT_TYPE_ENUM * arg1,
					  PT_TYPE_ENUM * arg2,
					  PT_TYPE_ENUM * arg3,
					  const TP_DOMAIN * expected_domain);
static bool pt_get_expression_definition (const PT_OP_TYPE op,
					  EXPRESSION_DEFINITION * def);
static bool does_op_specially_treat_null_arg (PT_OP_TYPE op);
static int pt_apply_expressions_definition (PARSER_CONTEXT * parser,
					    PT_NODE ** expr);
static PT_TYPE_ENUM pt_expr_get_return_type (PT_NODE * expr,
					     const EXPRESSION_SIGNATURE sig);
static int pt_coerce_expression_argument (PARSER_CONTEXT * parser,
					  PT_NODE * expr,
					  PT_NODE ** arg,
					  const PT_TYPE_ENUM arg_type,
					  PT_NODE * data_type);
static PT_NODE *pt_coerce_expr_arguments (PARSER_CONTEXT * parser,
					  PT_NODE * expr, PT_NODE * arg1,
					  PT_NODE * arg2, PT_NODE * arg3,
					  EXPRESSION_SIGNATURE sig);
static PT_NODE *pt_coerce_range_expr_arguments (PARSER_CONTEXT * parser,
						PT_NODE * expr,
						PT_NODE * arg1,
						PT_NODE * arg2,
						PT_NODE * arg3,
						EXPRESSION_SIGNATURE sig);
static bool pt_is_range_expression (const PT_OP_TYPE op);
static bool pt_are_equivalent_types (const PT_ARG_TYPE def_type,
				     const PT_TYPE_ENUM op_type);
static bool pt_are_unmatchable_types (const PT_ARG_TYPE def_type,
				      const PT_TYPE_ENUM op_type);
static PT_TYPE_ENUM pt_get_equivalent_type (const PT_ARG_TYPE def_type,
					    const PT_TYPE_ENUM type);
static PT_TYPE_ENUM pt_get_equivalent_type_with_op (const PT_ARG_TYPE
						    def_type,
						    const PT_TYPE_ENUM
						    arg_type, PT_OP_TYPE op);
static PT_NODE *pt_evaluate_new_data_type (const PT_TYPE_ENUM old_type,
					   const PT_TYPE_ENUM new_type,
					   PT_NODE * data_type);
static PT_TYPE_ENUM pt_get_common_collection_type (const PT_NODE * set,
						   bool * is_multitype);
static bool pt_is_collection_of_type (const PT_NODE * collection,
				      const PT_TYPE_ENUM collection_type,
				      const PT_TYPE_ENUM element_type);
static bool pt_is_symmetric_type (const PT_TYPE_ENUM type_enum);
static PT_NODE *pt_propagate_types (PARSER_CONTEXT * parser, PT_NODE * expr,
				    PT_NODE * otype1, PT_NODE * otype2);
static int pt_union_sets (PARSER_CONTEXT * parser, TP_DOMAIN * domain,
			  DB_VALUE * set1, DB_VALUE * set2, DB_VALUE * result,
			  PT_NODE * o2);
static int pt_difference_sets (PARSER_CONTEXT * parser, TP_DOMAIN * domain,
			       DB_VALUE * set1, DB_VALUE * set2,
			       DB_VALUE * result, PT_NODE * o2);
static int pt_product_sets (PARSER_CONTEXT * parser, TP_DOMAIN * domain,
			    DB_VALUE * set1, DB_VALUE * set2,
			    DB_VALUE * result, PT_NODE * o2);
static PT_NODE *pt_to_false_subquery (PARSER_CONTEXT * parser,
				      PT_NODE * node);
static PT_NODE *pt_fold_union (PARSER_CONTEXT * parser, PT_NODE * node,
			       bool arg1_is_false);
static PT_NODE *pt_eval_recursive_expr_type (PARSER_CONTEXT * parser,
					     PT_NODE * gl_expr);
static PT_NODE *pt_eval_type_pre (PARSER_CONTEXT * parser, PT_NODE * node,
				  void *arg, int *continue_walk);
static PT_NODE *pt_eval_type (PARSER_CONTEXT * parser, PT_NODE * node,
			      void *arg, int *continue_walk);
static PT_NODE *pt_fold_constants (PARSER_CONTEXT * parser, PT_NODE * node,
				   void *arg, int *continue_walk);
static void pt_chop_to_one_select_item (PARSER_CONTEXT * parser,
					PT_NODE * node);
static bool pt_is_able_to_determine_return_type (const PT_OP_TYPE op);
static PT_NODE *pt_eval_expr_type (PARSER_CONTEXT * parser, PT_NODE * node);
static PT_NODE *pt_eval_opt_type (PARSER_CONTEXT * parser, PT_NODE * node);
static PT_TYPE_ENUM pt_common_type_op (PT_TYPE_ENUM t1, PT_OP_TYPE op,
				       PT_TYPE_ENUM t2);
static int pt_upd_domain_info (PARSER_CONTEXT * parser, PT_NODE * arg1,
			       PT_NODE * arg2, PT_OP_TYPE op,
			       PT_TYPE_ENUM common_type, PT_NODE * node);
static int pt_check_and_coerce_to_time (PARSER_CONTEXT * parser,
					PT_NODE * src);
static int pt_check_and_coerce_to_date (PARSER_CONTEXT * parser,
					PT_NODE * src);
static int pt_coerce_str_to_time_date_utime_datetime (PARSER_CONTEXT * parser,
						      PT_NODE * src,
						      PT_TYPE_ENUM *
						      result_type);
static int pt_coerce_3args (PARSER_CONTEXT * parser, PT_NODE * arg1,
			    PT_NODE * arg2, PT_NODE * arg3);
static PT_NODE *pt_eval_function_type (PARSER_CONTEXT * parser,
				       PT_NODE * node);
static PT_NODE *pt_eval_method_call_type (PARSER_CONTEXT * parser,
					  PT_NODE * node);
static PT_NODE *pt_fold_const_expr (PARSER_CONTEXT * parser, PT_NODE * expr,
				    void *arg);
static PT_NODE *pt_fold_const_function (PARSER_CONTEXT * parser,
					PT_NODE * func);
static const char *pt_class_name (const PT_NODE * type);
static int pt_set_default_data_type (PARSER_CONTEXT * parser,
				     PT_TYPE_ENUM type, PT_NODE ** dtp);
#if defined(ENABLE_UNUSED_FUNCTION)
static int generic_func_casecmp (const void *a, const void *b);
static void init_generic_funcs (void);
#endif /* ENABLE_UNUSED_FUNCTION */
static PT_NODE *pt_compare_bounds_to_value (PARSER_CONTEXT * parser,
					    PT_NODE * expr, PT_OP_TYPE op,
					    PT_TYPE_ENUM lhs_type,
					    DB_VALUE * rhs_val,
					    PT_TYPE_ENUM rhs_type);
static PT_TYPE_ENUM pt_get_common_datetime_type (PARSER_CONTEXT * parser,
						 PT_TYPE_ENUM common_type,
						 PT_TYPE_ENUM arg1_type,
						 PT_TYPE_ENUM arg2_type,
						 PT_NODE * arg1,
						 PT_NODE * arg2);
static int pt_character_length_for_type (PT_NODE * node,
					 const PT_TYPE_ENUM coerce_type);
static PT_NODE *pt_wrap_expr_w_exp_dom_cast (PARSER_CONTEXT * parser,
					     PT_NODE * expr);
static bool pt_is_op_with_forced_common_type (PT_OP_TYPE op);
static bool pt_check_const_fold_op_w_args (PT_OP_TYPE op,
					   DB_VALUE * arg1, DB_VALUE * arg2,
					   DB_VALUE * arg3);
static bool pt_is_range_or_comp (PT_OP_TYPE op);
static bool pt_is_op_w_collation (const PT_OP_TYPE op);
static int pt_get_collation_info_for_collection_type (PARSER_CONTEXT * parser,
						      PT_NODE * node,
						      int *coll_id,
						      INTL_CODESET * codeset,
						      PT_COLL_COERC_LEV *
						      coerc_level);
static PT_NODE *pt_coerce_node_collation (PARSER_CONTEXT * parser,
					  PT_NODE * node, const int coll_id,
					  const INTL_CODESET codeset,
					  bool force_mode,
					  bool use_collate_modifier);
static int pt_check_expr_collation (PARSER_CONTEXT * parser, PT_NODE ** node);
static int pt_check_recursive_expr_collation (PARSER_CONTEXT * parser,
					      PT_NODE ** node);
static PT_NODE *pt_node_to_enumeration_expr (PARSER_CONTEXT * parser,
					     PT_NODE * data_type,
					     PT_NODE * node);
static PT_NODE *pt_select_list_to_enumeration_expr (PARSER_CONTEXT * parser,
						    PT_NODE * data_type,
						    PT_NODE * node);
static bool pt_is_enumeration_special_comparison (PT_NODE * arg1,
						  PT_OP_TYPE op,
						  PT_NODE * arg2);
static PT_NODE *pt_fix_enumeration_comparison (PARSER_CONTEXT * parser,
					       PT_NODE * expr);

/*
 * pt_get_expression_definition () - get the expression definition for the
 *				     expression op.
 *   return: true if the expression has a definition, false otherwise
 *   op(in)	: the expression operator
 *   def(in/out): the expression definition
 */
static bool
pt_get_expression_definition (const PT_OP_TYPE op,
			      EXPRESSION_DEFINITION * def)
{
  EXPRESSION_SIGNATURE sig;
  int num;

  assert (def != NULL);

  def->op = op;

  sig.arg1_type.is_generic = false;
  sig.arg1_type.val.type = PT_TYPE_NONE;

  sig.arg2_type.is_generic = false;
  sig.arg2_type.val.type = PT_TYPE_NONE;

  sig.arg3_type.is_generic = false;
  sig.arg3_type.val.type = PT_TYPE_NONE;

  sig.return_type.is_generic = false;
  sig.return_type.val.type = PT_TYPE_NONE;

  switch (op)
    {
    case PT_AND:
    case PT_OR:
    case PT_XOR:
      num = 0;

      /* one overload */

      /* arg1 */
      sig.arg1_type.is_generic = false;
      sig.arg1_type.val.type = PT_TYPE_LOGICAL;

      /* arg2 */
      sig.arg2_type.is_generic = false;
      sig.arg2_type.val.type = PT_TYPE_LOGICAL;

      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_LOGICAL;

      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_NOT:
      num = 0;

      /* one overload */

      /* arg1 */
      sig.arg1_type.is_generic = false;
      sig.arg1_type.val.type = PT_TYPE_LOGICAL;

      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_LOGICAL;

      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_ACOS:
    case PT_ASIN:
    case PT_ATAN:
    case PT_COS:
    case PT_COT:
    case PT_DEGREES:
    case PT_EXP:
    case PT_LN:
    case PT_LOG10:
    case PT_LOG2:
    case PT_SQRT:
    case PT_RADIANS:
    case PT_SIN:
    case PT_TAN:
      num = 0;

      /* one overload */

      /* arg1 */
      sig.arg1_type.is_generic = false;
      sig.arg1_type.val.type = PT_TYPE_DOUBLE;

      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_DOUBLE;

      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_ABS:
      num = 0;

      /* one overload */

      /* arg1 */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_NUMBER;

      /* return type */
      sig.return_type.is_generic = true;
      sig.return_type.val.generic_type = PT_GENERIC_TYPE_NUMBER;

      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_ATAN2:
    case PT_LOG:
    case PT_POWER:
      num = 0;

      /* one overload */

      /* arg1 */
      sig.arg1_type.is_generic = false;
      sig.arg1_type.val.type = PT_TYPE_DOUBLE;

      /* arg2 */
      sig.arg2_type.is_generic = false;
      sig.arg2_type.val.type = PT_TYPE_DOUBLE;

      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_DOUBLE;

      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_RAND:
    case PT_RANDOM:
      num = 0;

      /* one overload */

      /* arg1 */
      sig.arg1_type.is_generic = false;
      sig.arg1_type.val.type = PT_TYPE_NONE;

      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_INTEGER;

      def->overloads[num++] = sig;

      /* arg1 */
      sig.arg1_type.is_generic = false;
      sig.arg1_type.val.type = PT_TYPE_INTEGER;

      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_INTEGER;

      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_DRAND:
    case PT_DRANDOM:
      num = 0;

      /* one overload */

      /* arg1 */
      sig.arg1_type.is_generic = false;
      sig.arg1_type.val.type = PT_TYPE_INTEGER;

      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_DOUBLE;

      def->overloads[num++] = sig;

      /* arg1 */
      sig.arg1_type.is_generic = false;
      sig.arg1_type.val.type = PT_TYPE_NONE;

      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_DOUBLE;

      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_BIT_AND:
    case PT_BIT_XOR:
    case PT_BIT_OR:
    case PT_BITSHIFT_LEFT:
    case PT_BITSHIFT_RIGHT:
      num = 0;

      /* one overload */

      /* arg1 */
      sig.arg1_type.is_generic = false;
      sig.arg1_type.val.type = PT_TYPE_BIGINT;

      /* arg2 */
      sig.arg2_type.is_generic = false;
      sig.arg2_type.val.type = PT_TYPE_BIGINT;

      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_BIGINT;

      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_BIT_LENGTH:
    case PT_OCTET_LENGTH:
      num = 0;

      /* two overloads */

      /* arg1 */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_BIT;
      /* return type */

      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_INTEGER;
      def->overloads[num++] = sig;

      /* arg1 */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_STRING;

      /*return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_INTEGER;
      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_BIT_COUNT:
      num = 0;

      /* one overload */

      /* arg1 */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_NUMBER;

      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_INTEGER;

      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_BIT_NOT:
      num = 0;

      /* one overload */

      /* arg1 */
      sig.arg1_type.is_generic = false;
      sig.arg1_type.val.type = PT_TYPE_BIGINT;

      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_BIGINT;

      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_BETWEEN:
    case PT_NOT_BETWEEN:
      num = 0;

      /* one overload */

      /* arg1 */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_ANY;

      /* arg2 */
      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_ANY;

      /* arg3 */
      sig.arg3_type.is_generic = true;
      sig.arg3_type.val.generic_type = PT_GENERIC_TYPE_ANY;

      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_LOGICAL;

      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_LIKE:
    case PT_NOT_LIKE:
      num = 0;

      /* four overloads */

      /* BOOL PT_LIKE([VAR]CHAR, [VAR]CHAR); */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_CHAR;

      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_CHAR;

      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_LOGICAL;

      def->overloads[num++] = sig;

      /* BOOL PT_LIKE([VAR]NCHAR, [VAR]NCHAR); */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_NCHAR;

      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_NCHAR;

      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_LOGICAL;

      def->overloads[num++] = sig;

      /* BOOL PT_LIKE([VAR]CHAR, [VAR]CHAR, [VAR]CHAR); */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_CHAR;

      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_CHAR;

      sig.arg3_type.is_generic = true;
      sig.arg3_type.val.generic_type = PT_GENERIC_TYPE_CHAR;

      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_LOGICAL;

      def->overloads[num++] = sig;

      /* BOOL PT_LIKE([VAR]NCHAR, [VAR]NCHAR, [VAR]NCHAR); */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_NCHAR;

      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_NCHAR;

      sig.arg3_type.is_generic = true;
      sig.arg3_type.val.generic_type = PT_GENERIC_TYPE_NCHAR;

      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_LOGICAL;

      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_RLIKE:
    case PT_NOT_RLIKE:
    case PT_RLIKE_BINARY:
    case PT_NOT_RLIKE_BINARY:
      num = 0;

      /* two overloads */

      /* BOOL PT_RLIKE([VAR]CHAR, [VAR]CHAR, INT); */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_CHAR;

      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_CHAR;

      sig.arg3_type.is_generic = false;
      sig.arg3_type.val.type = PT_TYPE_INTEGER;

      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_LOGICAL;

      def->overloads[num++] = sig;

      /* BOOL PT_RLIKE([VAR]NCHAR, [VAR]NCHAR, INT); */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_NCHAR;

      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_NCHAR;

      sig.arg3_type.is_generic = false;
      sig.arg3_type.val.type = PT_TYPE_INTEGER;

      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_LOGICAL;

      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_CEIL:
    case PT_FLOOR:
      num = 0;

      /* one overload */

      /* arg1 */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_NUMBER;

      /* return type */
      sig.return_type.is_generic = true;
      sig.return_type.val.generic_type = PT_GENERIC_TYPE_NUMBER;

      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_CHR:
      num = 0;

      /* one overload */

      /* arg1 */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_NUMBER;

      /* arg2 */
      sig.arg2_type.is_generic = false;
      sig.arg2_type.val.type = PT_TYPE_INTEGER;

      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_VARCHAR;

      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_CHAR_LENGTH:
      num = 0;

      /* one overload */

      /* arg1 */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_STRING;

      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_INTEGER;

      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_REPEAT:
      num = 0;

      /* one overload */

      /* arg1 */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_STRING;

      /* arg2 */
      sig.arg2_type.is_generic = false;
      sig.arg2_type.val.type = PT_TYPE_INTEGER;

      /* return type */
      sig.return_type.is_generic = true;
      sig.return_type.val.generic_type = PT_GENERIC_TYPE_STRING_VARYING;

      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_ADD_MONTHS:
      num = 0;

      /* one overload */
      /* arg1 */
      sig.arg1_type.is_generic = false;
      sig.arg1_type.val.type = PT_TYPE_DATE;

      /* arg2 */
      sig.arg2_type.is_generic = false;
      sig.arg2_type.val.type = PT_TYPE_INTEGER;

      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_DATE;

      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_FROMDAYS:
      num = 0;

      /* two overload */

      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_STRING_VARYING;
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_DATE;
      def->overloads[num++] = sig;

      sig.arg1_type.is_generic = false;
      sig.arg1_type.val.type = PT_TYPE_INTEGER;
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_DATE;
      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_LOWER:
    case PT_UPPER:
      num = 0;

      /* one overload */

      /* arg1 */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_STRING;

      /* return type */
      sig.return_type.is_generic = true;
      sig.return_type.val.generic_type = PT_GENERIC_TYPE_STRING;

      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_HEX:
      num = 0;

      /* three overloads */

      /* HEX (STRING) */
      /* arg1 */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_STRING;
      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_VARCHAR;
      def->overloads[num++] = sig;

      /* HEX (NUMBER) */
      /* arg1 */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_NUMBER;
      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_VARCHAR;
      def->overloads[num++] = sig;

      /* HEX (BIT) */
      /* arg1 */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_BIT;
      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_VARCHAR;
      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_ASCII:
      num = 0;

      /* two overloads */

      /* ASCII (STRING) */
      /* arg1 */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_STRING;
      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_SMALLINT;
      def->overloads[num++] = sig;

      /* ASCII (BIT) */
      /* arg1 */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_BIT;
      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_SMALLINT;
      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_CONV:
      num = 0;

      /* three overloads */

      /* CONV(NUMBER, SMALLINT, SMALLINT) */
      /* arg1 */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_NUMBER;
      /* arg2 */
      sig.arg2_type.is_generic = false;
      sig.arg2_type.val.type = PT_TYPE_SMALLINT;
      /* arg3 */
      sig.arg3_type.is_generic = false;
      sig.arg3_type.val.type = PT_TYPE_SMALLINT;
      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_VARCHAR;
      def->overloads[num++] = sig;

      /* CONV(VARCHAR, SMALLINT, SMALLINT) */
      /* arg1 */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_STRING;
      /* arg2 */
      sig.arg2_type.is_generic = false;
      sig.arg2_type.val.type = PT_TYPE_SMALLINT;
      /* arg3 */
      sig.arg3_type.is_generic = false;
      sig.arg3_type.val.type = PT_TYPE_SMALLINT;
      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_VARCHAR;
      def->overloads[num++] = sig;

      /* CONV(BIT, SMALLINT, SMALLINT) */
      /* arg1 */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_BIT;
      /* arg2 */
      sig.arg2_type.is_generic = false;
      sig.arg2_type.val.type = PT_TYPE_SMALLINT;
      /* arg3 */
      sig.arg3_type.is_generic = false;
      sig.arg3_type.val.type = PT_TYPE_SMALLINT;
      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_VARCHAR;
      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_DATEF:
    case PT_REVERSE:
      num = 0;

      /* one overload */

      /* arg1 */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_STRING;

      /* return type */
      sig.return_type.is_generic = true;
      sig.return_type.val.generic_type = PT_GENERIC_TYPE_STRING_VARYING;

      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_LIKE_LOWER_BOUND:
    case PT_LIKE_UPPER_BOUND:
      num = 0;

      /* two overloads */

      /* arg1 */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_CHAR;
      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_VARCHAR;
      def->overloads[num++] = sig;

      /* arg1 */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_CHAR;
      /* arg2 */
      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_CHAR;
      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_VARCHAR;
      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_BIN:
      num = 0;

      /* one overload */

      /* arg1 */
      sig.arg1_type.is_generic = false;
      sig.arg1_type.val.type = PT_TYPE_BIGINT;
      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_VARCHAR;
      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_ADDTIME:
      num = 0;

      /* 3 overloads */

      /* arg1 */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_STRING;
      /* arg2 */
      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_STRING;
      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_VARCHAR;
      def->overloads[num++] = sig;

      /* arg1 */
      sig.arg1_type.is_generic = false;
      sig.arg1_type.val.type = PT_TYPE_DATETIME;
      /* arg2 */
      sig.arg2_type.is_generic = false;
      sig.arg2_type.val.type = PT_TYPE_TIME;
      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_DATETIME;
      def->overloads[num++] = sig;

      /* arg1 */
      sig.arg1_type.is_generic = false;
      sig.arg1_type.val.type = PT_TYPE_TIMESTAMP;
      /* arg2 */
      sig.arg2_type.is_generic = false;
      sig.arg2_type.val.type = PT_TYPE_TIME;
      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_DATETIME;
      def->overloads[num++] = sig;

      /* arg1 */
      sig.arg1_type.is_generic = false;
      sig.arg1_type.val.type = PT_TYPE_DATE;
      /* arg2 */
      sig.arg2_type.is_generic = false;
      sig.arg2_type.val.type = PT_TYPE_TIME;
      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_DATETIME;
      def->overloads[num++] = sig;

      /* arg1 */
      sig.arg1_type.is_generic = false;
      sig.arg1_type.val.type = PT_TYPE_TIME;
      /* arg2 */
      sig.arg2_type.is_generic = false;
      sig.arg2_type.val.type = PT_TYPE_TIME;
      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_TIME;
      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_TRIM:
    case PT_LTRIM:
    case PT_RTRIM:
      num = 0;

      /* two overloads */

      /* arg1 */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_STRING;
      /* return type */
      sig.return_type.is_generic = true;
      sig.return_type.val.generic_type = PT_GENERIC_TYPE_STRING_VARYING;
      def->overloads[num++] = sig;

      /* arg1 */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_STRING;
      /* arg2 */
      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_STRING;
      /* return type */
      sig.return_type.is_generic = true;
      sig.return_type.val.generic_type = PT_GENERIC_TYPE_STRING_VARYING;
      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_MAKEDATE:
      num = 0;

      /* arg1 */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_STRING_VARYING;
      /* arg2 */
      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_STRING_VARYING;
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_DATE;
      def->overloads[num++] = sig;

      /* arg1 */
      sig.arg1_type.is_generic = false;
      sig.arg1_type.val.type = PT_TYPE_INTEGER;
      /* arg2 */
      sig.arg2_type.is_generic = false;
      sig.arg2_type.val.type = PT_TYPE_INTEGER;
      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_DATE;
      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_MAKETIME:
      num = 0;

      /* two overloads */

      /* arg1 */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_STRING_VARYING;
      /* arg2 */
      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_STRING_VARYING;
      /* arg3 */
      sig.arg3_type.is_generic = true;
      sig.arg3_type.val.generic_type = PT_GENERIC_TYPE_STRING_VARYING;
      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_TIME;
      def->overloads[num++] = sig;

      /* arg1 */
      sig.arg1_type.is_generic = false;
      sig.arg1_type.val.type = PT_TYPE_INTEGER;
      /* arg2 */
      sig.arg2_type.is_generic = false;
      sig.arg2_type.val.type = PT_TYPE_INTEGER;
      /* arg3 */
      sig.arg3_type.is_generic = false;
      sig.arg3_type.val.type = PT_TYPE_INTEGER;
      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_TIME;
      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_SECTOTIME:
      num = 0;

      /* two overloads */

      /* arg1 */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_STRING_VARYING;
      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_TIME;
      def->overloads[num++] = sig;

      /* arg1 */
      sig.arg1_type.is_generic = false;
      sig.arg1_type.val.type = PT_TYPE_INTEGER;
      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_TIME;
      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_YEARF:
    case PT_DAYF:
    case PT_MONTHF:
    case PT_DAYOFMONTH:
    case PT_DAYOFWEEK:
    case PT_DAYOFYEAR:
    case PT_QUARTERF:
    case PT_TODAYS:
    case PT_WEEKDAY:
      num = 0;

      /* two overloads */

      /* arg1 */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_STRING_VARYING;
      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_INTEGER;
      def->overloads[num++] = sig;

      /* arg1 */
      sig.arg1_type.is_generic = false;
      sig.arg1_type.val.type = PT_TYPE_DATE;
      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_INTEGER;
      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_LAST_DAY:
      num = 0;

      /* one overload */

      /* arg1 */
      sig.arg1_type.is_generic = false;
      sig.arg1_type.val.type = PT_TYPE_DATE;

      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_DATE;

      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_CONCAT:
    case PT_SYS_CONNECT_BY_PATH:
      num = 0;

      /* two overloads */

      /* arg1 */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_CHAR;
      /* arg2 */
      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_CHAR;
      /* return type */
      sig.return_type.is_generic = true;
      sig.return_type.val.generic_type = PT_GENERIC_TYPE_CHAR;
      def->overloads[num++] = sig;

      /* arg1 */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_NCHAR;
      /* arg2 */
      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_NCHAR;
      /* return type */
      sig.return_type.is_generic = true;
      sig.return_type.val.generic_type = PT_GENERIC_TYPE_NCHAR;
      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_CONCAT_WS:
      num = 0;

      /* two overloads */

      /* arg1 */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_CHAR;
      /* arg2 */
      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_CHAR;
      /* arg2 */
      sig.arg3_type.is_generic = true;
      sig.arg3_type.val.generic_type = PT_GENERIC_TYPE_CHAR;
      /* return type */
      sig.return_type.is_generic = true;
      sig.return_type.val.generic_type = PT_GENERIC_TYPE_CHAR;
      def->overloads[num++] = sig;

      /* arg1 */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_NCHAR;
      /* arg2 */
      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_NCHAR;
      /* arg2 */
      sig.arg3_type.is_generic = true;
      sig.arg3_type.val.generic_type = PT_GENERIC_TYPE_NCHAR;
      /* return type */
      sig.return_type.is_generic = true;
      sig.return_type.val.generic_type = PT_GENERIC_TYPE_NCHAR;
      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_DATABASE:
    case PT_SCHEMA:
    case PT_VERSION:
    case PT_CURRENT_USER:
    case PT_LIST_DBS:
    case PT_USER:
      num = 0;

      /* one overload */

      /* no arguments, just a return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_VARCHAR;

      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_LOCAL_TRANSACTION_ID:
      num = 0;

      /* one overload */

      /* no arguments, just a return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_INTEGER;

      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_CURRENT_VALUE:
      num = 0;

      /* one overload */

      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_STRING;

      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_NUMERIC;

      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_NEXT_VALUE:
      num = 0;

      /* one overload */

      /* arg1 */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_STRING;

      /* arg2 */
      sig.arg2_type.is_generic = false;
      sig.arg2_type.val.type = PT_TYPE_INTEGER;

      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_NUMERIC;

      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_DATE_FORMAT:
      num = 0;

      /* two overloads */

      /* arg1 */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_STRING;
      /* arg2 */
      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_STRING;
      /* arg3 */
      sig.arg3_type.is_generic = false;
      sig.arg3_type.val.type = PT_TYPE_INTEGER;
      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_VARCHAR;
      def->overloads[num++] = sig;

      /* arg1 */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_DATE;
      /* arg2 */
      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_STRING;
      /* arg3 */
      sig.arg3_type.is_generic = false;
      sig.arg3_type.val.type = PT_TYPE_INTEGER;
      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_VARCHAR;
      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_DIV:
    case PT_MOD:
      num = 0;

      /* one overload */

      /* arg1 */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_DISCRETE_NUMBER;
      /* arg2 */
      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_DISCRETE_NUMBER;

      sig.return_type.is_generic = true;
      sig.return_type.val.generic_type = PT_GENERIC_TYPE_DISCRETE_NUMBER;
      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_DIVIDE:
    case PT_MODULUS:
      num = 0;

      /* one overload */

      /* arg1 */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_NUMBER;
      /* arg2 */
      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_NUMBER;

      sig.return_type.is_generic = true;
      sig.return_type.val.generic_type = PT_GENERIC_TYPE_NUMBER;
      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_TIMES:
      num = 0;

      /* two overloads */

      /* arg1 */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_NUMBER;
      /* arg2 */
      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_NUMBER;

      sig.return_type.is_generic = true;
      sig.return_type.val.generic_type = PT_GENERIC_TYPE_NUMBER;
      def->overloads[num++] = sig;

      /* arg1 */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_SEQUENCE;
      /* arg2 */
      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_SEQUENCE;

      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_MULTISET;
      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_PLUS:
      num = 0;

      /* number + number */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_NUMBER;
      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_NUMBER;
      sig.return_type.is_generic = true;
      sig.return_type.val.generic_type = PT_GENERIC_TYPE_NUMBER;
      def->overloads[num++] = sig;

      if (prm_get_bool_value (PRM_ID_PLUS_AS_CONCAT))
	{
	  /* char + char */
	  sig.arg1_type.is_generic = true;
	  sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_CHAR;
	  sig.arg2_type.is_generic = true;
	  sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_CHAR;
	  sig.return_type.is_generic = true;
	  sig.return_type.val.generic_type = PT_GENERIC_TYPE_CHAR;
	  def->overloads[num++] = sig;

	  /* nchar + nchar */
	  sig.arg1_type.is_generic = true;
	  sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_NCHAR;
	  sig.arg2_type.is_generic = true;
	  sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_NCHAR;
	  sig.return_type.is_generic = true;
	  sig.return_type.val.generic_type = PT_GENERIC_TYPE_NCHAR;
	  def->overloads[num++] = sig;

	  /* bit + bit */
	  sig.arg1_type.is_generic = true;
	  sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_BIT;
	  sig.arg2_type.is_generic = true;
	  sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_BIT;
	  sig.return_type.is_generic = true;
	  sig.return_type.val.generic_type = PT_GENERIC_TYPE_BIT;
	  def->overloads[num++] = sig;
	}

      /* collection + collection */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_SEQUENCE;
      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_SEQUENCE;
      sig.return_type.is_generic = true;
      sig.return_type.val.generic_type = PT_GENERIC_TYPE_SEQUENCE;
      def->overloads[num++] = sig;

      def->overloads_count = num;

      break;

    case PT_MINUS:
      num = 0;

      /* four overloads */

      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_NUMBER;
      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_NUMBER;
      sig.return_type.is_generic = true;
      sig.return_type.val.generic_type = PT_GENERIC_TYPE_NUMBER;
      def->overloads[num++] = sig;

      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_DATE;
      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_DATE;
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_BIGINT;
      def->overloads[num++] = sig;

      sig.arg1_type.is_generic = false;
      sig.arg1_type.val.type = PT_TYPE_TIME;
      sig.arg2_type.is_generic = false;
      sig.arg2_type.val.type = PT_TYPE_TIME;
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_BIGINT;
      def->overloads[num++] = sig;

      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_SEQUENCE;
      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_SEQUENCE;
      sig.return_type.is_generic = true;
      sig.return_type.val.generic_type = PT_GENERIC_TYPE_SEQUENCE;
      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_HOURF:
    case PT_MINUTEF:
    case PT_SECONDF:
    case PT_TIMETOSEC:
      num = 0;

      /* two overloads */

      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_STRING_VARYING;
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_INTEGER;
      def->overloads[num++] = sig;

      sig.arg1_type.is_generic = false;
      sig.arg1_type.val.type = PT_TYPE_TIME;
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_INTEGER;
      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_INSTR:
      num = 0;

      /* two overloads */

      /* arg1 */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_CHAR;
      /* arg2 */
      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_CHAR;
      /* arg3 */
      sig.arg3_type.is_generic = false;
      sig.arg3_type.val.type = PT_TYPE_INTEGER;
      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_INTEGER;
      def->overloads[num++] = sig;

      /* arg1 */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_NCHAR;
      /* arg2 */
      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_NCHAR;
      /* arg3 */
      sig.arg3_type.is_generic = false;
      sig.arg3_type.val.type = PT_TYPE_INTEGER;
      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_INTEGER;
      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_LEFT:
    case PT_RIGHT:
      num = 0;

      /* two overloads */

      /* arg1 */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_CHAR;
      /* arg2 */
      sig.arg2_type.is_generic = false;
      sig.arg2_type.val.type = PT_TYPE_INTEGER;

      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_VARCHAR;
      def->overloads[num++] = sig;

      /* arg1 */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_NCHAR;
      /* arg2 */
      sig.arg2_type.is_generic = false;
      sig.arg2_type.val.type = PT_TYPE_INTEGER;

      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_VARNCHAR;
      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_LOCATE:
      num = 0;

      /* four overloads */

      /* arg1 */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_CHAR;
      /* arg2 */
      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_CHAR;
      /* arg3 */
      sig.arg3_type.is_generic = false;
      sig.arg3_type.val.type = PT_TYPE_NONE;

      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_INTEGER;
      def->overloads[num++] = sig;

      /* arg1 */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_NCHAR;
      /* arg2 */
      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_NCHAR;
      /* arg3 */
      sig.arg3_type.is_generic = false;
      sig.arg3_type.val.type = PT_TYPE_NONE;

      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_INTEGER;
      def->overloads[num++] = sig;

      /* arg1 */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_CHAR;
      /* arg2 */
      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_CHAR;
      /* arg3 */
      sig.arg3_type.is_generic = false;
      sig.arg3_type.val.type = PT_TYPE_INTEGER;

      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_INTEGER;
      def->overloads[num++] = sig;

      /* arg1 */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_NCHAR;
      /* arg2 */
      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_NCHAR;
      /* arg3 */
      sig.arg3_type.is_generic = false;
      sig.arg3_type.val.type = PT_TYPE_INTEGER;

      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_INTEGER;
      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_POSITION:
    case PT_STRCMP:
    case PT_FINDINSET:
      num = 0;

      /* two overloads */

      /* arg1 */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_CHAR;
      /* arg2 */
      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_CHAR;

      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_INTEGER;
      def->overloads[num++] = sig;

      /* arg1 */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_NCHAR;
      /* arg2 */
      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_NCHAR;

      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_INTEGER;
      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_SUBSTRING_INDEX:
      num = 0;

      /* two overloads */

      /* arg1 */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_CHAR;
      /* arg2 */
      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_CHAR;
      /* arg3 */
      sig.arg3_type.is_generic = false;
      sig.arg3_type.val.type = PT_TYPE_INTEGER;

      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_VARCHAR;
      def->overloads[num++] = sig;

      /* arg1 */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_NCHAR;
      /* arg2 */
      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_NCHAR;
      /* arg3 */
      sig.arg3_type.is_generic = false;
      sig.arg3_type.val.type = PT_TYPE_INTEGER;

      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_VARNCHAR;
      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_LPAD:
    case PT_RPAD:
      num = 0;

      /* two overloads */

      /* arg1 */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_CHAR;

      /* arg2 */
      sig.arg2_type.is_generic = false;
      sig.arg2_type.val.generic_type = PT_TYPE_INTEGER;

      /* arg3 */
      sig.arg3_type.is_generic = true;
      sig.arg3_type.val.generic_type = PT_GENERIC_TYPE_CHAR;

      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_VARCHAR;
      def->overloads[num++] = sig;

      /* arg1 */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_NCHAR;

      /* arg2 */
      sig.arg2_type.is_generic = false;
      sig.arg2_type.val.generic_type = PT_TYPE_INTEGER;

      /* arg3 */
      sig.arg3_type.is_generic = true;
      sig.arg3_type.val.generic_type = PT_GENERIC_TYPE_NCHAR;

      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_VARNCHAR;
      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_MD5:
      num = 0;

      /* one overload */

      /* arg1 */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_STRING;

      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_CHAR;
      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_MID:
      num = 0;

      /* one overload */

      /* arg1 */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_STRING;

      /* arg2 */
      sig.arg2_type.is_generic = false;
      sig.arg2_type.val.type = PT_TYPE_INTEGER;

      /* arg3 */
      sig.arg3_type.is_generic = false;
      sig.arg3_type.val.type = PT_TYPE_INTEGER;

      /* return type */
      sig.return_type.is_generic = true;
      sig.return_type.val.generic_type = PT_GENERIC_TYPE_STRING_VARYING;
      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_SUBSTRING:
      num = 0;

      /* two overloads */

      /* SUBSTRING (string, int, int) */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_STRING;
      sig.arg2_type.is_generic = false;
      sig.arg2_type.val.type = PT_TYPE_INTEGER;
      sig.arg3_type.is_generic = false;
      sig.arg3_type.val.type = PT_TYPE_INTEGER;
      sig.return_type.is_generic = true;
      sig.return_type.val.generic_type = PT_GENERIC_TYPE_STRING_VARYING;
      def->overloads[num++] = sig;

      /* SUBSTRING (string, int) */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_STRING;
      sig.arg2_type.is_generic = false;
      sig.arg2_type.val.type = PT_TYPE_INTEGER;
      sig.arg3_type.is_generic = false;
      sig.arg3_type.val.type = PT_TYPE_NONE;
      sig.return_type.is_generic = true;
      sig.return_type.val.generic_type = PT_GENERIC_TYPE_STRING_VARYING;
      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_MONTHS_BETWEEN:
      num = 0;

      /* one overload */

      /* arg1 */
      sig.arg1_type.is_generic = false;
      sig.arg1_type.val.type = PT_TYPE_DATE;

      /* arg2 */
      sig.arg2_type.is_generic = false;
      sig.arg2_type.val.type = PT_TYPE_DATE;

      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_DOUBLE;
      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_PI:
      num = 0;

      /* one overload */

      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_DOUBLE;
      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_REPLACE:
    case PT_TRANSLATE:
      num = 0;

      /* two overloads */

      /* arg1 */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_CHAR;
      /* arg2 */
      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_CHAR;
      /* arg3 */
      sig.arg3_type.is_generic = true;
      sig.arg3_type.val.generic_type = PT_GENERIC_TYPE_CHAR;
      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_VARCHAR;
      def->overloads[num++] = sig;

      /* arg1 */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_NCHAR;
      /* arg2 */
      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_NCHAR;
      /* arg3 */
      sig.arg3_type.is_generic = true;
      sig.arg3_type.val.generic_type = PT_GENERIC_TYPE_NCHAR;
      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_VARNCHAR;
      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_SPACE:
      num = 0;

      /* one overload */

      /* arg1 */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_DISCRETE_NUMBER;

      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_VARCHAR;
      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_STRCAT:
      num = 0;

      /* two overloads */

      /* arg1 */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_CHAR;

      /* arg2 */
      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_CHAR;

      /* return type */
      sig.return_type.is_generic = true;
      sig.return_type.val.generic_type = PT_GENERIC_TYPE_CHAR;
      def->overloads[num++] = sig;

      /* arg1 */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_NCHAR;

      /* arg2 */
      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_NCHAR;

      /* return type */
      sig.return_type.is_generic = true;
      sig.return_type.val.generic_type = PT_GENERIC_TYPE_NCHAR;
      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_UTC_DATE:
    case PT_SYS_DATE:
      num = 0;

      /* one overload */

      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_DATE;
      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_SYS_DATETIME:
      num = 0;

      /* one overload */

      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_DATETIME;
      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_UTC_TIME:
    case PT_SYS_TIME:
      num = 0;

      /* one overload */

      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_TIME;
      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_SYS_TIMESTAMP:
      num = 0;

      /* one overload */

      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_TIMESTAMP;
      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_TIME_FORMAT:
      num = 0;

      /* two overloads */

      /* arg1 */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_CHAR;
      /* arg2 */
      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_STRING;
      /* arg3 */
      sig.arg3_type.is_generic = false;
      sig.arg3_type.val.type = PT_TYPE_INTEGER;
      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_VARCHAR;
      def->overloads[num++] = sig;

      /* arg1 */
      sig.arg1_type.is_generic = false;
      sig.arg1_type.val.type = PT_TYPE_TIME;
      /* arg2 */
      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_STRING;
      /* arg3 */
      sig.arg3_type.is_generic = false;
      sig.arg3_type.val.type = PT_TYPE_INTEGER;
      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_VARCHAR;
      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_TIMEF:
      num = 0;

      /* four overloads */

      /* arg1 */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_STRING;
      /* return type */
      sig.return_type.is_generic = true;
      sig.return_type.val.generic_type = PT_GENERIC_TYPE_STRING_VARYING;
      def->overloads[num++] = sig;

      /* arg1 */
      sig.arg1_type.is_generic = false;
      sig.arg1_type.val.type = PT_TYPE_TIME;
      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_VARCHAR;
      def->overloads[num++] = sig;

      /* arg1 */
      sig.arg1_type.is_generic = false;
      sig.arg1_type.val.type = PT_TYPE_DATETIME;
      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_VARCHAR;
      def->overloads[num++] = sig;

      /* arg1 */
      sig.arg1_type.is_generic = false;
      sig.arg1_type.val.type = PT_TYPE_TIMESTAMP;
      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_VARCHAR;
      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_TO_DATE:
      num = 0;

      /* one overload */

      /* arg1 */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_STRING;
      /* arg2 */
      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_STRING;
      /* arg3 */
      sig.arg3_type.is_generic = false;
      sig.arg3_type.val.type = PT_TYPE_INTEGER;

      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_DATE;
      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_TO_DATETIME:
      num = 0;

      /* two overloads */

      /* arg1 */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_CHAR;
      /* arg2 */
      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_CHAR;
      /* arg3 */
      sig.arg3_type.is_generic = false;
      sig.arg3_type.val.type = PT_TYPE_INTEGER;

      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_DATETIME;
      def->overloads[num++] = sig;

      /* arg1 */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_NCHAR;
      /* arg2 */
      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_NCHAR;
      /* arg3 */
      sig.arg3_type.is_generic = false;
      sig.arg3_type.val.type = PT_TYPE_INTEGER;

      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_DATETIME;
      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_TO_TIME:
      num = 0;

      /* two overloads */

      /* arg1 */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_CHAR;
      /* arg2 */
      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_CHAR;
      /* arg3 */
      sig.arg3_type.is_generic = false;
      sig.arg3_type.val.type = PT_TYPE_INTEGER;

      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_TIME;
      def->overloads[num++] = sig;

      /* arg1 */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_NCHAR;
      /* arg2 */
      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_NCHAR;
      /* arg3 */
      sig.arg3_type.is_generic = false;
      sig.arg3_type.val.type = PT_TYPE_INTEGER;

      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_TIME;
      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_TO_TIMESTAMP:
      num = 0;

      /* two overloads */

      /* arg1 */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_CHAR;
      /* arg2 */
      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_CHAR;
      /* arg3 */
      sig.arg3_type.is_generic = false;
      sig.arg3_type.val.type = PT_TYPE_INTEGER;

      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_TIMESTAMP;
      def->overloads[num++] = sig;

      /* arg1 */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_NCHAR;
      /* arg2 */
      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_NCHAR;
      /* arg3 */
      sig.arg3_type.is_generic = false;
      sig.arg3_type.val.type = PT_TYPE_INTEGER;

      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_TIMESTAMP;
      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_TO_NUMBER:
      num = 0;

      /* one overload */

      /* arg1 */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_STRING;
      /* arg2 */
      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_STRING;
      /* arg3 */
      sig.arg3_type.is_generic = false;
      sig.arg3_type.val.type = PT_TYPE_INTEGER;

      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_NUMERIC;
      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_WEEKF:
      num = 0;

      /* two overloads */

      /* arg1 */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.type = PT_GENERIC_TYPE_STRING_VARYING;
      /* arg2 */
      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.type = PT_GENERIC_TYPE_STRING_VARYING;
      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_INTEGER;
      def->overloads[num++] = sig;

      /* arg1 */
      sig.arg1_type.is_generic = false;
      sig.arg1_type.val.type = PT_TYPE_DATE;
      /* arg2 */
      sig.arg2_type.is_generic = false;
      sig.arg2_type.val.type = PT_TYPE_INTEGER;
      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_INTEGER;
      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_CLOB_LENGTH:
      num = 0;

      /* one overload */

      /* arg1 */
      sig.arg1_type.is_generic = false;
      sig.arg1_type.val.type = PT_TYPE_CLOB;

      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_BIGINT;
      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_BLOB_LENGTH:
      num = 0;

      /* one overload */

      /* arg1 */
      sig.arg1_type.is_generic = false;
      sig.arg1_type.val.type = PT_TYPE_BLOB;

      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_BIGINT;
      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_BIT_TO_BLOB:
      num = 0;

      /* one overload */

      /* arg1 */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_BIT;

      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_BLOB;
      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_CHAR_TO_CLOB:
      num = 0;

      /* one overload */

      /* arg1 */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_STRING;

      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_CLOB;
      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_CHAR_TO_BLOB:
      num = 0;

      /* one overload */

      /* arg1 */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_STRING;

      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_BLOB;
      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_BLOB_FROM_FILE:
      num = 0;

      /* one overload */

      /* arg1 */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_STRING;

      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_BLOB;
      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_CLOB_FROM_FILE:
      num = 0;

      /* one overload */

      /* arg1 */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_STRING;

      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_CLOB;
      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_BLOB_TO_BIT:
      num = 0;

      /* one overload */

      /* arg1 */
      sig.arg1_type.is_generic = false;
      sig.arg1_type.val.type = PT_TYPE_BLOB;

      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_VARBIT;
      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_CLOB_TO_CHAR:
      num = 0;

      /* one overload */

      /* arg1 */
      sig.arg1_type.is_generic = false;
      sig.arg1_type.val.type = PT_TYPE_CLOB;

      /* arg2 */
      sig.arg2_type.is_generic = false;
      sig.arg2_type.val.type = PT_TYPE_INTEGER;

      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_VARCHAR;
      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_INST_NUM:
    case PT_ROWNUM:
    case PT_ORDERBY_NUM:
      num = 0;

      /* one overload */

      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_BIGINT;
      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_LEVEL:
    case PT_CONNECT_BY_ISCYCLE:
    case PT_CONNECT_BY_ISLEAF:
    case PT_ROW_COUNT:
      num = 0;

      /* one overload */

      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_INTEGER;
      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_LAST_INSERT_ID:
      num = 0;

      /* one overload */

      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_NUMERIC;
      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_DATEDIFF:
      num = 0;

      /* one overload */

      sig.arg1_type.is_generic = false;
      sig.arg1_type.val.type = PT_TYPE_DATE;

      sig.arg2_type.is_generic = false;
      sig.arg2_type.val.type = PT_TYPE_DATE;

      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_INTEGER;
      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_TIMEDIFF:
      num = 0;

      /* three overloads */

      /* arg1 */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_STRING;
      /* arg2 */
      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_STRING;
      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_TIME;
      def->overloads[num++] = sig;

      /* arg1 */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_DATE;
      /* arg2 */
      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_DATE;
      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_TIME;
      def->overloads[num++] = sig;

      /* arg1 */
      sig.arg1_type.is_generic = false;
      sig.arg1_type.val.type = PT_TYPE_TIME;
      /* arg2 */
      sig.arg2_type.is_generic = false;
      sig.arg2_type.val.type = PT_TYPE_TIME;
      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_TIME;
      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_INCR:
    case PT_DECR:
      num = 0;

      /* two overloads */

      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_DISCRETE_NUMBER;
      /* return type */
      sig.return_type.is_generic = true;
      sig.return_type.val.generic_type = PT_GENERIC_TYPE_DISCRETE_NUMBER;
      def->overloads[num++] = sig;

      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_DISCRETE_NUMBER;
      sig.arg2_type.is_generic = false;
      sig.arg2_type.val.type = PT_TYPE_OBJECT;
      sig.arg3_type.is_generic = true;
      sig.arg3_type.val.generic_type = PT_GENERIC_TYPE_DISCRETE_NUMBER;
      /* return type */
      sig.return_type.is_generic = true;
      sig.return_type.val.generic_type = PT_GENERIC_TYPE_DISCRETE_NUMBER;
      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_FORMAT:
      num = 0;

      /* one overload */

      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_NUMBER;

      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_DISCRETE_NUMBER;

      sig.arg3_type.is_generic = false;
      sig.arg3_type.val.type = PT_TYPE_INTEGER;

      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_VARCHAR;
      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_ROUND:
      num = 0;
      /* five overloads */
      /* first overload for number: */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_NUMBER;

      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_NUMBER;

      /* return type */
      sig.return_type.is_generic = true;
      sig.return_type.val.generic_type = PT_GENERIC_TYPE_NUMBER;

      def->overloads[num++] = sig;

      /* overload for round('123', '1') */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_NUMBER;

      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_STRING;

      /* return type */
      sig.return_type.is_generic = true;
      sig.return_type.val.generic_type = PT_GENERIC_TYPE_NUMBER;

      def->overloads[num++] = sig;

      /* overload for round(date, 'year|month|day') */
      sig.arg1_type.is_generic = false;
      sig.arg1_type.val.type = PT_TYPE_DATE;

      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_STRING;

      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_DATE;

      def->overloads[num++] = sig;

      /* overload for round(datetime, 'year|month|day') */
      sig.arg1_type.is_generic = false;
      sig.arg1_type.val.type = PT_TYPE_DATETIME;

      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_STRING;

      /*return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_DATE;

      def->overloads[num++] = sig;

      /* overload for round(timestamp, 'year|month|day') */
      sig.arg1_type.is_generic = false;
      sig.arg1_type.val.type = PT_TYPE_TIMESTAMP;

      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_STRING;

      /*return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_DATE;

      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_TRUNC:
      num = 0;

      /* five overloads */

      /* number types */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_NUMBER;

      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.type = PT_GENERIC_TYPE_NUMBER;

      /* return type */
      sig.return_type.is_generic = true;
      sig.return_type.val.generic_type = PT_GENERIC_TYPE_NUMBER;
      def->overloads[num++] = sig;

      /* number types 2 */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_NUMBER;

      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.type = PT_GENERIC_TYPE_STRING;

      /* return type */
      sig.return_type.is_generic = true;
      sig.return_type.val.generic_type = PT_GENERIC_TYPE_NUMBER;
      def->overloads[num++] = sig;

      /* date */
      sig.arg1_type.is_generic = false;
      sig.arg1_type.val.generic_type = PT_TYPE_DATE;

      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.type = PT_GENERIC_TYPE_STRING;

      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.generic_type = PT_TYPE_DATE;
      def->overloads[num++] = sig;

      /* datetime */
      sig.arg1_type.is_generic = false;
      sig.arg1_type.val.generic_type = PT_TYPE_DATETIME;

      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.type = PT_GENERIC_TYPE_STRING;

      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.generic_type = PT_TYPE_DATE;
      def->overloads[num++] = sig;

      /* timestamp */
      sig.arg1_type.is_generic = false;
      sig.arg1_type.val.generic_type = PT_TYPE_TIMESTAMP;

      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.type = PT_GENERIC_TYPE_STRING;

      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.generic_type = PT_TYPE_DATE;
      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_DEFAULTF:
      num = 0;

      /* one overload */

      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_ANY;

      /* return type */
      sig.return_type.is_generic = true;
      sig.return_type.val.generic_type = PT_GENERIC_TYPE_ANY;
      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_INDEX_CARDINALITY:
      num = 0;

      /* one overload */

      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_CHAR;
      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_CHAR;
      sig.arg3_type.is_generic = false;
      sig.arg3_type.val.type = PT_TYPE_INTEGER;
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_INTEGER;
      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_SIGN:
      num = 0;

      /* one overload */

      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_NUMBER;

      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_INTEGER;
      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_SETEQ:
    case PT_SETNEQ:
    case PT_SUBSET:
    case PT_SUBSETEQ:
    case PT_SUPERSET:
    case PT_SUPERSETEQ:
      num = 0;

      /* one overload */

      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_SEQUENCE;

      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_SEQUENCE;

      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_LOGICAL;
      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_GE:
    case PT_GT:
    case PT_LT:
    case PT_LE:
      num = 0;

      /* two overloads */

      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_PRIMITIVE;

      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_PRIMITIVE;

      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_LOGICAL;
      def->overloads[num++] = sig;

      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_LOB;

      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_LOB;

      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_LOGICAL;
      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_EQ:
    case PT_NE:
    case PT_NULLSAFE_EQ:
      num = 0;

      /* one overload */

      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_ANY;

      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_ANY;

      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_LOGICAL;
      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_NVL:
    case PT_IFNULL:
    case PT_COALESCE:
      num = 0;

      /* arg1 : generic string , arg2 : generic string */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_STRING;
      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_STRING;
      sig.return_type.is_generic = true;
      sig.return_type.val.type = PT_GENERIC_TYPE_STRING;
      def->overloads[num++] = sig;

      /* arg1 : generic string , arg2 : generic any */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_STRING;
      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_ANY;
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_VARCHAR;
      def->overloads[num++] = sig;

      /* arg1 : generic bit , arg2 : generic bit */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_BIT;
      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_BIT;
      sig.return_type.is_generic = true;
      sig.return_type.val.type = PT_GENERIC_TYPE_BIT;
      def->overloads[num++] = sig;

      /* arg1 : generic bit , arg2 : generic any */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_BIT;
      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_ANY;
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_VARCHAR;
      def->overloads[num++] = sig;

      /* arg1 : generic number , arg2 : generic number */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_NUMBER;
      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_NUMBER;
      sig.return_type.is_generic = true;
      sig.return_type.val.type = PT_GENERIC_TYPE_NUMBER;
      def->overloads[num++] = sig;

      /* arg1 : generic number , arg2 : generic any */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_NUMBER;
      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_ANY;
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_VARCHAR;
      def->overloads[num++] = sig;

      /* arg1 : generic date , arg2 : generic date */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_DATE;
      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_DATE;
      sig.return_type.is_generic = true;
      sig.return_type.val.type = PT_GENERIC_TYPE_DATE;
      def->overloads[num++] = sig;

      /* arg1 : generic date , arg2 : generic any */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_DATE;
      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_ANY;
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_VARCHAR;
      def->overloads[num++] = sig;

      /* arg1 : PT_TYPE_TIME , arg2 : PT_TYPE_TIME */
      sig.arg1_type.is_generic = false;
      sig.arg1_type.val.type = PT_TYPE_TIME;
      sig.arg2_type.is_generic = false;
      sig.arg2_type.val.type = PT_TYPE_TIME;
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_TIME;
      def->overloads[num++] = sig;

      /* arg1 : PT_TYPE_TIME , arg2 : generic any */
      sig.arg1_type.is_generic = false;
      sig.arg1_type.val.type = PT_TYPE_TIME;
      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_ANY;
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_VARCHAR;
      def->overloads[num++] = sig;

      /* arg1 : generic sequence, arg2 : generic sequence */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_SEQUENCE;
      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_SEQUENCE;
      sig.return_type.is_generic = true;
      sig.return_type.val.type = PT_GENERIC_TYPE_SEQUENCE;
      def->overloads[num++] = sig;

      /* arg1 : generic sequence, arg2 : generic type any */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_SEQUENCE;
      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_ANY;
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_VARCHAR;
      def->overloads[num++] = sig;

      /* arg1 : generic lob, arg2 : generic lob */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_LOB;
      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_LOB;
      sig.return_type.is_generic = true;
      sig.return_type.val.type = PT_GENERIC_TYPE_LOB;
      def->overloads[num++] = sig;

      /* arg1 : generic sequence, arg2 : generic type any */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_LOB;
      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_ANY;
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_VARCHAR;
      def->overloads[num++] = sig;

      /* arg1 : generic any, arg2 : generic any */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_ANY;

      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_ANY;

      sig.return_type.is_generic = true;
      sig.return_type.val.generic_type = PT_GENERIC_TYPE_ANY;
      def->overloads[num++] = sig;

      def->overloads_count = num;

      break;

    case PT_NULLIF:
    case PT_LEAST:
    case PT_GREATEST:
      num = 0;

      /* one overload */

      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_ANY;

      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_ANY;

      sig.return_type.is_generic = true;
      sig.return_type.val.generic_type = PT_GENERIC_TYPE_ANY;
      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_NVL2:
      num = 0;

      /* arg1, arg1, arg3 : generic string */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_STRING;
      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_STRING;
      sig.arg3_type.is_generic = true;
      sig.arg3_type.val.generic_type = PT_GENERIC_TYPE_STRING;
      sig.return_type.is_generic = true;
      sig.return_type.val.type = PT_GENERIC_TYPE_STRING;
      def->overloads[num++] = sig;

      /* arg1 : generic string , arg2, arg3 : generic any */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_STRING;
      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_ANY;
      sig.arg3_type.is_generic = true;
      sig.arg3_type.val.generic_type = PT_GENERIC_TYPE_ANY;
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_VARCHAR;
      def->overloads[num++] = sig;

      /* arg1, arg2, arg3 : generic bit */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_BIT;
      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_BIT;
      sig.arg3_type.is_generic = true;
      sig.arg3_type.val.generic_type = PT_GENERIC_TYPE_BIT;
      sig.return_type.is_generic = true;
      sig.return_type.val.type = PT_GENERIC_TYPE_BIT;
      def->overloads[num++] = sig;

      /* arg1 : generic bit , arg2, arg3 : generic any */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_BIT;
      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_ANY;
      sig.arg3_type.is_generic = true;
      sig.arg3_type.val.generic_type = PT_GENERIC_TYPE_ANY;
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_VARCHAR;
      def->overloads[num++] = sig;

      /* arg1, arg2, arg3 : generic number */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_NUMBER;
      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_NUMBER;
      sig.arg3_type.is_generic = true;
      sig.arg3_type.val.generic_type = PT_GENERIC_TYPE_NUMBER;
      sig.return_type.is_generic = true;
      sig.return_type.val.type = PT_GENERIC_TYPE_NUMBER;
      def->overloads[num++] = sig;

      /* arg1 : generic number , arg2, arg3 : generic any */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_NUMBER;
      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_ANY;
      sig.arg3_type.is_generic = true;
      sig.arg3_type.val.generic_type = PT_GENERIC_TYPE_ANY;
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_VARCHAR;
      def->overloads[num++] = sig;

      /* arg1, arg2, arg3 : generic date */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_DATE;
      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_DATE;
      sig.arg3_type.is_generic = true;
      sig.arg3_type.val.generic_type = PT_GENERIC_TYPE_DATE;
      sig.return_type.is_generic = true;
      sig.return_type.val.type = PT_GENERIC_TYPE_DATE;
      def->overloads[num++] = sig;

      /* arg1 : generic date , arg2, arg3 : generic any */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_DATE;
      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_ANY;
      sig.arg3_type.is_generic = true;
      sig.arg3_type.val.generic_type = PT_GENERIC_TYPE_ANY;
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_VARCHAR;
      def->overloads[num++] = sig;

      /* arg1, arg2, arg3 : PT_TYPE_TIME */
      sig.arg1_type.is_generic = false;
      sig.arg1_type.val.type = PT_TYPE_TIME;
      sig.arg2_type.is_generic = false;
      sig.arg2_type.val.type = PT_TYPE_TIME;
      sig.arg3_type.is_generic = false;
      sig.arg3_type.val.type = PT_TYPE_TIME;
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_TIME;
      def->overloads[num++] = sig;

      /* arg1 : PT_TYPE_TIME , arg2, arg3 : generic any */
      sig.arg1_type.is_generic = false;
      sig.arg1_type.val.type = PT_TYPE_TIME;
      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_ANY;
      sig.arg3_type.is_generic = true;
      sig.arg3_type.val.generic_type = PT_GENERIC_TYPE_ANY;
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_VARCHAR;
      def->overloads[num++] = sig;

      /* arg1, arg2, arg3 : generic sequence */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_SEQUENCE;
      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_SEQUENCE;
      sig.arg3_type.is_generic = true;
      sig.arg3_type.val.generic_type = PT_GENERIC_TYPE_SEQUENCE;
      sig.return_type.is_generic = true;
      sig.return_type.val.type = PT_GENERIC_TYPE_SEQUENCE;
      def->overloads[num++] = sig;

      /* arg1 : generic sequence, arg2, arg3 : generic type any */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_SEQUENCE;
      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_ANY;
      sig.arg3_type.is_generic = true;
      sig.arg3_type.val.generic_type = PT_GENERIC_TYPE_ANY;
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_VARCHAR;
      def->overloads[num++] = sig;

      /* arg1, arg2, arg3 : generic lob */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_LOB;
      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_LOB;
      sig.arg3_type.is_generic = true;
      sig.arg3_type.val.generic_type = PT_GENERIC_TYPE_LOB;
      sig.return_type.is_generic = true;
      sig.return_type.val.type = PT_GENERIC_TYPE_LOB;
      def->overloads[num++] = sig;

      /* arg1 : generic lob, arg2, arg3 : generic type any */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_LOB;
      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_ANY;
      sig.arg3_type.is_generic = true;
      sig.arg3_type.val.generic_type = PT_GENERIC_TYPE_ANY;
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_VARCHAR;
      def->overloads[num++] = sig;

      /* arg1, arg2, arg3 : generic any */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_ANY;
      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_ANY;
      sig.arg3_type.is_generic = true;
      sig.arg3_type.val.generic_type = PT_GENERIC_TYPE_ANY;
      sig.return_type.is_generic = true;
      sig.return_type.val.generic_type = PT_GENERIC_TYPE_ANY;
      def->overloads[num++] = sig;

      def->overloads_count = num;

      break;
    case PT_CONNECT_BY_ROOT:
    case PT_PRIOR:
    case PT_QPRIOR:
      num = 0;

      /* one overload */

      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_ANY;

      sig.return_type.is_generic = true;
      sig.return_type.val.type = PT_GENERIC_TYPE_ANY;
      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_UNARY_MINUS:
      num = 0;

      /* one overload */

      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_NUMBER;

      sig.return_type.is_generic = true;
      sig.return_type.val.generic_type = PT_GENERIC_TYPE_NUMBER;
      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_TO_CHAR:
      num = 0;

      /* two overloads */

      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_NUMBER;

      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_STRING;

      sig.arg3_type.is_generic = false;
      sig.arg3_type.val.type = PT_TYPE_INTEGER;

      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_VARCHAR;
      def->overloads[num++] = sig;

      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_DATETIME;

      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_STRING;

      sig.arg3_type.is_generic = false;
      sig.arg3_type.val.type = PT_TYPE_INTEGER;

      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_VARCHAR;
      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_ISNULL:
    case PT_IS_NULL:
    case PT_IS_NOT_NULL:
      num = 0;

      /* one overload */

      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_ANY;

      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_LOGICAL;
      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_IS:
    case PT_IS_NOT:
      num = 0;

      /* one overload */

      sig.arg1_type.is_generic = false;
      sig.arg1_type.val.type = PT_TYPE_LOGICAL;

      sig.arg2_type.is_generic = false;
      sig.arg2_type.val.type = PT_TYPE_LOGICAL;

      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_LOGICAL;
      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_SUBDATE:
    case PT_ADDDATE:
      num = 0;

      /* four overloads */

      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_CHAR;

      sig.arg2_type.is_generic = false;
      sig.arg2_type.val.type = PT_TYPE_INTEGER;

      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_VARCHAR;
      def->overloads[num++] = sig;

      sig.arg1_type.is_generic = false;
      sig.arg1_type.val.type = PT_TYPE_DATE;

      sig.arg2_type.is_generic = false;
      sig.arg2_type.val.type = PT_TYPE_INTEGER;

      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_DATE;
      def->overloads[num++] = sig;

      sig.arg1_type.is_generic = false;
      sig.arg1_type.val.type = PT_TYPE_DATETIME;

      sig.arg2_type.is_generic = false;
      sig.arg2_type.val.type = PT_TYPE_INTEGER;

      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_DATETIME;
      def->overloads[num++] = sig;

      sig.arg1_type.is_generic = false;
      sig.arg1_type.val.type = PT_TYPE_TIMESTAMP;

      sig.arg2_type.is_generic = false;
      sig.arg2_type.val.type = PT_TYPE_INTEGER;

      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_DATETIME;
      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_GE_SOME:
    case PT_GT_SOME:
    case PT_LT_SOME:
    case PT_LE_SOME:
    case PT_GE_ALL:
    case PT_GT_ALL:
    case PT_LT_ALL:
    case PT_LE_ALL:
      num = 0;

      /* two overloads */

      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_PRIMITIVE;

      sig.arg2_type.is_generic = false;
      sig.arg2_type.val.type = PT_TYPE_SET;

      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_LOGICAL;
      def->overloads[num++] = sig;

      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_PRIMITIVE;

      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_QUERY;

      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_LOGICAL;
      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_EQ_SOME:
    case PT_NE_SOME:
    case PT_EQ_ALL:
    case PT_NE_ALL:
    case PT_IS_IN:
    case PT_IS_NOT_IN:
      num = 0;

      /* two overloads */

      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_ANY;

      sig.arg2_type.is_generic = false;
      sig.arg2_type.val.type = PT_TYPE_SET;

      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_LOGICAL;
      def->overloads[num++] = sig;

      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_ANY;

      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_QUERY;

      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_LOGICAL;
      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_BETWEEN_EQ_NA:
    case PT_BETWEEN_GE_INF:
    case PT_BETWEEN_GT_INF:
    case PT_BETWEEN_INF_LE:
    case PT_BETWEEN_INF_LT:
    case PT_LT_INF:
    case PT_GT_INF:
      /* these expressions are introduced during query rewriting and
         are used in the range operator. we should set the return type
         to the type of the argument */

      num = 0;

      /* one overload */

      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_ANY;

      sig.return_type.is_generic = true;
      sig.return_type.val.generic_type = PT_GENERIC_TYPE_ANY;
      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_UNIX_TIMESTAMP:
      num = 0;

      /* three overloads */

      /* UNIX_TIMESTAMP(string) */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_STRING;
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_INTEGER;
      def->overloads[num++] = sig;

      /* UNIX_TIMESTAMP(date/time type) */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_DATE;
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_INTEGER;
      def->overloads[num++] = sig;

      /* UNIX_TIMESTAMP (void) */
      sig.arg1_type.is_generic = false;
      sig.arg1_type.val.type = PT_TYPE_NONE;
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_INTEGER;
      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_FROM_UNIXTIME:
      num = 0;

      /* two overloads */

      sig.arg1_type.is_generic = false;
      sig.arg1_type.val.type = PT_TYPE_INTEGER;

      sig.arg2_type.is_generic = false;
      sig.arg2_type.val.type = PT_TYPE_NONE;

      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_TIMESTAMP;
      def->overloads[num++] = sig;

      sig.arg1_type.is_generic = false;
      sig.arg1_type.val.type = PT_TYPE_INTEGER;

      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_STRING;

      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_VARCHAR;
      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_TIMESTAMP:
      num = 0;

      /* eight overloads */

      /* TIMESTAMP(STRING) */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_STRING;
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_DATETIME;
      def->overloads[num++] = sig;

      /* TIMESTAMP(DATETIME) */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_DATE;
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_DATETIME;
      def->overloads[num++] = sig;

      /* TIMESTAMP(STRING,STRING) */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_STRING;
      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_STRING;
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_DATETIME;
      def->overloads[num++] = sig;

      /* TIMESTAMP(STRING,NUMBER) */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_STRING;
      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_NUMBER;
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_DATETIME;
      def->overloads[num++] = sig;


      /* TIMESTAMP(STRING,TIME) */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_STRING;
      sig.arg2_type.is_generic = false;
      sig.arg2_type.val.type = PT_TYPE_TIME;
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_DATETIME;
      def->overloads[num++] = sig;

      /* TIMESTAMP(DATETIME,STRING) */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_DATE;
      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_STRING;
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_DATETIME;
      def->overloads[num++] = sig;

      /* TIMESTAMP(DATETIME,TIME) */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_DATE;
      sig.arg2_type.is_generic = false;
      sig.arg2_type.val.type = PT_TYPE_TIME;
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_DATETIME;
      def->overloads[num++] = sig;

      /* TIMESTAMP(DATETIME,NUMBER) */
      sig.arg1_type.is_generic = false;
      sig.arg1_type.val.type = PT_TYPE_DATETIME;
      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_NUMBER;
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_DATETIME;
      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_TYPEOF:
      num = 0;

      /* one overload */

      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_ANY;
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_VARCHAR;
      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_EXTRACT:
      num = 0;

      /* five overloads */

      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.type = PT_GENERIC_TYPE_ANY;
      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_INTEGER;
      def->overloads[num++] = sig;

      sig.arg1_type.is_generic = false;
      sig.arg1_type.val.type = PT_TYPE_TIME;
      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_INTEGER;
      def->overloads[num++] = sig;

      sig.arg1_type.is_generic = false;
      sig.arg1_type.val.type = PT_TYPE_DATE;
      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_INTEGER;
      def->overloads[num++] = sig;

      sig.arg1_type.is_generic = false;
      sig.arg1_type.val.type = PT_TYPE_TIMESTAMP;
      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_INTEGER;
      def->overloads[num++] = sig;

      sig.arg1_type.is_generic = false;
      sig.arg1_type.val.type = PT_TYPE_DATETIME;
      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_INTEGER;
      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_EVALUATE_VARIABLE:
      num = 0;

      /* one overload */

      sig.arg1_type.is_generic = false;
      sig.arg1_type.val.type = PT_TYPE_CHAR;

      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_MAYBE;

      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_DEFINE_VARIABLE:
      num = 0;

      /* two overloads */

      sig.arg1_type.is_generic = false;
      sig.arg1_type.val.type = PT_TYPE_CHAR;

      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_STRING;

      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_MAYBE;
      def->overloads[num++] = sig;

      sig.arg1_type.is_generic = false;
      sig.arg1_type.val.type = PT_TYPE_CHAR;

      sig.arg2_type.is_generic = true;
      sig.arg2_type.val.generic_type = PT_GENERIC_TYPE_NUMBER;

      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_MAYBE;
      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_EXEC_STATS:
      num = 0;

      /* one overload */

      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_CHAR;
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_BIGINT;
      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_TO_ENUMERATION_VALUE:
      num = 0;

      /* one overload */

      /* arg1 */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_ANY;

      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_ENUMERATION;

      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_INET_ATON:
      num = 0;

      /* one overload */

      /* arg1 */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_CHAR;

      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_BIGINT;

      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_INET_NTOA:
      num = 0;

      /* one overload */

      /* arg1 */
      sig.arg1_type.is_generic = false;
      sig.arg1_type.val.type = PT_TYPE_BIGINT;

      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_VARCHAR;

      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_COERCIBILITY:
      num = 0;

      /* one overload */

      /* arg1 */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_ANY;

      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_INTEGER;

      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_CHARSET:
    case PT_COLLATION:
      num = 0;

      /* one overload */

      /* arg1 */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.generic_type = PT_GENERIC_TYPE_ANY;

      /* return type */
      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_VARCHAR;

      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    case PT_WIDTH_BUCKET:
      num = 0;

      /* five overloads */

      /* generic number */
      sig.arg1_type.is_generic = true;
      sig.arg1_type.val.type = PT_GENERIC_TYPE_NUMBER;

      sig.arg2_type.is_generic = false;
      sig.arg2_type.val.type = PT_TYPE_DOUBLE;	/* between */

      sig.arg3_type.is_generic = false;
      sig.arg3_type.val.type = PT_TYPE_DOUBLE;

      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_INTEGER;

      def->overloads[num++] = sig;

      /* date */
      sig.arg1_type.is_generic = false;
      sig.arg1_type.val.type = PT_TYPE_DATE;

      sig.arg2_type.is_generic = false;
      sig.arg2_type.val.type = PT_TYPE_DATE;	/* between */

      sig.arg3_type.is_generic = false;
      sig.arg3_type.val.type = PT_TYPE_DOUBLE;

      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_INTEGER;

      def->overloads[num++] = sig;

      /* datetime */
      sig.arg1_type.is_generic = false;
      sig.arg1_type.val.type = PT_TYPE_DATETIME;

      sig.arg2_type.is_generic = false;
      sig.arg2_type.val.type = PT_TYPE_DATETIME;	/* between */

      sig.arg3_type.is_generic = false;
      sig.arg3_type.val.type = PT_TYPE_DOUBLE;

      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_INTEGER;

      def->overloads[num++] = sig;

      /* timestamp */
      sig.arg1_type.is_generic = false;
      sig.arg1_type.val.type = PT_TYPE_TIMESTAMP;

      sig.arg2_type.is_generic = false;
      sig.arg2_type.val.type = PT_TYPE_TIMESTAMP;	/* between */

      sig.arg3_type.is_generic = false;
      sig.arg3_type.val.type = PT_TYPE_DOUBLE;

      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_INTEGER;

      def->overloads[num++] = sig;

      /* time */
      sig.arg1_type.is_generic = false;
      sig.arg1_type.val.type = PT_TYPE_TIME;

      sig.arg2_type.is_generic = false;
      sig.arg2_type.val.type = PT_TYPE_TIME;	/* between */

      sig.arg3_type.is_generic = false;
      sig.arg3_type.val.type = PT_TYPE_DOUBLE;

      sig.return_type.is_generic = false;
      sig.return_type.val.type = PT_TYPE_INTEGER;

      def->overloads[num++] = sig;

      def->overloads_count = num;
      break;

    default:
      return false;
    }

  assert (def->overloads_count <= MAX_OVERLOADS);

  return true;
}

/*
 * pt_get_equivalent_type () - get the type to which a node should be
 *			       converted to in order to match an expression
 *			       definition
 *   return	  : the new type
 *   def_type(in) : the type defined in the expression signature
 *   arg_type(in) : the type of the received expression argument
 */
static PT_TYPE_ENUM
pt_get_equivalent_type (const PT_ARG_TYPE def_type,
			const PT_TYPE_ENUM arg_type)
{
  if (arg_type == PT_TYPE_NULL || arg_type == PT_TYPE_NONE)
    {
      /* either the argument is null or not defined */
      return arg_type;
    }

  if (!def_type.is_generic)
    {
      /* if the definition does not have a generic type,
         return the definition type
       */
      return def_type.val.type;
    }

  /* In some cases that involve ENUM (e.g. bit_length function) we need to
   * convert ENUM to the other type even if the types are equivalent */
  if (pt_are_equivalent_types (def_type, arg_type)
      && arg_type != PT_TYPE_ENUMERATION)
    {
      /* def_type includes type */
      if (arg_type == PT_TYPE_LOGICAL)
	{
	  /* def_type is a generic type and even though
	     logical type might be equivalent with the generic definition,
	     we are sure that we don't want it to be logical here */
	  return PT_TYPE_INTEGER;
	}
      return arg_type;
    }

  /* At this point we do not have a clear match. We will return the "largest"
     type for the generic type defined in the expression signature */
  switch (def_type.val.generic_type)
    {
    case PT_GENERIC_TYPE_ANY:
      if (arg_type == PT_TYPE_LOGICAL)
	{
	  /* if PT_TYPE_LOGICAL apprears for a PT_GENERIC_TYPE_ANY, it should
	     be converted to PT_TYPE_INTEGER. */
	  return PT_TYPE_INTEGER;
	}
      return arg_type;
      break;

    case PT_GENERIC_TYPE_PRIMITIVE:
      if (PT_IS_PRIMITIVE_TYPE (arg_type))
	{
	  return arg_type;
	}
      break;

    case PT_GENERIC_TYPE_LOB:
      if (PT_IS_LOB_TYPE (arg_type))
	{
	  return arg_type;
	}
      break;

    case PT_GENERIC_TYPE_DISCRETE_NUMBER:
      return PT_TYPE_BIGINT;
      break;

    case PT_GENERIC_TYPE_NUMBER:
      if (arg_type == PT_TYPE_ENUMERATION)
	{
	  return PT_TYPE_SMALLINT;
	}
      return PT_TYPE_DOUBLE;
      break;

    case PT_GENERIC_TYPE_CHAR:
    case PT_GENERIC_TYPE_STRING:
    case PT_GENERIC_TYPE_STRING_VARYING:
      return PT_TYPE_VARCHAR;
      break;

    case PT_GENERIC_TYPE_NCHAR:
      return PT_TYPE_VARNCHAR;
      break;

    case PT_GENERIC_TYPE_BIT:
      return PT_TYPE_VARBIT;
      break;

    case PT_GENERIC_TYPE_DATE:
      return PT_TYPE_DATETIME;
      break;

    default:
      return PT_TYPE_NONE;
      break;
    }

  return PT_TYPE_NONE;
}

/*
 * pt_coerce_expression_argument () - check if arg has the correct type and
 *				      wrap arg with a cast node to def_type
 *				      if needed.
 *   return	  : NO_ERROR on success, ER_FAILED on error
 *   parser(in)	  : the parser context
 *   expr(in)	  : the expression to which arg belongs to
 *   arg (in/out) : the expression argument that will be checked
 *   def_type(in) : the type that was evaluated from the expression definition
 *   data_type(in): precision and scale information for arg
 */
static int
pt_coerce_expression_argument (PARSER_CONTEXT * parser, PT_NODE * expr,
			       PT_NODE ** arg, const PT_TYPE_ENUM def_type,
			       PT_NODE * data_type)
{
  PT_NODE *node = *arg;
  PT_NODE *new_node = NULL, *new_dt = NULL;
  PT_TYPE_ENUM new_type = PT_TYPE_NONE;
  TP_DOMAIN *d;
  int scale = DB_DEFAULT_SCALE, precision = DB_DEFAULT_PRECISION;

  if (node == NULL)
    {
      if (def_type != PT_TYPE_NONE)
	{
	  return ER_FAILED;
	}
      return NO_ERROR;
    }

  if (node->type_enum == PT_TYPE_NULL)
    {
      /* no coercion needed for NULL arguments */
      return NO_ERROR;
    }

  if (def_type == node->type_enum)
    {
      return NO_ERROR;
    }

  if (def_type == PT_TYPE_LOGICAL)
    {
      /* no cast for type logical. this is an error and we should report it */
      return ER_FAILED;
    }

  /* set default scale and precision for parametrized types */
  switch (def_type)
    {
    case PT_TYPE_BIGINT:
      precision = DB_DEFAULT_PRECISION;
      scale = DB_DEFAULT_SCALE;
      break;

    case PT_TYPE_NUMERIC:
      if (PT_IS_DISCRETE_NUMBER_TYPE (node->type_enum))
	{
	  precision = DB_DEFAULT_NUMERIC_PRECISION;
	  scale = 0;
	}
      else
	{
	  precision = DB_DEFAULT_NUMERIC_PRECISION;
	  scale = DB_DEFAULT_NUMERIC_DIVISION_SCALE;
	}
      break;

    case PT_TYPE_VARCHAR:
      precision = TP_FLOATING_PRECISION_VALUE;
      scale = 0;
      break;

    case PT_TYPE_VARNCHAR:
      precision = TP_FLOATING_PRECISION_VALUE;
      scale = 0;
      break;
    case PT_TYPE_ENUMERATION:
      {
	/* Because enumerations should always be casted to a fully specified
	 * type, we only accept casting to an enumeration type if we have
	 * a symmetrical expression (meaning that the arguments of the
	 * expression should have the same type) and one of the arguments is
	 * already an enumeration. In this case, we will cast the argument
	 * that is not an enumeration to the enumeration type of the other
	 * argument
	 */
	PT_NODE *arg1, *arg2, *arg3;
	if (expr == NULL)
	  {
	    assert (false);
	    return ER_FAILED;
	  }
	if (!pt_is_symmetric_op (expr->info.expr.op))
	  {
	    assert (false);
	    return ER_FAILED;
	  }

	arg1 = expr->info.expr.arg1;
	arg2 = expr->info.expr.arg2;
	arg3 = expr->info.expr.arg3;
	/* we already know that arg is not an enumeration so we have to look
	 * for an enumeration between the other arguments of expr
	 */
	if (arg1 != NULL && arg1->type_enum == PT_TYPE_ENUMERATION)
	  {
	    new_dt = arg1->data_type;
	  }
	else if (arg2 != NULL && arg2->type_enum == PT_TYPE_ENUMERATION)
	  {
	    new_dt = arg2->data_type;
	  }
	else if (arg3 != NULL && arg3->type_enum == PT_TYPE_ENUMERATION)
	  {
	    new_dt = arg3->data_type;
	  }
	if (new_dt == NULL)
	  {
	    assert (false);
	    PT_ERRORmf2 (parser, node, MSGCAT_SET_PARSER_SEMANTIC,
			 MSGCAT_SEMANTIC_OP_NOT_DEFINED_ON_1,
			 pt_show_binopcode (expr->info.expr.op),
			 pt_show_type_enum (PT_TYPE_ENUMERATION));
	    return ER_FAILED;
	  }
	break;
      }
    default:
      precision = DB_DEFAULT_PRECISION;
      scale = DB_DEFAULT_SCALE;
      break;
    }

  if (node->type_enum == PT_TYPE_MAYBE)
    {
      if ((node->node_type == PT_EXPR &&
	   pt_is_op_hv_late_bind (node->info.expr.op)) ||
	  (node->node_type == PT_SELECT &&
	   node->info.query.is_subquery == PT_IS_SUBQUERY))
	{
	  /* wrap with cast, instead of setting expected domain */
	  new_node =
	    pt_wrap_with_cast_op (parser, node, def_type, precision, scale,
				  new_dt);

	  if (new_node == NULL)
	    {
	      return ER_FAILED;
	    }
	  /* reset expected domain of wrapped argument to NULL: it will be
	   * replaced at XASL generation with DB_TYPE_VARIABLE domain*/
	  node->expected_domain = NULL;

	  *arg = new_node;
	}
      else
	{
	  if (new_dt != NULL)
	    {
	      d = pt_data_type_to_db_domain (parser, new_dt, NULL);
	    }
	  else
	    {
	      d = tp_domain_resolve_default (pt_type_enum_to_db (def_type));
	    }
	  if (d == NULL)
	    {
	      return ER_FAILED;
	    }
	  /* make sure the returned domain is cached */
	  d = tp_domain_cache (d);
	  SET_EXPECTED_DOMAIN (node, d);
	}

      if (node->node_type == PT_HOST_VAR)
	{
	  pt_preset_hostvar (parser, node);
	}
    }
  else
    {
      if (PT_IS_COLLECTION_TYPE (def_type))
	{
	  if (data_type == NULL)
	    {
	      if (PT_IS_COLLECTION_TYPE (node->type_enum))
		{
		  data_type = parser_copy_tree_list (parser, node->data_type);
		}
	      else
		{
		  /* this is might not be an error and we might do more damage
		     if we cast it */
		  return NO_ERROR;
		}
	    }
	  new_node =
	    pt_wrap_collection_with_cast_op (parser, node, def_type,
					     data_type);
	}
      else
	{
	  new_node =
	    pt_wrap_with_cast_op (parser, node, def_type, precision, scale,
				  new_dt);
	}
      if (new_node == NULL)
	{
	  return ER_FAILED;
	}
      if (prm_get_bool_value (PRM_ID_RETURN_NULL_ON_FUNCTION_ERRORS))
	{
	  PT_EXPR_INFO_SET_FLAG (new_node, PT_EXPR_INFO_CAST_NOFAIL);
	}

      *arg = new_node;
    }

  return NO_ERROR;
}

/*
 * pt_are_unmatchable_types () - check if the two types cannot be matched
 *   return	  : true if the two types cannot be matched
 *   def_type(in) : an expression definition type
 *   op_type(in)  : an argument type
 */
static bool
pt_are_unmatchable_types (const PT_ARG_TYPE def_type,
			  const PT_TYPE_ENUM op_type)
{
  /* PT_TYPE_NONE does not match anything */
  if (op_type == PT_TYPE_NONE &&
      !(def_type.is_generic == false && def_type.val.type == PT_TYPE_NONE))
    {
      return true;
    }

  if (op_type != PT_TYPE_NONE && def_type.is_generic == false
      && def_type.val.type == PT_TYPE_NONE)
    {
      return true;
    }

  return false;
}

/*
 * pt_are_equivalent_types () - check if a node type is equivalent with a
 *				definition type
 * return	: true if the types are equivalent, false otherwise
 * def_type(in)	: the definition type
 * op_type(in)	: argument type
 */
static bool
pt_are_equivalent_types (const PT_ARG_TYPE def_type,
			 const PT_TYPE_ENUM op_type)
{
  if (!def_type.is_generic)
    {
      if (def_type.val.type == op_type && op_type == PT_TYPE_NONE)
	{
	  /* return false if both arguments are of type none */
	  return false;
	}
      if (def_type.val.type == op_type)
	{
	  /* return true if both have the same type */
	  return true;
	}
      /* if def_type is a PT_TYPE_ENUM and the conditions above did not hold
         then the two types are not equivalent. */
      return false;
    }

  switch (def_type.val.generic_type)
    {
    case PT_GENERIC_TYPE_ANY:
      /* PT_GENERIC_TYPE_ANY is equivalent to any type */
      return true;
      break;
    case PT_GENERIC_TYPE_PRIMITIVE:
      if (PT_IS_PRIMITIVE_TYPE (op_type))
	{
	  return true;
	}
      break;
    case PT_GENERIC_TYPE_DISCRETE_NUMBER:
      if (PT_IS_DISCRETE_NUMBER_TYPE (op_type)
	  || op_type == PT_TYPE_ENUMERATION)
	{
	  /* PT_GENERIC_TYPE_DISCRETE_NUMBER is equivalent with
	     SHORT, INTEGER and BIGINT */
	  return true;
	}
      break;

    case PT_GENERIC_TYPE_NUMBER:
      if (PT_IS_NUMERIC_TYPE (op_type) || op_type == PT_TYPE_ENUMERATION)
	{
	  /* any NUMBER type is equivalent with PT_GENERIC_TYPE_NUMBER */
	  return true;
	}
      break;

    case PT_GENERIC_TYPE_STRING:
      if (PT_IS_CHAR_STRING_TYPE (op_type) || op_type == PT_TYPE_ENUMERATION)
	{
	  /* any STRING type is equivalent with PT_GENERIC_TYPE_STRING */
	  return true;
	}
      break;

    case PT_GENERIC_TYPE_CHAR:
      if (op_type == PT_TYPE_CHAR || op_type == PT_TYPE_VARCHAR
	  || op_type == PT_TYPE_ENUMERATION)
	{
	  /* CHAR and VARCHAR are equivalent to PT_GENERIC_TYPE_CHAR */
	  return true;
	}
      break;

    case PT_GENERIC_TYPE_NCHAR:
      if (op_type == PT_TYPE_NCHAR || op_type == PT_TYPE_VARNCHAR)
	{
	  /* NCHAR and VARNCHAR are equivalent to PT_GENERIC_TYPE_NCHAR */
	  return true;
	}
      break;

    case PT_GENERIC_TYPE_BIT:
      if (PT_IS_BIT_STRING_TYPE (op_type))
	{
	  /* BIT and BIT VARYING are equivalent to PT_GENERIC_TYPE_BIT */
	  return true;
	}
      break;

    case PT_GENERIC_TYPE_DATETIME:
      if (PT_IS_DATE_TIME_TYPE (op_type))
	{
	  return true;
	}
      break;
    case PT_GENERIC_TYPE_DATE:
      if (PT_HAS_DATE_PART (op_type))
	{
	  return true;
	}
      break;
    case PT_GENERIC_TYPE_SEQUENCE:
      if (PT_IS_COLLECTION_TYPE (op_type))
	{
	  /* any COLLECTION is equivalent with PT_GENERIC_TYPE_SEQUENCE */
	  return true;
	}
      break;

    default:
      return false;
      break;
    }

  return false;
}

static PT_NODE *
pt_evaluate_new_data_type (const PT_TYPE_ENUM old_type,
			   const PT_TYPE_ENUM new_type, PT_NODE * data_type)
{
  if (new_type == PT_TYPE_NUMERIC)
    {
      switch (old_type)
	{
	case PT_TYPE_SMALLINT:
	case PT_TYPE_INTEGER:
	case PT_TYPE_BIGINT:
	  return data_type;
	default:
	  /* any other type should set maximum scale and precision since
	     we cannot correctly evaluate them during type checking */
	  return NULL;
	}
    }
  return NULL;
}


/*
 * pt_infer_common_type - get the common type of the three arguments
 *
 *  return	  : the common type
 *  op(in)	  : expression identifier
 *  arg1(in/out)  : type of the first argument
 *  arg2(in/out)  : type of the second argument
 *  arg3(in/out)  : type of the third argument
 *
 * Notes: Unlike pt_common_type_op, this function infers a type for host
 *	  variables also. We can do this here because this function is called
 *	  after the expression signature has been applied. This means that
 *	  a type is left as PT_TYPE_MAYBE because the expression is symmetric
 *	  and the signature defines PT_GENERIC_TYPE_ANY or
 *	  PT_GENERIC_TYPE_PRIMITIVE for this argument.
 *	  There are some issues with collection types that this function does
 *	  not address. Collections are composed types so it's not enough to
 *	  decide that the common type is a collection type. We should also
 *	  infer the common type of the collection elements. This should be
 *	  done in the calling function because we have access to the actual
 *	  arguments there.
 */
static PT_TYPE_ENUM
pt_infer_common_type (const PT_OP_TYPE op, PT_TYPE_ENUM * arg1,
		      PT_TYPE_ENUM * arg2, PT_TYPE_ENUM * arg3,
		      const TP_DOMAIN * expected_domain)
{
  PT_TYPE_ENUM common_type = PT_TYPE_NONE;
  PT_TYPE_ENUM arg1_eq_type = *arg1;
  PT_TYPE_ENUM arg2_eq_type = *arg2;
  PT_TYPE_ENUM arg3_eq_type = *arg3;
  PT_TYPE_ENUM expected_type = PT_TYPE_NONE;
  assert (pt_is_symmetric_op (op));

  /* We ignore PT_TYPE_NONE arguments because, in the context of this
     function, if an argument is of type PT_TYPE_NONE then it is not defined
     in the signature that we have decided to use.
   */

  if (expected_domain != NULL)
    {
      expected_type = pt_db_to_type_enum (TP_DOMAIN_TYPE (expected_domain));
    }

  common_type = arg1_eq_type;
  if (arg1_eq_type != PT_TYPE_NONE)
    {
      /* arg1 is defined */
      if (arg2_eq_type != PT_TYPE_NONE)
	{
	  /* arg2 is defined */
	  common_type = pt_common_type_op (arg1_eq_type, op, arg2_eq_type);
	  if (common_type == PT_TYPE_MAYBE && op != PT_COALESCE)
	    {
	      /* "mirror" the known argument type to the other argument if
	       * the later is PT_TYPE_MAYBE*/
	      if (!pt_is_op_hv_late_bind (op))
		{
		  if (arg1_eq_type != PT_TYPE_MAYBE)
		    {
		      /* then arg2_eq_type is PT_TYPE_MAYBE */
		      arg2_eq_type = arg1_eq_type;
		      common_type = arg1_eq_type;
		    }
		  else if (arg2_eq_type != PT_TYPE_MAYBE)
		    {
		      arg1_eq_type = arg2_eq_type;
		      common_type = arg2_eq_type;
		    }
		}
	    }
	}
    }
  if (arg3_eq_type != PT_TYPE_NONE)
    {
      /* at this point either all arg1_eq_type, arg2_eq_type and common_type
         are PT_TYPE_MAYBE, or are already set to a PT_TYPE_ENUM */
      common_type = pt_common_type_op (common_type, op, arg3_eq_type);
      if (common_type == PT_TYPE_MAYBE)
	{
	  /* either arg3_eq_type is PT_TYPE_MABYE or arg1, arg2 and
	     common_type were PT_TYPE_MABYE */
	  if (arg3_eq_type == PT_TYPE_MAYBE)
	    {
	      if (arg1_eq_type != PT_TYPE_MAYBE)
		{
		  common_type = arg1_eq_type;
		  arg3_eq_type = common_type;
		}
	    }
	  else
	    {
	      arg1_eq_type = arg3_eq_type;
	      arg2_eq_type = arg3_eq_type;
	      common_type = arg3_eq_type;
	    }
	}
    }
  if (common_type == PT_TYPE_MAYBE && expected_type != PT_TYPE_NONE
      && !pt_is_op_hv_late_bind (op))
    {
      /* if expected type if not PT_TYPE_NONE then a expression higher up in
         the parser tree has set an expected domain for this node and we can
         use it to set the expected domain of the arguments */
      common_type = expected_type;
    }

  /* final check :
   * common_type should be PT_TYPE_MAYBE at this stage only for a small number
   * of operators (PLUS, MINUS,..) and only when at least one of the arguments
   * is PT_TYPE_MAYBE;
   * -when common type is PT_TYPE_MAYBE, then leave all arguments as they are:
   * either TYPE_MAYBE, either a concrete TYPE - without cast. The operator's
   * arguments will be resolved at execution in this case
   * -if common type is a concrete type, then force all other arguments to the
   * common type (this should apply for most operators)*/
  if (common_type != PT_TYPE_MAYBE)
    {
      *arg1 = (arg1_eq_type == PT_TYPE_NONE) ? PT_TYPE_NONE : common_type;
      *arg2 = (arg2_eq_type == PT_TYPE_NONE) ? PT_TYPE_NONE : common_type;
      *arg3 = (arg3_eq_type == PT_TYPE_NONE) ? PT_TYPE_NONE : common_type;
    }
  return common_type;
}

/*
 * pt_get_common_collection_type - get the common type of the elements in a
 *				   collection
 *  return	: the common type
 *  set(in)	: the collection
 *  is_multitype : specifies that there are at least two different types in
 *  collection's elements.
 *
 *  Note: The PT_TYPE_MAYBE is ignored and all numeric types
 *  are counted as one.
 */
static PT_TYPE_ENUM
pt_get_common_collection_type (const PT_NODE * set, bool * is_multitype)
{
  PT_TYPE_ENUM common_type = PT_TYPE_NONE, temp_type = PT_TYPE_NONE;
  bool is_multitype_temp = false;
  PT_NODE *data_type = NULL;
  assert (set != NULL);
  assert (PT_IS_COLLECTION_TYPE (set->type_enum));

  if (set->node_type == PT_EXPR && set->info.expr.op == PT_CAST)
    {
      /* cast nodes keep element information in the cast_type node */
      data_type = set->info.expr.cast_type;
    }
  else
    {
      data_type = set->data_type;
    }

  if (data_type)
    {
      common_type = data_type->type_enum;
      for (data_type = data_type->next; data_type;
	   data_type = data_type->next)
	{
	  temp_type = data_type->type_enum;
	  if (common_type != temp_type && temp_type != PT_TYPE_MAYBE
	      && (!PT_IS_NUMERIC_TYPE (temp_type)
		  || !PT_IS_NUMERIC_TYPE (common_type)))
	    {
	      is_multitype_temp = true;
	    }
	  common_type = pt_common_type (common_type, temp_type);
	}
    }

  if (is_multitype)
    {
      *is_multitype = is_multitype_temp;
    }

  return common_type;
}

/*
 * pt_is_collection_of_type - check if a node is of type
 *			      collection_type of element_type
 *  return	: true if the node is of type collection of element_type
 *  node(in)		: the node to be checked
 *  collection_type(in)	: the type of the collection (SET, MULTISET, etc)
 *  element_type(in)	: the type of the elements in the collection
 */
static bool
pt_is_collection_of_type (const PT_NODE * node,
			  const PT_TYPE_ENUM collection_type,
			  const PT_TYPE_ENUM element_type)
{
  PT_NODE *temp = NULL;
  assert (node != NULL);

  if (!PT_IS_COLLECTION_TYPE (node->type_enum))
    {
      /* if it's not a collection return false */
      return false;
    }

  if (node->type_enum != collection_type)
    {
      /* if it's not the same type of collection return false */
      return false;
    }

  for (temp = node->data_type; temp; temp = temp->next)
    {
      if (element_type != temp->type_enum)
	{
	  /* if collection has an element of different type than element_type
	     return false */
	  return false;
	}
    }

  return true;
}

/*
 * pt_coerce_range_expr_arguments - apply signature sig to the arguments of the
 *				 logical expression expr
 *  return	: the (possibly modified) expr or NULL on error
 *  parser(in)	: the parser context
 *  expr(in)	: the SQL expression
 *  arg1(in)	: first argument of the expression
 *  arg2(in)	: second argument of the expression
 *  arg3(in)	: third argument of the expression
 *  sig(in)	: the expression signature
 *
 */
static PT_NODE *
pt_coerce_range_expr_arguments (PARSER_CONTEXT * parser, PT_NODE * expr,
				PT_NODE * arg1, PT_NODE * arg2,
				PT_NODE * arg3, EXPRESSION_SIGNATURE sig)
{
  PT_TYPE_ENUM arg1_type = PT_TYPE_NONE, arg2_type = PT_TYPE_NONE;
  PT_TYPE_ENUM arg3_type = PT_TYPE_NONE;
  PT_TYPE_ENUM arg1_eq_type = PT_TYPE_NONE, arg2_eq_type = PT_TYPE_NONE;
  PT_TYPE_ENUM arg3_eq_type = PT_TYPE_NONE;
  PT_TYPE_ENUM common_type = PT_TYPE_NONE;
  PT_NODE *arg1_dt = NULL;
  PT_NODE *arg2_dt = NULL;
  PT_NODE *arg3_dt = NULL;
  PT_NODE *common_dt = NULL;
  PT_OP_TYPE op;
  int error = NO_ERROR;

  op = expr->info.expr.op;

  arg1 = expr->info.expr.arg1;
  if (arg1)
    {
      arg1_type = arg1->type_enum;
    }

  arg2 = expr->info.expr.arg2;
  if (arg2)
    {
      arg2_type = arg2->type_enum;
    }

  arg3 = expr->info.expr.arg3;
  if (arg3)
    {
      arg3_type = arg3->type_enum;
    }

  arg1_eq_type = pt_get_equivalent_type (sig.arg1_type, arg1_type);
  arg2_eq_type = pt_get_equivalent_type (sig.arg2_type, arg2_type);
  arg3_eq_type = pt_get_equivalent_type (sig.arg3_type, arg3_type);

  /* for range expressions the second argument may be a collection or a
     query. */
  if (PT_IS_QUERY_NODE_TYPE (arg2->node_type))
    {
      /* the select list must have only one element and the first argument
         has to be of the same type as the argument from the select list */
      PT_NODE *arg2_list = NULL;

      expr->info.expr.arg2->info.query.all_distinct = PT_DISTINCT;

      arg2_list = pt_get_select_list (parser, arg2);
      if (PT_IS_COLLECTION_TYPE (arg2_list->type_enum)
	  && arg2_list->node_type == PT_FUNCTION)
	{
	  expr->type_enum = PT_TYPE_LOGICAL;
	  return expr;
	}
      if (pt_length_of_select_list (arg2_list, EXCLUDE_HIDDEN_COLUMNS) != 1)
	{
	  PT_ERRORm (parser, arg2, MSGCAT_SET_PARSER_SEMANTIC,
		     MSGCAT_SEMANTIC_NOT_SINGLE_COL);
	  return NULL;
	}
      arg2_type = arg2_list->type_enum;
      if (pt_is_enumeration_special_comparison (arg1, op, arg2))
	{
	  /* In case of 'ENUM IN [query]' we need to convert all elements of
	     right argument to the ENUM type in order to preserve an eventual
	     index scan on left argument */
	  common_type = PT_TYPE_ENUMERATION;
	}
      else
	{
	  common_type = pt_common_type_op (arg1_eq_type, op, arg2_type);
	}
      if (PT_IS_COLLECTION_TYPE (common_type))
	{
	  /* we cannot make a decision during type checking in this case */
	  return expr;
	}
      if (PT_IS_NUMERIC_TYPE (arg1_type) && PT_IS_NUMERIC_TYPE (common_type))
	{
	  /* do not cast between numeric types */
	  arg1_eq_type = arg1_type;
	}
      else
	{
	  arg1_eq_type = common_type;
	}
      if (PT_IS_NUMERIC_TYPE (arg2_type) && PT_IS_NUMERIC_TYPE (common_type))
	{
	  arg2_eq_type = arg2_type;
	}
      else
	{
	  arg2_eq_type = common_type;
	}

      error =
	pt_coerce_expression_argument (parser, expr, &arg1, arg1_eq_type,
				       NULL);
      if (error != NO_ERROR)
	{
	  return NULL;
	}
      else
	{
	  expr->info.expr.arg1 = arg1;
	}

      if (pt_wrap_select_list_with_cast_op (parser, arg2, arg2_eq_type,
					    0, 0, NULL, false) != NO_ERROR)
	{
	  return NULL;
	}
    }
  else if (PT_IS_COLLECTION_TYPE (arg2_type))
    {
      /* Because we're using collections, semantically, all three cases below
       * are valid:
       * 1. SELECT * FROM tbl WHERE int_col in {integer, set, object, date}
       * 2. SELECT * FROM tbl WHERE int_col in {str, str, str}
       * 3. SELECT * FROM tbl WHERE int_col in {integer, integer, integer}
       * We will only coerce arg2 if there is a common type between arg1
       * and all elements from the collection arg2.
       * We do not consider the case in which we cannot discern a common type
       * to be a semantic error and we rely on the functionality of the
       * comparison operators to be applied correctly on the expression during
       * expression evaluation. This means that we consider that the user knew
       * what he was doing in the first case but wanted to write something
       * else in the second case.
       */
      PT_TYPE_ENUM collection_type = PT_TYPE_NONE;
      bool should_cast = false;
      PT_NODE *data_type = NULL;

      if (pt_is_enumeration_special_comparison (arg1, op, arg2))
	{
	  /* In case of 'ENUM IN (...)' we need to convert all elements of right
	     argument to the ENUM type in order to preserve an eventual index
	     scan on left argument */
	  common_type = arg1_eq_type = collection_type = PT_TYPE_ENUMERATION;
	}
      else
	{
	  common_type = collection_type =
	    pt_get_common_collection_type (arg2, &should_cast);
	  if (common_type != PT_TYPE_NONE)
	    {
	      common_type = pt_common_type_op (arg1_eq_type, op, common_type);
	      if (common_type != PT_TYPE_NONE)
		{
		  arg1_eq_type = common_type;
		}
	    }
	}
      if (common_type == PT_TYPE_OBJECT ||
	  PT_IS_COLLECTION_TYPE (common_type))
	{
	  /* we cannot make a cast decision here. to keep backwards
	     compatibility we will call pt_coerce_value which will only work
	     on constants */
	  PT_NODE *temp = NULL, *msg_temp = NULL;
	  msg_temp = parser->error_msgs;
	  (void) pt_coerce_value (parser, arg2, arg2, PT_TYPE_SET,
				  arg2->data_type);
	  if (pt_has_error (parser))
	    {
	      /* ignore errors */
	      parser_free_tree (parser, parser->error_msgs);
	      parser->error_msgs = msg_temp;
	    }
	  if (pt_coerce_value (parser, arg1, arg1,
			       common_type, NULL) != NO_ERROR)
	    {
	      expr->type_enum = PT_TYPE_NONE;
	    }
	  if (arg2->node_type == PT_VALUE)
	    {
	      for (temp = arg2->info.value.data_value.set; temp;
		   temp = temp->next)
		{
		  if (common_type != temp->type_enum)
		    {
		      msg_temp = parser->error_msgs;
		      parser->error_msgs = NULL;
		      (void) pt_coerce_value (parser, temp, temp,
					      common_type, NULL);
		      if (pt_has_error (parser))
			{
			  parser_free_tree (parser, parser->error_msgs);
			}
		      parser->error_msgs = msg_temp;
		    }
		}
	    }

	  return expr;
	}

      if (PT_IS_NUMERIC_TYPE (arg1_type) && PT_IS_NUMERIC_TYPE (arg1_eq_type))
	{
	  /* do not cast between numeric types */
	  arg1_eq_type = arg1_type;
	}
      else
	{
	  error =
	    pt_coerce_expression_argument (parser, expr, &arg1, arg1_eq_type,
					   NULL);
	  if (arg1 == NULL)
	    {
	      return NULL;
	    }
	  expr->info.expr.arg1 = arg1;
	}

      /* verify if we should cast arg2 to appropriate type */
      if (common_type == PT_TYPE_NONE	/* check if there is a valid common type
					   between members of arg2 and arg1. */
	  || (!should_cast	/* check if there are at least two different
				   types in arg2 */
	      && (collection_type == common_type
		  || (PT_IS_NUMERIC_TYPE (collection_type)
		      && PT_IS_NUMERIC_TYPE (common_type))
		  || (PT_IS_CHAR_STRING_TYPE (collection_type)
		      && PT_IS_CHAR_STRING_TYPE (common_type)))))
	{
	  return expr;
	}

      /* we can perform an implicit cast here */
      data_type = parser_new_node (parser, PT_DATA_TYPE);
      if (data_type == NULL)
	{
	  return NULL;
	}
      data_type->info.data_type.dec_precision = 0;
      data_type->info.data_type.precision = 0;
      data_type->type_enum = common_type;
      if (PT_IS_PARAMETERIZED_TYPE (common_type))
	{
	  PT_NODE *temp = NULL;
	  int precision = 0, scale = 0;
	  bool keep_searching = true;
	  for (temp = arg2->data_type; temp != NULL && keep_searching;
	       temp = temp->next)
	    {
	      if (temp->type_enum == PT_TYPE_NULL)
		{
		  continue;
		}

	      switch (common_type)
		{
		case PT_TYPE_CHAR:
		case PT_TYPE_NCHAR:
		case PT_TYPE_BIT:
		  {
		    /* CHAR, NCHAR and BIT are common types only if all
		       arguments are of type NCHAR or CHAR */
		    assert (temp->type_enum == PT_TYPE_CHAR ||
			    temp->type_enum == PT_TYPE_NCHAR);
		    if (precision > temp->info.data_type.precision)
		      {
			precision = temp->info.data_type.precision;
		      }
		    break;
		  }
		case PT_TYPE_VARCHAR:
		case PT_TYPE_VARNCHAR:
		  {
		    /* either all elements are already of string types
		       or we set maximum precision */
		    if (!PT_IS_CHAR_STRING_TYPE (temp->type_enum))
		      {
			precision = TP_FLOATING_PRECISION_VALUE;
			/* no need to look any further, we've already
			   found a type for which we have to set maximum
			   precision */
			keep_searching = false;
			break;
		      }
		    if (precision > temp->info.data_type.precision)
		      {
			precision = temp->info.data_type.precision;
		      }
		    break;
		  }
		case PT_TYPE_VARBIT:
		  {
		    /* either all elements are already of bit types
		       or we set maximum precision */
		    if (!PT_IS_BIT_STRING_TYPE (temp->type_enum))
		      {
			precision = TP_FLOATING_PRECISION_VALUE;
			/* no need to look any further, we've already
			   found a type for which we have to set maximum
			   precision */
			keep_searching = false;
			break;
		      }
		    if (precision > temp->info.data_type.precision)
		      {
			precision = temp->info.data_type.precision;
		      }
		    break;
		  }
		case PT_TYPE_NUMERIC:
		  {
		    /* either all elements are numeric or all are
		       discrete numbers */
		    if (temp->type_enum == PT_TYPE_NUMERIC)
		      {
			if (precision < temp->info.data_type.precision)
			  {
			    precision = temp->info.data_type.precision;
			  }
			if (scale < temp->info.data_type.dec_precision)
			  {
			    scale = temp->info.data_type.dec_precision;
			  }
		      }
		    else if (PT_IS_DISCRETE_NUMBER_TYPE (temp->type_enum))
		      {
			if (precision < TP_BIGINT_PRECISION)
			  {
			    precision = TP_BIGINT_PRECISION;
			  }
		      }
		    else
		      {
			assert (false);
		      }
		    break;
		  }

		default:
		  assert (false);
		  break;
		}
	    }
	  data_type->info.data_type.precision = precision;
	  data_type->info.data_type.dec_precision = scale;
	}

      arg2 =
	pt_wrap_collection_with_cast_op (parser, arg2, sig.arg2_type.val.type,
					 data_type);
      if (!arg2)
	{
	  return NULL;
	}

      expr->info.expr.arg2 = arg2;
    }
  else if (PT_IS_HOSTVAR (arg2))
    {
      TP_DOMAIN *d =
	tp_domain_resolve_default (pt_type_enum_to_db (PT_TYPE_SET));
      SET_EXPECTED_DOMAIN (arg2, d);
      pt_preset_hostvar (parser, arg2);

      error =
	pt_coerce_expression_argument (parser, expr, &arg1, arg1_eq_type,
				       NULL);
      if (error != NO_ERROR)
	{
	  return NULL;
	}
      expr->info.expr.arg1 = arg1;
    }
  else
    {
      /* This is a semantic error */
      return NULL;
    }
  return expr;
}

/*
 * pt_coerce_expr_arguments - apply signature sig to the arguments of the
 *			      expression expr
 *  return	: the (possibly modified) expr or NULL on error
 *  parser(in)	: the parser context
 *  expr(in)	: the SQL expression
 *  arg1(in)	: first argument of the expression
 *  arg2(in)	: second argument of the expression
 *  arg3(in)	: third argument of the expression
 *  sig(in)	: the expression signature
 */
static PT_NODE *
pt_coerce_expr_arguments (PARSER_CONTEXT * parser, PT_NODE * expr,
			  PT_NODE * arg1, PT_NODE * arg2, PT_NODE * arg3,
			  EXPRESSION_SIGNATURE sig)
{
  PT_TYPE_ENUM arg1_type = PT_TYPE_NONE, arg2_type = PT_TYPE_NONE;
  PT_TYPE_ENUM arg3_type = PT_TYPE_NONE;
  PT_TYPE_ENUM arg1_eq_type = PT_TYPE_NONE, arg2_eq_type = PT_TYPE_NONE;
  PT_TYPE_ENUM arg3_eq_type = PT_TYPE_NONE;
  PT_TYPE_ENUM common_type = PT_TYPE_NONE;
  PT_NODE *arg1_dt = NULL;
  PT_NODE *arg2_dt = NULL;
  PT_NODE *arg3_dt = NULL;
  PT_NODE *common_dt = NULL;
  PT_OP_TYPE op;
  int error = NO_ERROR;
  PT_NODE *between = NULL;
  PT_NODE *between_ge_lt = NULL;
  PT_NODE *b1 = NULL;
  PT_NODE *b2 = NULL;

  op = expr->info.expr.op;

  arg1 = expr->info.expr.arg1;
  if (arg1)
    {
      arg1_type = arg1->type_enum;
    }

  arg2 = expr->info.expr.arg2;
  if (arg2)
    {
      arg2_type = arg2->type_enum;
    }

  arg3 = expr->info.expr.arg3;
  if (arg3)
    {
      arg3_type = arg3->type_enum;
    }

  arg1_eq_type =
    pt_get_equivalent_type_with_op (sig.arg1_type, arg1_type, op);
  arg2_eq_type =
    pt_get_equivalent_type_with_op (sig.arg2_type, arg2_type, op);
  arg3_eq_type =
    pt_get_equivalent_type_with_op (sig.arg3_type, arg3_type, op);

  if (pt_is_symmetric_op (op))
    {
      if (pt_is_enumeration_special_comparison (arg1, op, arg2))
	{
	  /* In case of 'ENUM = const' we need to convert the right argument to
	     the ENUM type in order to preserve an eventual index scan on left
	     argument */
	  common_type = PT_TYPE_ENUMERATION;
	}
      else
	{
	  /* We should make sure that, for symmetric operators, all arguments
	     are of the same type. */
	  common_type =
	    pt_infer_common_type (op, &arg1_eq_type, &arg2_eq_type,
				  &arg3_eq_type, expr->expected_domain);
	}
      if (common_type == PT_TYPE_NONE)
	{
	  /* this is an error */
	  return NULL;

	}

      if (pt_is_range_or_comp (op)
	  && (!PT_IS_NUMERIC_TYPE (arg1_type)
	      || !PT_IS_NUMERIC_TYPE (arg2_type)
	      || (arg3_type != PT_TYPE_NONE
		  && !PT_IS_NUMERIC_TYPE (arg3_type))))
	{
	  /* cast value when comparing column with value */
	  if (PT_IS_NAME_NODE (arg1) && PT_IS_VALUE_NODE (arg2)
	      && (arg3_type == PT_TYPE_NONE || PT_IS_VALUE_NODE (arg3))
	      && arg1_type != PT_TYPE_ENUMERATION)
	    {
	      arg1_eq_type = arg2_eq_type = arg1_type;
	      if (arg3_type != PT_TYPE_NONE)
		{
		  arg3_eq_type = arg1_type;
		}
	      /* if column type is number (except NUMERIC) and operator
	       * is not EQ cast const node to DOUBLE to enhance correctness
	       * of results */
	      if (PT_IS_NUMERIC_TYPE (arg1_type)
		  && arg1_type != PT_TYPE_NUMERIC
		  && op != PT_EQ && op != PT_EQ_SOME && op != PT_EQ_ALL)
		{
		  if (arg2_type != arg1_type)
		    {
		      arg2_eq_type = PT_TYPE_DOUBLE;
		    }
		  if (arg3_type != PT_TYPE_NONE && arg3_type != arg1_type)
		    {
		      arg3_eq_type = PT_TYPE_DOUBLE;
		    }
		}
	    }
	  else if (PT_IS_NAME_NODE (arg2) && PT_IS_VALUE_NODE (arg1)
		   && arg3_type == PT_TYPE_NONE
		   && arg2_type != PT_TYPE_ENUMERATION)
	    {
	      arg1_eq_type = arg2_eq_type = arg2_type;
	      if (arg1_type != arg2_type
		  && PT_IS_NUMERIC_TYPE (arg2_type)
		  && arg2_type != PT_TYPE_NUMERIC
		  && op != PT_EQ && op != PT_EQ_SOME && op != PT_EQ_ALL)
		{
		  arg1_eq_type = PT_TYPE_DOUBLE;
		}
	    }
	}

      if (pt_is_comp_op (op))
	{
	  /* do not cast between numeric types or char types for comparison
	     operators */
	  if (PT_ARE_COMPARABLE (arg1_eq_type, arg1_type))
	    {
	      arg1_eq_type = arg1_type;
	    }
	  if (PT_ARE_COMPARABLE (arg2_eq_type, arg2_type))
	    {
	      arg2_eq_type = arg2_type;
	    }
	  if (PT_ARE_COMPARABLE (arg3_eq_type, arg3_type))
	    {
	      arg3_eq_type = arg3_type;
	    }
	}

      if (PT_IS_COLLECTION_TYPE (common_type))
	{
	  /* We do not perform implicit casts on collection types. The
	     following code will only handle constant arguments.
	   */
	  if ((arg1_eq_type != PT_TYPE_NONE
	       && !PT_IS_COLLECTION_TYPE (arg1_eq_type))
	      || (arg2_eq_type != PT_TYPE_NONE
		  && !PT_IS_COLLECTION_TYPE (arg2_eq_type))
	      || (arg3_eq_type != PT_TYPE_NONE
		  && !PT_IS_COLLECTION_TYPE (arg3_eq_type)))
	    {
	      return NULL;
	    }
	  else
	    {
	      if (pt_is_symmetric_type (common_type))
		{
		  if (arg1_eq_type != common_type
		      && arg1_eq_type != PT_TYPE_NONE)
		    {
		      pt_coerce_value (parser, arg1, arg1, common_type,
				       PT_NODE_DATA_TYPE (arg1));
		      arg1_type = common_type;
		    }
		  if (arg2_type != common_type
		      && arg2_eq_type != PT_TYPE_NONE)
		    {
		      pt_coerce_value (parser, arg2, arg2, common_type,
				       PT_NODE_DATA_TYPE (arg2));
		      arg2_type = common_type;
		    }
		}
	      /* we're not casting collection types in this context but we
	         should "propagate" types */
	      if (arg2_type != PT_TYPE_NONE)
		{
		  pt_propagate_types (parser, expr, PT_NODE_DATA_TYPE (arg1),
				      PT_NODE_DATA_TYPE (arg2));
		}
	      if (arg3_type != PT_TYPE_NONE)
		{
		  pt_propagate_types (parser, expr, PT_NODE_DATA_TYPE (arg2),
				      PT_NODE_DATA_TYPE (arg3));
		}
	      expr->info.expr.arg1 = arg1;
	      expr->info.expr.arg2 = arg2;
	      expr->info.expr.arg3 = arg3;
	      return expr;
	    }
	}
    }

  /* We might have decided a new type for arg1 based on the common_type
     but, if the signature defines an exact type, we should keep it.
     For example, + is a symmetric operator but also defines date + bigint
     which is not symmetric and we have to keep the bigint type even if
     the common type is date. This is why, before coercing expression
     arguments, we check the signature that we decided to apply */
  if (!sig.arg1_type.is_generic)
    {
      arg1_eq_type = sig.arg1_type.val.type;
    }
  if (PT_IS_RECURSIVE_EXPRESSION (expr)
      && expr->info.expr.recursive_type != PT_TYPE_NONE)
    {
      /* In case of recursive expression (PT_GREATEST, PT_LEAST, ...) the common
         type is stored in recursive_type */
      arg1_eq_type = expr->info.expr.recursive_type;
    }
  error = pt_coerce_expression_argument (parser, expr, &arg1, arg1_eq_type,
					 arg1_dt);
  if (error != NO_ERROR)
    {
      return NULL;
    }
  else
    {
      expr->info.expr.arg1 = arg1;
    }

  if (!sig.arg2_type.is_generic)
    {
      arg2_eq_type = sig.arg2_type.val.type;
    }
  if (PT_IS_RECURSIVE_EXPRESSION (expr)
      && expr->info.expr.recursive_type != PT_TYPE_NONE)
    {
      /* In case of recursive expression (PT_GREATEST, PT_LEAST, ...) the common
         type is stored in recursive_type */
      arg2_eq_type = expr->info.expr.recursive_type;
    }

  if (op != PT_WIDTH_BUCKET)
    {
      error =
	pt_coerce_expression_argument (parser, expr, &arg2, arg2_eq_type,
				       arg2_dt);
      if (error != NO_ERROR)
	{
	  return NULL;
	}
      else
	{
	  expr->info.expr.arg2 = arg2;
	}
    }
  else
    {
      /* width_bucket is a special case. It has 4 params
       * the 2nd and 3rd args are coerced here
       */
      between = expr->info.expr.arg2;
      if (between == NULL
	  || between->node_type != PT_EXPR
	  || between->info.expr.op != PT_BETWEEN)
	{
	  return NULL;
	}

      between_ge_lt = between->info.expr.arg2;
      if (between_ge_lt == NULL
	  || between_ge_lt->node_type != PT_EXPR
	  || between_ge_lt->info.expr.op != PT_BETWEEN_GE_LT)
	{
	  return NULL;
	}

      /* 2nd, 3rd param of width_bucket */
      b1 = between_ge_lt->info.expr.arg1;
      b2 = between_ge_lt->info.expr.arg2;
      assert (b1 != NULL && b2 != NULL);

      error = pt_coerce_expression_argument (parser, between_ge_lt, &b1,
					     arg2_eq_type, arg1_dt);
      if (error != NO_ERROR)
	{
	  return NULL;
	}
      else
	{
	  between_ge_lt->info.expr.arg1 = b1;
	}

      error = pt_coerce_expression_argument (parser, between_ge_lt, &b2,
					     arg2_eq_type, arg2_dt);
      if (error != NO_ERROR)
	{
	  return NULL;
	}
      else
	{
	  between_ge_lt->info.expr.arg2 = b2;
	}
    }

  if (!sig.arg3_type.is_generic)
    {
      arg3_eq_type = sig.arg3_type.val.type;
    }
  error = pt_coerce_expression_argument (parser, expr, &arg3, arg3_eq_type,
					 arg3_dt);
  if (error != NO_ERROR)
    {
      return NULL;
    }
  else
    {
      expr->info.expr.arg3 = arg3;
    }

  return expr;
}

/*
 * pt_is_range_expression () - return true if the expression is evaluated
 *				 as a logical expression
 *  return  : true if the expression is of type logical
 *  op (in) : the expression
 */
static bool
pt_is_range_expression (const PT_OP_TYPE op)
{
  switch (op)
    {
    case PT_GE_SOME:
    case PT_GT_SOME:
    case PT_LT_SOME:
    case PT_LE_SOME:
    case PT_GE_ALL:
    case PT_GT_ALL:
    case PT_LT_ALL:
    case PT_LE_ALL:
    case PT_EQ_SOME:
    case PT_NE_SOME:
    case PT_EQ_ALL:
    case PT_NE_ALL:
    case PT_IS_IN:
    case PT_IS_NOT_IN:
      return true;
    default:
      return false;
    }
  return false;
}

/*
 * pt_is_range_or_comp () - return true if the operator is range or comparison
 *  return:
 *  op(in):
 */
static bool
pt_is_range_or_comp (PT_OP_TYPE op)
{
  switch (op)
    {
    case PT_EQ:
    case PT_NE:
    case PT_GE:
    case PT_GT:
    case PT_LT:
    case PT_LE:
    case PT_NULLSAFE_EQ:
    case PT_GT_INF:
    case PT_LT_INF:
    case PT_BETWEEN:
    case PT_BETWEEN_GE_LE:
    case PT_BETWEEN_GE_LT:
    case PT_BETWEEN_GT_LE:
    case PT_BETWEEN_GT_LT:
    case PT_BETWEEN_INF_LE:
    case PT_BETWEEN_INF_LT:
    case PT_BETWEEN_GE_INF:
    case PT_BETWEEN_GT_INF:
      return true;
    default:
      return false;
    }
}

static bool
does_op_specially_treat_null_arg (PT_OP_TYPE op)
{
  if (pt_is_operator_logical (op))
    {
      return true;
    }

  switch (op)
    {
    case PT_NVL:
    case PT_IFNULL:
    case PT_ISNULL:
    case PT_NVL2:
    case PT_COALESCE:
    case PT_NULLIF:
    case PT_RPAD:
    case PT_TRANSLATE:
    case PT_RAND:
    case PT_DRAND:
    case PT_RANDOM:
    case PT_DRANDOM:
    case PT_CONCAT:
    case PT_CONCAT_WS:
      return true;
    default:
      return false;
    }
}

/*
 *  pt_apply_expressions_definition () - evaluate which expression signature
 *					 best matches the received arguments
 *					 and cast arguments to the types
 *					 described in the signature
 *  return	: NO_ERROR or error code
 *  parser(in)	: the parser context
 *  node(in/out): an SQL expression
 */
static int
pt_apply_expressions_definition (PARSER_CONTEXT * parser, PT_NODE ** node)
{
  int error = NO_ERROR;
  PT_OP_TYPE op;
  PT_NODE *arg1 = NULL, *arg2 = NULL, *arg3 = NULL;
  PT_TYPE_ENUM arg1_type = PT_TYPE_NONE, arg2_type = PT_TYPE_NONE;
  PT_TYPE_ENUM arg3_type = PT_TYPE_NONE;
  PT_TYPE_ENUM arg1_eq_type = PT_TYPE_NONE, arg2_eq_type = PT_TYPE_NONE;
  PT_TYPE_ENUM arg3_eq_type = PT_TYPE_NONE;
  PT_TYPE_ENUM common_type = PT_TYPE_NONE;
  PT_NODE *common_data_type = NULL;
  EXPRESSION_DEFINITION def;
  PT_NODE *expr = *node;
  int matches = 0, best_match = -1, i = 0;
  EXPRESSION_SIGNATURE sig;

  if (expr->node_type != PT_EXPR)
    {
      return NO_ERROR;
    }
  op = expr->info.expr.op;
  if (!pt_get_expression_definition (op, &def))
    {
      *node = NULL;
      return NO_ERROR;
    }

  if (PT_IS_RECURSIVE_EXPRESSION (expr)
      && expr->info.expr.recursive_type != PT_TYPE_NONE)
    {
      arg1_type = arg2_type = expr->info.expr.recursive_type;
    }
  else
    {
      arg1 = expr->info.expr.arg1;
      if (arg1)
	{
	  arg1_type = arg1->type_enum;
	}

      arg2 = expr->info.expr.arg2;
      if (arg2)
	{
	  arg2_type = arg2->type_enum;
	}
    }

  arg3 = expr->info.expr.arg3;
  if (arg3)
    {
      arg3_type = arg3->type_enum;
    }

  /* check the expression contains NULL argument.
   * If the op does not specially treat NULL args, for instance, NVL, NVL2,
   * IS [NOT] NULL and so on, just decide the retun type as NULL.
   */
  if (!does_op_specially_treat_null_arg (op)
      && ((arg1 && !arg1->is_added_by_parser && arg1_type == PT_TYPE_NULL)
	  || (arg2 && !arg2->is_added_by_parser && arg2_type == PT_TYPE_NULL)
	  || (arg3 && !arg3->is_added_by_parser
	      && arg3_type == PT_TYPE_NULL)))
    {
      expr->type_enum = PT_TYPE_NULL;
      return NO_ERROR;
    }

  matches = -1;
  best_match = 0;
  for (i = 0; i < def.overloads_count; i++)
    {
      int match_cnt = 0;
      if (pt_are_unmatchable_types (def.overloads[i].arg1_type, arg1_type))
	{
	  match_cnt = -1;
	  continue;
	}
      if (pt_are_equivalent_types (def.overloads[i].arg1_type, arg1_type))
	{
	  match_cnt++;
	}
      if (pt_are_unmatchable_types (def.overloads[i].arg2_type, arg2_type))
	{
	  match_cnt = -1;
	  continue;
	}
      if (pt_are_equivalent_types (def.overloads[i].arg2_type, arg2_type))
	{
	  match_cnt++;
	}
      if (pt_are_unmatchable_types (def.overloads[i].arg3_type, arg3_type))
	{
	  match_cnt = -1;
	  continue;
	}
      if (pt_are_equivalent_types (def.overloads[i].arg3_type, arg3_type))
	{
	  match_cnt++;
	}
      if (match_cnt == 3)
	{
	  best_match = i;
	  break;
	}
      else if (match_cnt > matches)
	{
	  matches = match_cnt;
	  best_match = i;
	}
    }

  if (best_match == -1)
    {
      /* if best_match is -1 then we have an expression definition but
         it cannot be applied on this arguments. */
      expr->node_type = PT_TYPE_NONE;
      return ER_FAILED;
    }

  sig = def.overloads[best_match];
  if (pt_is_range_expression (op))
    {
      /* Range expressions are expressions that compare an argument with
         a subquery or a collection. We handle these expressions separately */
      expr =
	pt_coerce_range_expr_arguments (parser, expr, arg1, arg2, arg3, sig);
    }
  else
    {
      expr = pt_coerce_expr_arguments (parser, expr, arg1, arg2, arg3, sig);
    }
  if (expr == NULL)
    {
      return ER_FAILED;
    }

  if (pt_is_op_hv_late_bind (op)
      && (arg1_type == PT_TYPE_MAYBE || arg2_type == PT_TYPE_MAYBE
	  || arg3_type == PT_TYPE_MAYBE))
    {
      expr->type_enum = PT_TYPE_MAYBE;
    }
  else
    {
      expr->type_enum = pt_expr_get_return_type (expr, sig);
    }

  /* re-read arguments to include the wrapped-cast */
  arg1 = expr->info.expr.arg1;
  arg2 = expr->info.expr.arg2;

  if (PT_IS_PARAMETERIZED_TYPE (expr->type_enum)
      && pt_upd_domain_info (parser, arg1, arg2, op, expr->type_enum,
			     expr) != NO_ERROR)
    {
      expr = NULL;
      return ER_FAILED;
    }
  *node = expr;
  return NO_ERROR;
}

/*
 * pt_expr_get_return_type () - get the return type of an expression based on
 *				the expression signature and the types of its
 *				arguments
 *  return  : the expression return type
 *  expr(in): an SQL expression
 *  sig(in) : the expression signature
 *
 *  Remarks: This function is called after the expression signature has been
 *	     decided and the expression arguments have been wrapped with CASTs
 *	     to the type described in the signature. At this point we can
 *	     decide the return type based on the argument types which are
 *	     proper CUBRID types (i.e.: not generic types)
 */
static PT_TYPE_ENUM
pt_expr_get_return_type (PT_NODE * expr, const EXPRESSION_SIGNATURE sig)
{
  PT_TYPE_ENUM arg1_type = PT_TYPE_NONE, arg2_type = PT_TYPE_NONE;
  PT_TYPE_ENUM arg3_type = PT_TYPE_NONE;

  if (sig.return_type.is_generic == false)
    {
      /* if the signature does not define a generic type, return the defined
         type */
      return sig.return_type.val.type;
    }

  if (expr->info.expr.arg1)
    {
      arg1_type = expr->info.expr.arg1->type_enum;
      if (arg1_type == PT_TYPE_MAYBE)
	{
	  if (expr->info.expr.arg1->expected_domain)
	    {
	      /* we were able to decide an expected domain for this argument
	         and we can use it in deciding the return type */
	      arg1_type =
		pt_db_to_type_enum (expr->info.expr.arg1->expected_domain->
				    type->id);
	    }
	}
    }

  if (expr->info.expr.arg2)
    {
      arg2_type = expr->info.expr.arg2->type_enum;
      if (arg2_type == PT_TYPE_MAYBE)
	{
	  if (expr->info.expr.arg2->expected_domain)
	    {
	      /* we were able to decide an expected domain for this argument
	         and we can use it in deciding the return type */
	      arg2_type =
		pt_db_to_type_enum (expr->info.expr.arg2->expected_domain->
				    type->id);
	    }
	}
    }

  if (expr->info.expr.arg3)
    {
      arg3_type = expr->info.expr.arg3->type_enum;
      if (arg3_type == PT_TYPE_MAYBE)
	{
	  if (expr->info.expr.arg3->expected_domain)
	    {
	      /* we were able to decide an expected domain for this argument
	         and we can use it in deciding the return type */
	      arg3_type =
		pt_db_to_type_enum (expr->info.expr.arg3->expected_domain->
				    type->id);
	    }
	}
    }

  /* the return type of the signature is a generic type */
  switch (sig.return_type.val.generic_type)
    {
    case PT_GENERIC_TYPE_STRING:
      {
	/* The return type might be CHAR, VARCHAR, NCHAR or VARNCHAR.
	 * Since not all arguments are required to be of string type, we have
	 * to infer the return type based only on the string type arguments
	 */
	PT_TYPE_ENUM common_type = PT_TYPE_NONE;
	if (PT_IS_STRING_TYPE (arg1_type))
	  {
	    common_type = arg1_type;
	    if (PT_IS_STRING_TYPE (arg2_type))
	      {
		common_type = pt_common_type (arg1_type, arg2_type);
		if (PT_IS_STRING_TYPE (arg3_type))
		  {
		    common_type = pt_common_type (common_type, arg3_type);
		  }
		return common_type;
	      }

	    if (PT_IS_STRING_TYPE (arg3_type))
	      {
		common_type = pt_common_type (common_type, arg3_type);
	      }
	    return common_type;
	  }
	/* arg1 is not string type */
	if (PT_IS_STRING_TYPE (arg2_type))
	  {
	    common_type = arg2_type;
	    if (PT_IS_STRING_TYPE (arg3_type))
	      {
		common_type = pt_common_type (common_type, arg3_type);
	      }
	    return common_type;
	  }

	/* arg1 and arg2 are not of string type */
	if (PT_IS_STRING_TYPE (arg3_type))
	  {
	    return arg3_type;
	  }

	if (common_type != PT_TYPE_NONE)
	  {
	    return common_type;
	  }
	break;
      }

    case PT_GENERIC_TYPE_STRING_VARYING:
      {
	PT_ARG_TYPE type;
	type.is_generic = true;
	type.val.generic_type = PT_GENERIC_TYPE_NCHAR;
	/* if one or the arguments is of national string type
	   the return type must be VARNCHAR, else it is VARCHAR */
	if (pt_are_equivalent_types (type, arg1_type)
	    || pt_are_equivalent_types (type, arg2_type)
	    || pt_are_equivalent_types (type, arg3_type))
	  {
	    return PT_TYPE_VARNCHAR;
	  }
	return PT_TYPE_VARCHAR;
      }

    case PT_GENERIC_TYPE_CHAR:
      if (arg1_type == PT_TYPE_VARCHAR || arg2_type == PT_TYPE_VARCHAR
	  || arg3_type == PT_TYPE_VARCHAR)
	{
	  return PT_TYPE_VARCHAR;
	}
      return PT_TYPE_CHAR;

    case PT_GENERIC_TYPE_NCHAR:
      if (arg1_type == PT_TYPE_VARNCHAR || arg2_type == PT_TYPE_VARNCHAR
	  || arg3_type == PT_TYPE_VARNCHAR)
	{
	  return PT_TYPE_VARNCHAR;
	}
      return PT_TYPE_NCHAR;

    case PT_GENERIC_TYPE_NUMBER:
    case PT_GENERIC_TYPE_DISCRETE_NUMBER:
    case PT_GENERIC_TYPE_ANY:
    case PT_GENERIC_TYPE_DATE:
    case PT_GENERIC_TYPE_DATETIME:
    case PT_GENERIC_TYPE_SEQUENCE:
    case PT_GENERIC_TYPE_BIT:
      {
	PT_TYPE_ENUM common_type = PT_TYPE_NONE;
	if (arg2_type == PT_TYPE_NONE
	    || (!pt_is_symmetric_op (expr->info.expr.op)
		&& !pt_is_op_with_forced_common_type (expr->info.expr.op)))
	  {
	    return arg1_type;
	  }
	common_type = pt_common_type (arg1_type, arg2_type);
	if (arg3_type != PT_TYPE_NONE)
	  {
	    common_type = pt_common_type (common_type, arg3_type);
	  }

	if (common_type != PT_TYPE_NONE)
	  {
	    return common_type;
	  }
	break;
      }
    default:
      break;
    }

  /* we might reach this point on expressions with a single argument
     of value null */
  if (arg1_type == PT_TYPE_NULL)
    {
      return PT_TYPE_NULL;
    }
  return PT_TYPE_NONE;
}

/*
 * pt_is_symmetric_type () -
 *   return:
 *   type_enum(in):
 */
static bool
pt_is_symmetric_type (const PT_TYPE_ENUM type_enum)
{
  switch (type_enum)
    {
    case PT_TYPE_INTEGER:
    case PT_TYPE_BIGINT:
    case PT_TYPE_FLOAT:
    case PT_TYPE_DOUBLE:
    case PT_TYPE_NUMERIC:
    case PT_TYPE_SMALLINT:
    case PT_TYPE_MONETARY:

    case PT_TYPE_LOGICAL:

    case PT_TYPE_SET:
    case PT_TYPE_MULTISET:
    case PT_TYPE_SEQUENCE:
    case PT_TYPE_OBJECT:

    case PT_TYPE_VARCHAR:
    case PT_TYPE_CHAR:
    case PT_TYPE_VARNCHAR:
    case PT_TYPE_NCHAR:
    case PT_TYPE_VARBIT:
    case PT_TYPE_BIT:
      return true;

    default:
      return false;
    }
}

/*
 * pt_is_symmetric_op () -
 *   return:
 *   op(in):
 */
bool
pt_is_symmetric_op (const PT_OP_TYPE op)
{
  switch (op)
    {
    case PT_FUNCTION_HOLDER:
    case PT_ASSIGN:
    case PT_GE_SOME:
    case PT_GT_SOME:
    case PT_LT_SOME:
    case PT_LE_SOME:
    case PT_GE_ALL:
    case PT_GT_ALL:
    case PT_LT_ALL:
    case PT_LE_ALL:
    case PT_EQ_SOME:
    case PT_NE_SOME:
    case PT_EQ_ALL:
    case PT_NE_ALL:
    case PT_IS_IN:
    case PT_IS_NOT_IN:
    case PT_IS_NULL:
    case PT_IS_NOT_NULL:
    case PT_POSITION:
    case PT_FINDINSET:
    case PT_SUBSTRING:
    case PT_SUBSTRING_INDEX:
    case PT_OCTET_LENGTH:
    case PT_BIT_LENGTH:
    case PT_CHAR_LENGTH:
    case PT_BIN:
    case PT_TRIM:
    case PT_LTRIM:
    case PT_RTRIM:
    case PT_LIKE_LOWER_BOUND:
    case PT_LIKE_UPPER_BOUND:
    case PT_LPAD:
    case PT_RPAD:
    case PT_REPEAT:
    case PT_REPLACE:
    case PT_TRANSLATE:
    case PT_ADD_MONTHS:
    case PT_LAST_DAY:
    case PT_MONTHS_BETWEEN:
    case PT_SYS_DATE:
    case PT_SYS_TIME:
    case PT_SYS_TIMESTAMP:
    case PT_SYS_DATETIME:
    case PT_UTC_TIME:
    case PT_UTC_DATE:
    case PT_TO_CHAR:
    case PT_TO_DATE:
    case PT_TO_TIME:
    case PT_TO_TIMESTAMP:
    case PT_TO_DATETIME:
    case PT_TO_NUMBER:
    case PT_CURRENT_VALUE:
    case PT_NEXT_VALUE:
    case PT_CAST:
    case PT_EXTRACT:
    case PT_INST_NUM:
    case PT_ROWNUM:
    case PT_ORDERBY_NUM:
    case PT_CONNECT_BY_ISCYCLE:
    case PT_CONNECT_BY_ISLEAF:
    case PT_LEVEL:
    case PT_CONNECT_BY_ROOT:
    case PT_SYS_CONNECT_BY_PATH:
    case PT_QPRIOR:
    case PT_CURRENT_USER:
    case PT_LOCAL_TRANSACTION_ID:
    case PT_CHR:
    case PT_ROUND:
    case PT_TRUNC:
    case PT_INSTR:
    case PT_TIME_FORMAT:
    case PT_TIMESTAMP:
    case PT_TIMEF:
    case PT_YEARF:
    case PT_MONTHF:
    case PT_DAYF:
    case PT_DAYOFMONTH:
    case PT_HOURF:
    case PT_MINUTEF:
    case PT_SECONDF:
    case PT_QUARTERF:
    case PT_WEEKDAY:
    case PT_DAYOFWEEK:
    case PT_DAYOFYEAR:
    case PT_TODAYS:
    case PT_FROMDAYS:
    case PT_TIMETOSEC:
    case PT_SECTOTIME:
    case PT_WEEKF:
    case PT_MAKETIME:
    case PT_MAKEDATE:
    case PT_ADDTIME:
    case PT_SCHEMA:
    case PT_DATABASE:
    case PT_VERSION:
    case PT_UNIX_TIMESTAMP:
    case PT_FROM_UNIXTIME:
    case PT_IS:
    case PT_IS_NOT:
    case PT_CONCAT:
    case PT_CONCAT_WS:
    case PT_FIELD:
    case PT_LEFT:
    case PT_RIGHT:
    case PT_LOCATE:
    case PT_MID:
    case PT_REVERSE:
    case PT_ADDDATE:
    case PT_DATE_ADD:
    case PT_SUBDATE:
    case PT_DATE_SUB:
    case PT_FORMAT:
    case PT_ATAN2:
    case PT_DATE_FORMAT:
    case PT_USER:
    case PT_STR_TO_DATE:
    case PT_LIST_DBS:
    case PT_IF:
    case PT_POWER:
    case PT_BIT_TO_BLOB:
    case PT_BLOB_FROM_FILE:
    case PT_BLOB_LENGTH:
    case PT_BLOB_TO_BIT:
    case PT_CHAR_TO_BLOB:
    case PT_CHAR_TO_CLOB:
    case PT_CLOB_FROM_FILE:
    case PT_CLOB_LENGTH:
    case PT_CLOB_TO_CHAR:
    case PT_TYPEOF:
    case PT_INDEX_CARDINALITY:
    case PT_INCR:
    case PT_DECR:
    case PT_RAND:
    case PT_RANDOM:
    case PT_DRAND:
    case PT_DRANDOM:
    case PT_PI:
    case PT_ROW_COUNT:
    case PT_LAST_INSERT_ID:
    case PT_ABS:
    case PT_BETWEEN_EQ_NA:
    case PT_BETWEEN_GE_INF:
    case PT_BETWEEN_GT_INF:
    case PT_BETWEEN_INF_LE:
    case PT_BETWEEN_INF_LT:
    case PT_LT_INF:
    case PT_GT_INF:
    case PT_CASE:
    case PT_DECODE:
    case PT_LIKE_ESCAPE:
    case PT_RLIKE:
    case PT_NOT_RLIKE:
    case PT_RLIKE_BINARY:
    case PT_NOT_RLIKE_BINARY:
    case PT_EVALUATE_VARIABLE:
    case PT_DEFINE_VARIABLE:
    case PT_EXEC_STATS:
    case PT_CONV:
    case PT_IFNULL:
    case PT_NVL:
    case PT_NVL2:
    case PT_COALESCE:
    case PT_TO_ENUMERATION_VALUE:
    case PT_CHARSET:
    case PT_COERCIBILITY:
    case PT_COLLATION:
    case PT_WIDTH_BUCKET:
      return false;

    default:
      return true;
    }
}

/*
 * pt_propagate_types () - propagate datatypes upward to this expr node
 *   return: expr's new data_type if all OK, NULL otherwise
 *   parser(in): the parser context
 *   expr(in/out): expr node needing the data_type decoration
 *   otype1(in): data_type of expr's 1st operand
 *   otype2(in): data_type of expr's 2nd operand
 */

static PT_NODE *
pt_propagate_types (PARSER_CONTEXT * parser, PT_NODE * expr,
		    PT_NODE * otype1, PT_NODE * otype2)
{
  PT_NODE *o1, *o2;

  assert (parser != NULL);

  if (!expr || !otype1 || !otype2)
    {
      return NULL;
    }

  o1 = parser_copy_tree_list (parser, otype1);
  o2 = parser_copy_tree_list (parser, otype2);

  /* append one to the other */
  if (expr->data_type)
    {
      parser_free_tree (parser, expr->data_type);
    }

  expr->data_type = parser_append_node (o2, o1);

  return expr->data_type;
}

/*
 * pt_union_sets () - compute result = set1 + set2
 *   return: 1 if all OK, 0 otherwise
 *   parser(in): the parser context
 *   domain(in): tp_domain of result
 *   set1(in): a set/multiset db_value
 *   set2(in): a set/multiset db_value
 *   result(out): an empty db_value container
 *   o2(in): a PT_NODE containing the source line & column number of set2
 *           (used purely for generating error messages)
 */

static int
pt_union_sets (PARSER_CONTEXT * parser, TP_DOMAIN * domain,
	       DB_VALUE * set1, DB_VALUE * set2,
	       DB_VALUE * result, PT_NODE * o2)
{
  DB_SET *set, *s1, *s2;
  int error;

  assert (parser != NULL && set1 != NULL && set2 != NULL && result != NULL);

  s1 = db_get_set (set1);
  s2 = db_get_set (set2);
  error = set_union (s1, s2, &set, domain);
  if (error < 0)
    {
      PT_ERRORc (parser, o2, db_error_string (3));
    }

  set_make_collection (result, set);

  return (!pt_has_error (parser));
}

/*
 * pt_difference_sets () - compute result = set1 - set2
 *   return: 1 if all OK, 0 otherwise
 *   parser(in): the parser context
 *   domain(in): tp_domain of result
 *   set1(in): a set/multiset db_value
 *   set2(in): a set/multiset db_value
 *   result(out): an empty db_value container
 *   o2(in): a PT_NODE containing the source line & column number of set1
 *           (used purely for generating error messages)
 */

static int
pt_difference_sets (PARSER_CONTEXT * parser, TP_DOMAIN * domain,
		    DB_VALUE * set1, DB_VALUE * set2,
		    DB_VALUE * result, PT_NODE * o2)
{
  DB_SET *set, *s1, *s2;
  int error;

  assert (parser != NULL && set1 != NULL && set2 != NULL && result != NULL);

  s1 = db_get_set (set1);
  s2 = db_get_set (set2);
  error = set_difference (s1, s2, &set, domain);
  if (error < 0)
    {
      PT_ERRORc (parser, o2, db_error_string (3));
    }

  set_make_collection (result, set);

  return (!pt_has_error (parser));
}

/*
 * pt_product_sets () - compute result = set1 * set2
 *   return: 1 if all OK, 0 otherwise
 *   parser(in): the parser context
 *   domain(in): tp_domain of result
 *   set1(in): a set/multiset db_value
 *   set2(in): a set/multiset db_value
 *   result(out): an empty db_value container
 *   o2(in): a PT_NODE containing the source line & column number of set1
 *           (used purely for generating error messages)
 */
static int
pt_product_sets (PARSER_CONTEXT * parser, TP_DOMAIN * domain,
		 DB_VALUE * set1, DB_VALUE * set2,
		 DB_VALUE * result, PT_NODE * o2)
{
  DB_SET *set, *s1, *s2;
  int error;

  assert (parser != NULL && set1 != NULL && set2 != NULL && result != NULL);

  s1 = db_get_set (set1);
  s2 = db_get_set (set2);
  error = set_intersection (s1, s2, &set, domain);
  if (error < 0)
    {
      PT_ERRORc (parser, o2, db_error_string (3));
    }

  set_make_collection (result, set);

  return (!pt_has_error (parser));
}


/*
 * pt_where_type () - Test for constant folded where clause,
 * 		      and fold as necessary
 *   return:
 *   parser(in):
 *   where(in/out):
 *
 * Note :
 * Unfortunately, NULL is allowed in this test to provide
 * for constant folded clauses.
 *
 */

PT_NODE *
pt_where_type (PARSER_CONTEXT * parser, PT_NODE * where)
{
  PT_NODE *cnf_node, *dnf_node, *cnf_prev, *dnf_prev;
  bool cut_off;
  int line = 0, column = 0;
  short location;

  if (where)
    {
      line = where->line_number;
      column = where->column_number;
    }

  /* traverse CNF list and keep track the pointer to previous node */
  cnf_prev = NULL;
  while ((cnf_node = ((cnf_prev) ? cnf_prev->next : where)))
    {
      /* save location */
      location = 0;
      switch (cnf_node->node_type)
	{
	case PT_EXPR:
	  location = cnf_node->info.expr.location;
	  break;

	case PT_VALUE:
	  location = cnf_node->info.value.location;
	  break;

	case PT_HOST_VAR:
	  /* TRUE/FALSE can be bound */
	  break;

	default:
	  /* stupid where cond. treat it as false condition
	   * example: SELECT * FROM foo WHERE id;
	   */
	  goto always_false;
	}

      if (cnf_node->type_enum == PT_TYPE_NA
	  || cnf_node->type_enum == PT_TYPE_NULL)
	{
	  /* on_cond does not confused with where
	     conjunct is a NULL, treat it as false condition */
	  goto always_false;
	}

      if (cnf_node->type_enum != PT_TYPE_MAYBE
	  && cnf_node->type_enum != PT_TYPE_LOGICAL
	  && !(cnf_node->type_enum == PT_TYPE_NA
	       || cnf_node->type_enum == PT_TYPE_NULL))
	{
	  /* If the conjunct is not a NULL or a logical type, then there's
	     a problem. But don't say anything if somebody else has already
	     complained */
	  if (!pt_has_error (parser))
	    {
	      PT_ERRORm (parser, where, MSGCAT_SET_PARSER_SEMANTIC,
			 MSGCAT_SEMANTIC_WANT_LOGICAL_WHERE);
	    }
	  break;
	}

      cut_off = false;

      if (cnf_node->or_next == NULL)
	{
	  if (cnf_node->node_type == PT_VALUE
	      && cnf_node->type_enum == PT_TYPE_LOGICAL
	      && cnf_node->info.value.data_value.i == 1)
	    {
	      cut_off = true;
	    }
	  else
	    {
	      /* do not fold node which already have been folded */
	      if (cnf_node->node_type == PT_VALUE
		  && cnf_node->type_enum == PT_TYPE_LOGICAL
		  && cnf_node->info.value.data_value.i == 0)
		{
		  if (cnf_node == where && cnf_node->next == NULL)
		    {
		      return where;
		    }
		  goto always_false;
		}
	    }
	}
      else
	{
	  /* traverse DNF list and
	   * keep track of the pointer to previous node */
	  dnf_prev = NULL;
	  while ((dnf_node = ((dnf_prev) ? dnf_prev->or_next : cnf_node)))
	    {
	      if (dnf_node->node_type == PT_VALUE
		  && dnf_node->type_enum == PT_TYPE_LOGICAL
		  && dnf_node->info.value.data_value.i == 1)
		{
		  cut_off = true;
		  break;
		}

	      if (dnf_node->node_type == PT_VALUE
		  && dnf_node->type_enum == PT_TYPE_LOGICAL
		  && dnf_node->info.value.data_value.i == 0)
		{
		  /* cut it off from DNF list */
		  if (dnf_prev)
		    {
		      dnf_prev->or_next = dnf_node->or_next;
		    }
		  else
		    {
		      if (cnf_prev)
			{
			  if (dnf_node->or_next)
			    {
			      cnf_prev->next = dnf_node->or_next;
			      dnf_node->or_next->next = dnf_node->next;
			    }
			  else
			    {
			      goto always_false;
			    }

			  cnf_node = cnf_prev->next;
			}
		      else
			{
			  if (dnf_node->or_next)
			    {
			      where = dnf_node->or_next;
			      dnf_node->or_next->next = dnf_node->next;
			    }
			  else
			    {
			      goto always_false;
			    }

			  cnf_node = where;
			}	/* else (cnf_prev) */
		    }		/* else (dnf_prev) */
		  dnf_node->next = NULL;
		  dnf_node->or_next = NULL;
		  parser_free_tree (parser, dnf_node);
		  if (where == NULL)
		    {
		      goto always_false;
		    }
		  continue;
		}

	      dnf_prev = (dnf_prev) ? dnf_prev->or_next : dnf_node;
	    }			/* while (dnf_node) */
	}			/* else (cnf_node->or_next == NULL) */

      if (cut_off)
	{
	  /* cut if off from CNF list */
	  if (cnf_prev)
	    {
	      cnf_prev->next = cnf_node->next;
	    }
	  else
	    {
	      where = cnf_node->next;
	    }
	  cnf_node->next = NULL;
	  parser_free_tree (parser, cnf_node);
	}
      else
	{
	  cnf_prev = (cnf_prev) ? cnf_prev->next : cnf_node;
	}
    }				/* while (cnf_node) */

  return where;

always_false:

  /* If any conjunct is false, the entire WHERE clause is false. Jack the
     return value to be a single false node (being sure to unlink the node
     from the "next" chain if we reuse the incoming node). */
  parser_free_tree (parser, where);
  where = parser_new_node (parser, PT_VALUE);
  if (where == NULL)
    {
      PT_INTERNAL_ERROR (parser, "allocate new node");
      return NULL;
    }

  where->line_number = line;
  where->column_number = column;
  where->type_enum = PT_TYPE_LOGICAL;
  where->info.value.data_value.i = 0;
  where->info.value.location = location;
  (void) pt_value_to_db (parser, where);

  return where;
}

/*
 * pt_where_type_keep_true () - The same as pt_where_type but if the expression
 *   is true it is folded to a true value rather than a NULL.
 */
PT_NODE *
pt_where_type_keep_true (PARSER_CONTEXT * parser, PT_NODE * where)
{
  PT_NODE *save_where = where;

  where = pt_where_type (parser, where);
  if (where == NULL && save_where != NULL)
    {
      /* TODO: The line/column number is lost. */
      where = parser_new_node (parser, PT_VALUE);
      if (where == NULL)
	{
	  PT_INTERNAL_ERROR (parser, "allocate new node");
	  return NULL;
	}
      where->type_enum = PT_TYPE_LOGICAL;
      where->info.value.data_value.i = 1;
      (void) pt_value_to_db (parser, where);
    }
  return where;
}

/*
 * pt_false_where () - Test for constant folded where in select
 * 	that evaluated false. also check that it is not an aggregate select
 *   return:
 *   parser(in):
 *   node(in):
 */

bool
pt_false_where (PARSER_CONTEXT * parser, PT_NODE * node)
{
  PT_NODE *from, *where;

  where = NULL;

  switch (node->node_type)
    {
    case PT_VALUE:
      where = node;
      break;

    case PT_SELECT:

      /* If the "connect by" condition is false the query still has to return
       * all the "start with" tuples. Therefore we do not check that
       * "connect by" is false.
       */
      if (node->info.query.q.select.start_with)
	{
	  where = node->info.query.q.select.start_with;
	  if (pt_false_search_condition (parser, where) == true)
	    {
	      return true;
	    }
	}
      if (node->info.query.q.select.after_cb_filter)
	{
	  where = node->info.query.q.select.after_cb_filter;
	  if (pt_false_search_condition (parser, where) == true)
	    {
	      return true;
	    }
	}

      if (node->info.query.orderby_for)
	{
	  where = node->info.query.orderby_for;
	  if (pt_false_search_condition (parser, where) == true)
	    {
	      return true;
	    }
	}

      if (pt_is_single_tuple (parser, node))
	{
	  return false;
	}

      if (node->info.query.q.select.group_by)
	{
	  where = node->info.query.q.select.having;
	  if (pt_false_search_condition (parser, where) == true)
	    {
	      return true;
	    }
	}

      for (from = node->info.query.q.select.from; from; from = from->next)
	{
	  /* exclude outer join spec from folding */
	  if (from->info.spec.join_type == PT_JOIN_LEFT_OUTER
	      || from->info.spec.join_type == PT_JOIN_RIGHT_OUTER
	      || (from->next
		  && ((from->next->info.spec.join_type == PT_JOIN_LEFT_OUTER)
		      || (from->next->info.spec.join_type ==
			  PT_JOIN_RIGHT_OUTER))))
	    {
	      continue;
	    }
	  else if (from->info.spec.derived_table_type == PT_IS_SUBQUERY
		   || from->info.spec.derived_table_type == PT_IS_SET_EXPR)
	    {
	      PT_NODE *derived_table;

	      derived_table = from->info.spec.derived_table;
	      if (derived_table && derived_table->node_type == PT_VALUE)
		{
		  if (derived_table->type_enum == PT_TYPE_NULL
		      || (derived_table->type_enum == PT_TYPE_SET
			  && (derived_table->info.value.data_value.set ==
			      NULL)))
		    {
		      /* derived table is '(null)' or 'table(set{})' */
		      return true;
		    }
		}
	    }
	}

      where = node->info.query.q.select.where;
      break;

    case PT_UPDATE:
      where = node->info.update.search_cond;
      break;

    case PT_DELETE:
      where = node->info.delete_.search_cond;
      break;

    case PT_MERGE:
      where = node->info.merge.search_cond;
      break;

    default:
      break;
    }

  return pt_false_search_condition (parser, where);
}


/*
 * pt_false_search_condition () - Test for constant-folded search condition
 * 				  that evaluated false
 *   return: 1 if any of the conjuncts are effectively false, 0 otherwise
 *   parser(in):
 *   node(in):
 */

bool
pt_false_search_condition (PARSER_CONTEXT * parser, const PT_NODE * node)
{
  while (node)
    {
      if (node->or_next == NULL
	  && (node->type_enum == PT_TYPE_NA
	      || node->type_enum == PT_TYPE_NULL
	      || (node->node_type == PT_VALUE
		  && node->type_enum == PT_TYPE_LOGICAL
		  && node->info.value.data_value.i == 0)))
	{
	  return true;
	}

      node = node->next;
    }

  return false;
}

/*
 * pt_to_false_subquery () -
 *   return:
 *   parser(in):
 *   node(in/out):
 */
static PT_NODE *
pt_to_false_subquery (PARSER_CONTEXT * parser, PT_NODE * node)
{
  PT_NODE *next;
  int line, column;
  const char *alias_print;
  PT_SELECT_INFO *subq;
  int col_cnt, i;
  PT_NODE *col, *set, *spec;

  if (node->info.query.has_outer_spec == 1 || node->info.query.is_sort_spec
      || node->info.query.is_insert_select)
    {
      /* rewrite as empty subquery
       * for example,
       *     SELECT a, b FROM x LEFT OUTER JOIN y WHERE 0 <> 0
       *  => SELECT null, null FROM table({}) as av6749(av_1) WHERE 0 <> 0
       */

      if (node->node_type != PT_SELECT)
	{
	  return NULL;
	}

      subq = &(node->info.query.q.select);

      /* rewrite SELECT list */
      col_cnt = pt_length_of_select_list (subq->list, EXCLUDE_HIDDEN_COLUMNS);

      parser_free_tree (parser, subq->list);
      subq->list = NULL;

      for (i = 0; i < col_cnt; i++)
	{
	  col = parser_new_node (parser, PT_VALUE);
	  if (col)
	    {
	      col->type_enum = PT_TYPE_NULL;
	      subq->list = parser_append_node (subq->list, col);
	    }
	  else
	    {
	      PT_ERRORm (parser, node, MSGCAT_SET_PARSER_SEMANTIC,
			 MSGCAT_SEMANTIC_OUT_OF_MEMORY);
	      return NULL;
	    }
	}

      /* rewrite FROM list */
      set = parser_new_node (parser, PT_VALUE);
      spec = parser_new_node (parser, PT_SPEC);
      if (set && spec)
	{
	  parser_free_tree (parser, subq->from);
	  subq->from = NULL;

	  set->type_enum = PT_TYPE_SEQUENCE;

	  spec->info.spec.id = (UINTPTR) spec;
	  spec->info.spec.derived_table = set;
	  spec->info.spec.derived_table_type = PT_IS_SET_EXPR;

	  /* set line number to dummy class, dummy attr */
	  spec->info.spec.range_var = pt_name (parser, "av6749");
	  spec->info.spec.as_attr_list = pt_name (parser, "av_1");
	  subq->from = spec;
	}
      else
	{
	  PT_ERRORm (parser, node, MSGCAT_SET_PARSER_SEMANTIC,
		     MSGCAT_SEMANTIC_OUT_OF_MEMORY);
	  return NULL;
	}

      /* clear unnecessary node info */
      if (node->info.query.order_by && !node->info.query.q.select.connect_by)
	{
	  parser_free_tree (parser, node->info.query.order_by);
	  node->info.query.order_by = NULL;
	}

      if (node->info.query.orderby_for)
	{
	  parser_free_tree (parser, node->info.query.orderby_for);
	  node->info.query.orderby_for = NULL;
	}

      node->info.query.correlation_level = 0;
      node->info.query.all_distinct = PT_ALL;

      /* clear unnecessary subq info */
      if (subq->group_by)
	{
	  parser_free_tree (parser, subq->group_by);
	  subq->group_by = NULL;
	}

      if (subq->connect_by)
	{
	  parser_free_tree (parser, subq->connect_by);
	  subq->connect_by = NULL;
	}

      if (subq->start_with)
	{
	  parser_free_tree (parser, subq->start_with);
	  subq->start_with = NULL;
	}

      if (subq->after_cb_filter)
	{
	  parser_free_tree (parser, subq->after_cb_filter);
	  subq->after_cb_filter = NULL;
	}

      if (subq->having)
	{
	  parser_free_tree (parser, subq->having);
	  subq->having = NULL;
	}

      if (subq->using_index)
	{
	  parser_free_tree (parser, subq->using_index);
	  subq->using_index = NULL;
	}

      subq->hint = PT_HINT_NONE;
    }
  else
    {
      int hidden = node->is_hidden_column;

      /* rewrite as null value */
      next = node->next;
      line = node->line_number;
      column = node->column_number;
      alias_print = node->alias_print;

      node->next = NULL;
      parser_free_tree (parser, node);

      node = parser_new_node (parser, PT_VALUE);
      if (!node)
	{
	  PT_ERRORm (parser, node, MSGCAT_SET_PARSER_SEMANTIC,
		     MSGCAT_SEMANTIC_OUT_OF_MEMORY);
	  return NULL;
	}

      node->line_number = line;
      node->column_number = column;
      node->alias_print = alias_print;
      node->type_enum = PT_TYPE_NULL;
      node->info.value.location = 0;
      node->is_hidden_column = hidden;
      node->next = next;	/* restore link */
    }

  return node;
}


/*
 * pt_fold_union () - Called when at least one side is a compile time
 * 	null query, this removes the empty query from the tree
 *   return:
 *   parser(in):
 *   node(in/out):
 *   arg1_is_false(in):
 */
static PT_NODE *
pt_fold_union (PARSER_CONTEXT * parser, PT_NODE * node, bool arg1_is_false)
{
  PT_MISC_TYPE distinct, subq;
  char oids_incl, comp_lock;
  PT_NODE *order_by, *orderby_for;
  PT_NODE *into_list, *for_update;
  PT_NODE_TYPE type;
  int line, column;
  const char *alias_print;
  PT_NODE *next;
  PT_NODE *arg1, *arg2;

  distinct = node->info.query.all_distinct;
  subq = node->info.query.is_subquery;
  oids_incl = node->info.query.oids_included;
  comp_lock = node->info.query.composite_locking;
  order_by = node->info.query.order_by;
  orderby_for = node->info.query.orderby_for;
  into_list = node->info.query.into_list;
  for_update = node->info.query.for_update;
  type = node->node_type;
  line = node->line_number;
  column = node->column_number;
  alias_print = node->alias_print;

  arg1 = node->info.query.q.union_.arg1;
  arg2 = node->info.query.q.union_.arg2;

  next = node->next;

  /* cut-off link, free node */
  node->next = NULL;
  node->info.query.q.union_.arg1 = NULL;
  node->info.query.q.union_.arg2 = NULL;
  node->info.query.order_by = NULL;
  node->info.query.orderby_for = NULL;
  node->info.query.into_list = NULL;
  node->info.query.for_update = NULL;
  parser_free_tree (parser, node);

  if (arg1_is_false)
    {
      if (type == PT_UNION)
	{
	  node = arg2;
	}
      else
	{
	  node = arg1;
	}
    }
  else
    {				/* arg2 must be a null query */
      if (type == PT_INTERSECTION)
	{
	  node = arg2;
	}
      else
	{
	  node = arg1;
	}
    }

  if (node == arg1)
    {
      parser_free_tree (parser, arg2);
    }
  else
    {
      parser_free_tree (parser, arg1);
    }

  node->line_number = line;
  node->column_number = column;
  node->alias_print = alias_print;

  if (PT_IS_QUERY_NODE_TYPE (node->node_type))
    {
      /* These things need to be kept from the outer union node.
       * i.e. if there is an order_by on the union statement, it takes
       * precedence over any order_by on the argument node
       */

      if (distinct == PT_DISTINCT)
	{
	  node->info.query.all_distinct = PT_DISTINCT;
	}
      node->info.query.is_subquery = subq;
      node->info.query.oids_included = oids_incl;
      node->info.query.composite_locking = comp_lock;
      if (order_by)
	{
	  node->info.query.order_by = order_by;
	}

      if (orderby_for)
	{
	  node->info.query.orderby_for = orderby_for;
	}

      if (into_list)
	{
	  node->info.query.into_list = into_list;
	}

      if (for_update)
	{
	  node->info.query.for_update = for_update;
	}
    }

  node->next = next;

  return node;
}

/*
 * pt_eval_recursive_expr_type () - evaluates type for recursive expression
 *	nodes.
 *   return: evaluated node
 *   parser(in):
 *   recursive_expr(in): recursive expression node to evaluate.
 */
static PT_NODE *
pt_eval_recursive_expr_type (PARSER_CONTEXT * parser,
			     PT_NODE * recursive_expr)
{
  PT_OP_TYPE op;

  if (recursive_expr == NULL)
    {
      return NULL;
    }

  if (PT_IS_RECURSIVE_EXPRESSION (recursive_expr))
    {
      op = recursive_expr->info.expr.op;
      if (PT_IS_LEFT_RECURSIVE_EXPRESSION (recursive_expr))
	{
	  if (recursive_expr->info.expr.arg1 != NULL
	      && PT_IS_RECURSIVE_EXPRESSION (recursive_expr->info.expr.arg1)
	      && op == recursive_expr->info.expr.arg1->info.expr.op)
	    {
	      recursive_expr->info.expr.arg1 =
		pt_eval_recursive_expr_type (parser,
					     recursive_expr->info.expr.arg1);
	    }
	}
      else
	{
	  if (recursive_expr->info.expr.arg2 != NULL
	      && PT_IS_RECURSIVE_EXPRESSION (recursive_expr->info.expr.arg2)
	      && op == recursive_expr->info.expr.arg2->info.expr.op)
	    {
	      recursive_expr->info.expr.arg2 =
		pt_eval_recursive_expr_type (parser,
					     recursive_expr->info.expr.arg2);
	    }
	}
    }

  return pt_eval_expr_type (parser, recursive_expr);
}

/*
 * pt_eval_type_pre () -
 *   return:
 *   parser(in):
 *   node(in):
 *   arg(in):
 *   continue_walk(in):
 */
static PT_NODE *
pt_eval_type_pre (PARSER_CONTEXT * parser, PT_NODE * node,
		  void *arg, int *continue_walk)
{
  PT_NODE *arg1, *arg2;
  PT_NODE *derived_table;
  SEMANTIC_CHK_INFO *sc_info = (SEMANTIC_CHK_INFO *) arg;

  /* To ensure that after exit of recursive expression node the evaluation of
     type will continue in normal mode */
  *continue_walk = PT_CONTINUE_WALK;
  if (sc_info->donot_fold == true)
    {				/* skip folding */
      return node;
    }

  switch (node->node_type)
    {
    case PT_SPEC:
      derived_table = node->info.spec.derived_table;
      if (pt_is_query (derived_table))
	{
	  /* exclude outer join spec from folding */
	  derived_table->info.query.has_outer_spec =
	    (node->info.spec.join_type == PT_JOIN_LEFT_OUTER
	     || node->info.spec.join_type == PT_JOIN_RIGHT_OUTER
	     || (node->next
		 && (node->next->info.spec.join_type == PT_JOIN_LEFT_OUTER
		     || (node->next->info.spec.join_type ==
			 PT_JOIN_RIGHT_OUTER)))) ? 1 : 0;
	}
      break;

    case PT_SORT_SPEC:
      /* if sort spec expression is query, mark it as such */
      if (node->info.sort_spec.expr
	  && PT_IS_QUERY (node->info.sort_spec.expr))
	{
	  node->info.sort_spec.expr->info.query.is_sort_spec = 1;
	}
      break;

    case PT_UNION:
    case PT_DIFFERENCE:
    case PT_INTERSECTION:
      /* propagate to children */
      arg1 = node->info.query.q.union_.arg1;
      arg2 = node->info.query.q.union_.arg2;
      arg1->info.query.has_outer_spec = node->info.query.has_outer_spec;
      arg2->info.query.has_outer_spec = node->info.query.has_outer_spec;

      /* rewrite limit clause as numbering expression and add it
       * to the corresponding predicate
       */
      if (node->info.query.limit && node->info.query.rewrite_limit)
	{
	  PT_NODE *limit, *t_node;
	  PT_NODE **expr_pred;

	  /* If both ORDER BY clause and LIMIT clause are specified,
	   * we will rewrite LIMIT to ORDERBY_NUM().
	   * For example,
	   *   (SELECT ...) UNION (SELECT ...) ORDER BY a LIMIT 10
	   *   will be rewritten to:
	   *   (SELECT ...) UNION (SELECT ...) ORDER BY a FOR ORDERBY_NUM() <= 10
	   *
	   * If LIMIT clause is only specified, we will rewrite the query
	   * at query optimization step. See qo_rewrite_queries() function
	   * for more information.
	   */
	  if (node->info.query.order_by != NULL)
	    {
	      expr_pred = &node->info.query.orderby_for;
	      limit = pt_limit_to_numbering_expr (parser,
						  node->info.query.limit,
						  PT_ORDERBY_NUM, false);
	      if (limit != NULL)
		{
		  t_node = *expr_pred;
		  while (t_node && t_node->next)
		    {
		      t_node = t_node->next;
		    }
		  if (!t_node)
		    {
		      t_node = *expr_pred = limit;
		    }
		  else
		    {
		      t_node->next = limit;
		    }

		  node->info.query.rewrite_limit = 0;
		}
	      else
		{
		  PT_ERRORm (parser, node, MSGCAT_SET_PARSER_SEMANTIC,
			     MSGCAT_SEMANTIC_OUT_OF_MEMORY);
		}
	    }
	}
      break;

    case PT_SELECT:
      /* rewrite limit clause as numbering expression and add it
       * to the corresponding predicate
       */
      if (node->info.query.limit && node->info.query.rewrite_limit)
	{
	  PT_NODE *limit, *t_node;
	  PT_NODE **expr_pred;

	  if (node->info.query.order_by)
	    {
	      expr_pred = &node->info.query.orderby_for;
	      limit = pt_limit_to_numbering_expr (parser,
						  node->info.query.limit,
						  PT_ORDERBY_NUM, false);
	    }
	  else if (node->info.query.q.select.group_by)
	    {
	      expr_pred = &node->info.query.q.select.having;
	      limit = pt_limit_to_numbering_expr (parser,
						  node->info.query.limit, 0,
						  true);
	    }
	  else
	    {
	      expr_pred = &node->info.query.q.select.where;
	      limit = pt_limit_to_numbering_expr (parser,
						  node->info.query.limit,
						  PT_INST_NUM, false);
	    }

	  if (limit)
	    {
	      t_node = *expr_pred;
	      while (t_node && t_node->next)
		{
		  t_node = t_node->next;
		}
	      if (!t_node)
		{
		  t_node = *expr_pred = limit;
		}
	      else
		{
		  t_node->next = limit;
		}

	      node->info.query.rewrite_limit = 0;
	    }
	  else
	    {
	      PT_ERRORm (parser, node, MSGCAT_SET_PARSER_SEMANTIC,
			 MSGCAT_SEMANTIC_OUT_OF_MEMORY);
	    }
	}
      break;

    case PT_INSERT:
      /* mark inserted sub-query as belonging to insert statement */
      if (node->info.insert.value_clauses->info.node_list.list_type ==
	  PT_IS_SUBQUERY)
	{
	  node->info.insert.value_clauses->info.node_list.list->info.query.
	    is_insert_select = 1;
	}
      break;

    case PT_DELETE:
      /* rewrite limit clause as numbering expression and add it
       * to search condition
       */
      if (node->info.delete_.limit && node->info.delete_.rewrite_limit)
	{
	  PT_NODE *t_node = node->info.delete_.search_cond;
	  PT_NODE *limit =
	    pt_limit_to_numbering_expr (parser, node->info.delete_.limit,
					PT_INST_NUM, false);
	  if (limit)
	    {
	      while (t_node && t_node->next)
		{
		  t_node = t_node->next;
		}
	      if (!t_node)
		{
		  node->info.delete_.search_cond = limit;
		}
	      else
		{
		  t_node->next = limit;
		}

	      node->info.delete_.rewrite_limit = 0;
	    }
	  else
	    {
	      PT_ERRORm (parser, node, MSGCAT_SET_PARSER_SEMANTIC,
			 MSGCAT_SEMANTIC_OUT_OF_MEMORY);
	    }
	}
      break;

    case PT_UPDATE:
      /* rewrite limit clause as numbering expression and add it
       * to search condition
       */
      if (node->info.update.limit && node->info.update.rewrite_limit)
	{
	  PT_NODE **expr_pred = NULL;
	  PT_NODE *t_node = NULL, *limit = NULL;

	  if (node->info.update.order_by)
	    {
	      expr_pred = &(node->info.update.orderby_for);
	      limit = pt_limit_to_numbering_expr (parser,
						  node->info.update.limit,
						  PT_ORDERBY_NUM, false);
	    }
	  else
	    {
	      expr_pred = &(node->info.update.search_cond);
	      limit = pt_limit_to_numbering_expr (parser,
						  node->info.update.limit,
						  PT_INST_NUM, false);
	    }
	  if (limit)
	    {
	      *expr_pred = parser_append_node (limit, *expr_pred);
	      node->info.update.rewrite_limit = 0;
	    }
	  else
	    {
	      PT_ERRORm (parser, node, MSGCAT_SET_PARSER_SEMANTIC,
			 MSGCAT_SEMANTIC_OUT_OF_MEMORY);
	    }
	}
      break;

    case PT_EXPR:
      {
	PT_NODE *recurs_expr = node, *node_tmp = NULL;
	PT_TYPE_ENUM common_type = PT_TYPE_NONE;
	bool has_enum = false;
	PT_NODE **recurs_arg = NULL, **norm_arg = NULL;
	PT_OP_TYPE op = recurs_expr->info.expr.op;

	/* Because the recursive expressions with more than two
	   arguments are build as PT_GREATEST(PT_GREATEST(..., argn-1), argn)
	   we need to compute the common type between all arguments in order to
	   give a correct return type. Let's say we have the following call
	   to PT_GREATEST: greatest(e1, e2, e3, 2) where e1, e2, e3 are ENUMs.
	   The internal form is rewrited to
	   greatest(greatest(greatest(e1, e2), e3), 2). For the inner call the
	   common type will be STRING. For middle call the common type will be
	   STRING and for the outer call the common type will be DOUBLE and both
	   arguments will be casted to DOUBLE including the returned STRING
	   of the middle (or even inner) call. If the string does not have
	   a numeric format, the call will fail. The natural behaviour is a
	   conversion of all enum arguments to the type of '2' (integer). So we
	   compute the common type of all arguments and store it in the
	   recursive_type member of the PT_EXPR_INFO structure and use it in
	   pt_eval_type function (and other places) as common type. */
	if (!PT_IS_RECURSIVE_EXPRESSION (node)
	    || node->info.expr.recursive_type != PT_TYPE_NONE)
	  {
	    break;
	  }

	while (recurs_expr != NULL
	       && PT_IS_RECURSIVE_EXPRESSION (recurs_expr)
	       && op == recurs_expr->info.expr.op)
	  {
	    if (PT_IS_LEFT_RECURSIVE_EXPRESSION (recurs_expr))
	      {
		norm_arg = &recurs_expr->info.expr.arg2;
		recurs_arg = &recurs_expr->info.expr.arg1;
	      }
	    else
	      {
		norm_arg = &recurs_expr->info.expr.arg1;
		recurs_arg = &recurs_expr->info.expr.arg2;
	      }
	    /* In order to correctly compute the common type we need to
	       know the type of each argument and therefore we compute
	       it. */
	    node_tmp = pt_semantic_type (parser, *norm_arg, arg);
	    if (*norm_arg == NULL || pt_has_error (parser))
	      {
		return node;
	      }
	    *norm_arg = node_tmp;
	    if ((*norm_arg)->type_enum != PT_TYPE_ENUMERATION)
	      {
		if (common_type == PT_TYPE_NONE)
		  {
		    common_type = (*norm_arg)->type_enum;
		  }
		else
		  {
		    common_type =
		      pt_common_type (common_type, (*norm_arg)->type_enum);
		  }
	      }
	    else
	      {
		has_enum = true;
	      }
	    if (*recurs_arg == NULL
		|| !PT_IS_RECURSIVE_EXPRESSION (*recurs_arg)
		|| op != (*recurs_arg)->info.expr.op)
	      {
		node_tmp = pt_semantic_type (parser, *recurs_arg, arg);
		if (node_tmp == NULL || pt_has_error (parser))
		  {
		    return node;
		  }
		*recurs_arg = node_tmp;
		if ((*recurs_arg)->type_enum != PT_TYPE_ENUMERATION)
		  {
		    if (common_type == PT_TYPE_NONE)
		      {
			common_type = (*recurs_arg)->type_enum;
		      }
		    else
		      {
			common_type =
			  pt_common_type (common_type,
					  (*recurs_arg)->type_enum);
		      }
		  }
		else
		  {
		    has_enum = true;
		  }
	      }
	    if (recurs_expr->info.expr.arg3 != NULL)
	      {
		node_tmp =
		  pt_semantic_type (parser, recurs_expr->info.expr.arg3, arg);
		if (node_tmp == NULL || pt_has_error (parser))
		  {
		    return node;
		  }
		recurs_expr->info.expr.arg3 = node_tmp;
	      }
	    recurs_expr = *recurs_arg;
	  }

	if (has_enum)
	  {
	    if (common_type == PT_TYPE_NONE)
	      {
		/* Proceed to normal type evaluation: each function node
		   evaluates only its own arguments */
		break;
	      }
	    common_type = pt_common_type (common_type, PT_TYPE_ENUMERATION);
	    recurs_expr = node;
	    while (PT_IS_RECURSIVE_EXPRESSION (recurs_expr)
		   && op == recurs_expr->info.expr.op)
	      {
		recurs_expr->info.expr.recursive_type = common_type;

		if (PT_IS_LEFT_RECURSIVE_EXPRESSION (recurs_expr))
		  {
		    recurs_expr = recurs_expr->info.expr.arg1;
		  }
		else
		  {
		    recurs_expr = recurs_expr->info.expr.arg2;
		  }
	      }
	  }

	node = pt_eval_recursive_expr_type (parser, node);

	if (op == PT_DECODE || op == PT_CASE)
	  {
	    /* the rest of recursive expressions are checked by normal
	     * collation inference */
	    if (pt_check_recursive_expr_collation (parser, &node) != NO_ERROR)
	      {
		node = NULL;
	      }
	  }
	/* Because for recursive functions we evaluate type here, we don't
	   need to evaluate it twice so we skip the normal path
	   of type evaluation */
	*continue_walk = PT_LIST_WALK;
	return node;
      }
      break;

    default:
      break;
    }

  return node;
}

/*
 * pt_fold_constants () - perform constant folding on the specified node
 *   return	: the node after constant folding
 *
 *   parser(in)	: the parser context
 *   node(in)	: the node to be folded
 *   arg(in)	:
 *   continue_walk(in):
 */
static PT_NODE *
pt_fold_constants (PARSER_CONTEXT * parser, PT_NODE * node, void *arg,
		   int *continue_walk)
{
  if (node == NULL)
    {
      return node;
    }

  switch (node->node_type)
    {
    case PT_EXPR:
      {
	node = pt_fold_const_expr (parser, node, arg);
	break;
      }
    case PT_FUNCTION:
      {
	node = pt_fold_const_function (parser, node);
	break;
      }
    default:
      break;
    }

  if (node == NULL)
    {
      PT_INTERNAL_ERROR (parser, "pt_fold_constants");
      return NULL;
    }
  return node;
}

/*
 * pt_eval_type () -
 *   return:
 *   parser(in):
 *   node(in):
 *   arg(in):
 *   continue_walk(in):
 */
static PT_NODE *
pt_eval_type (PARSER_CONTEXT * parser, PT_NODE * node,
	      void *arg, int *continue_walk)
{
  PT_NODE *dt = NULL, *arg1 = NULL, *arg2 = NULL;
  PT_NODE *spec = NULL;
  SEMANTIC_CHK_INFO *sc_info = (SEMANTIC_CHK_INFO *) arg;
  bool arg1_is_false = false;
  PT_NODE *list;

  switch (node->node_type)
    {
    case PT_EXPR:
      node = pt_eval_expr_type (parser, node);
      if (node == NULL)
	{
	  assert (false);
	  PT_INTERNAL_ERROR (parser, "pt_eval_type");
	  return NULL;
	}
      break;

    case PT_FUNCTION:
      node = pt_eval_function_type (parser, node);
      if (node == NULL)
	{
	  assert (false);
	  PT_INTERNAL_ERROR (parser, "pt_eval_type");
	  return NULL;
	}
      break;

    case PT_METHOD_CALL:
      node = pt_eval_method_call_type (parser, node);
      break;

    case PT_CREATE_INDEX:
      node->info.index.where = pt_where_type (parser, node->info.index.where);
      break;

    case PT_DELETE:
      node->info.delete_.search_cond =
	pt_where_type (parser, node->info.delete_.search_cond);
      break;

    case PT_UPDATE:
      node->info.update.search_cond =
	pt_where_type (parser, node->info.update.search_cond);
      break;

    case PT_MERGE:
      node->info.merge.search_cond =
	pt_where_type (parser, node->info.merge.search_cond);
      node->info.merge.insert.search_cond =
	pt_where_type (parser, node->info.merge.insert.search_cond);
      node->info.merge.update.search_cond =
	pt_where_type (parser, node->info.merge.update.search_cond);
      node->info.merge.update.del_search_cond =
	pt_where_type (parser, node->info.merge.update.del_search_cond);

      if (parser->set_host_var == 0)
	{
	  PT_NODE *v;
	  PT_NODE *list = NULL;
	  PT_NODE *attr = NULL;
	  DB_DOMAIN *d;

	  /* insert part */
	  for (list = node->info.merge.insert.value_clauses; list != NULL;
	       list = list->next)
	    {
	      attr = node->info.merge.insert.attr_list;
	      for (v = list->info.node_list.list; v != NULL && attr != NULL;
		   v = v->next, attr = attr->next)
		{
		  if (PT_IS_HOSTVAR (v) && v->expected_domain == NULL)
		    {
		      d = pt_node_to_db_domain (parser, attr, NULL);
		      d = tp_domain_cache (d);
		      SET_EXPECTED_DOMAIN (v, d);
		      pt_preset_hostvar (parser, v);
		    }
		}
	    }
	}
      break;

    case PT_SELECT:
      if (node->info.query.q.select.list)
	{
	  /* for value query, compatibility check for rows */
	  if (PT_IS_VALUE_QUERY (node))
	    {
	      if (pt_check_type_compatibility_of_values_query (parser, node)
		  == NULL)
		{
		  break;
		}
	      list = node->info.query.q.select.list->info.node_list.list;
	      assert (list != NULL);

	      node->type_enum = list->type_enum;
	      dt = list->data_type;
	    }
	  else
	    {
	      node->type_enum = node->info.query.q.select.list->type_enum;
	      dt = node->info.query.q.select.list->data_type;
	    }

	  if (dt)
	    {
	      if (node->data_type)
		{
		  parser_free_tree (parser, node->data_type);
		}

	      node->data_type = parser_copy_tree_list (parser, dt);
	    }
	}

      for (spec = node->info.query.q.select.from; spec; spec = spec->next)
	{
	  if (spec->node_type == PT_SPEC && spec->info.spec.on_cond)
	    {
	      spec->info.spec.on_cond =
		pt_where_type (parser, spec->info.spec.on_cond);
	    }
	}

      node->info.query.q.select.where =
	pt_where_type (parser, node->info.query.q.select.where);

      if (sc_info->donot_fold == false
	  && (node->info.query.is_subquery == PT_IS_SUBQUERY
	      || node->info.query.is_subquery == PT_IS_UNION_SUBQUERY)
	  && pt_false_where (parser, node))
	{
	  node = pt_to_false_subquery (parser, node);
	}
      else
	{
	  node->info.query.q.select.connect_by =
	    pt_where_type_keep_true (parser,
				     node->info.query.q.select.connect_by);
	  node->info.query.q.select.start_with =
	    pt_where_type (parser, node->info.query.q.select.start_with);
	  node->info.query.q.select.after_cb_filter =
	    pt_where_type (parser, node->info.query.q.select.after_cb_filter);
	  node->info.query.q.select.having =
	    pt_where_type (parser, node->info.query.q.select.having);
	  node->info.query.orderby_for =
	    pt_where_type (parser, node->info.query.orderby_for);
	}
      break;

    case PT_DO:
      if (node->info.do_.expr)
	{
	  node->type_enum = node->info.do_.expr->type_enum;
	  dt = node->info.do_.expr->data_type;
	  if (dt)
	    {
	      if (node->data_type)
		{
		  parser_free_tree (parser, node->data_type);
		}

	      node->data_type = parser_copy_tree_list (parser, dt);
	    }
	}
      break;

    case PT_INSERT:
      if (node->info.insert.spec)
	{
	  node->type_enum = PT_TYPE_OBJECT;
	  dt = parser_new_node (parser, PT_DATA_TYPE);
	  if (dt)
	    {
	      dt->type_enum = PT_TYPE_OBJECT;
	      node->data_type = dt;
	      dt->info.data_type.entity =
		parser_copy_tree (parser,
				  node->info.insert.spec->info.spec.
				  flat_entity_list);
	    }
	}

      if (parser->set_host_var == 0)
	{
	  PT_NODE *v = NULL;
	  PT_NODE *list = NULL;
	  PT_NODE *attr = NULL;
	  DB_DOMAIN *d;

	  for (list = node->info.insert.value_clauses; list != NULL;
	       list = list->next)
	    {
	      attr = node->info.insert.attr_list;
	      for (v = list->info.node_list.list; v != NULL && attr != NULL;
		   v = v->next, attr = attr->next)
		{
		  if (PT_IS_HOSTVAR (v) && v->expected_domain == NULL)
		    {
		      d = pt_node_to_db_domain (parser, attr, NULL);
		      d = tp_domain_cache (d);
		      SET_EXPECTED_DOMAIN (v, d);
		      pt_preset_hostvar (parser, v);
		    }
		}
	    }
	}
      break;

    case PT_UNION:
    case PT_DIFFERENCE:
    case PT_INTERSECTION:
      arg1 = node->info.query.q.union_.arg1;
      arg2 = node->info.query.q.union_.arg2;

      arg1_is_false = pt_false_where (parser, arg1);
      if (arg1_is_false || pt_false_where (parser, arg2))
	{
	  node = pt_fold_union (parser, node, arg1_is_false);
	}
      else
	{
	  /* check that signatures are compatible */
	  if ((arg1 && PT_IS_VALUE_QUERY (arg1))
	      || (arg2 && PT_IS_VALUE_QUERY (arg2)))
	    {
	      if (pt_check_union_type_compatibility_of_values_query
		  (parser, node) == NULL)
		{
		  break;
		}
	    }
	  else
	    {
	      if (!pt_check_union_compatibility (parser, node))
		{
		  break;
		}
	    }

	  node->type_enum = node->info.query.q.union_.arg1->type_enum;
	  dt = node->info.query.q.union_.arg1->data_type;
	  if (dt)
	    {
	      node->data_type = parser_copy_tree_list (parser, dt);
	    }
	}
      break;

    case PT_VALUE:
    case PT_NAME:
    case PT_DOT_:
      /* these cases have types already assigned to them by
         parser and semantic name resolution. */
      break;

    case PT_HOST_VAR:
      if (node->type_enum == PT_TYPE_NONE
	  && node->info.host_var.var_type == PT_HOST_IN)
	{
	  /* type is not known yet (i.e, compile before bind a value) */
	  node->type_enum = PT_TYPE_MAYBE;
	}
      break;
    case PT_SET_OPT_LVL:
    case PT_GET_OPT_LVL:
      node = pt_eval_opt_type (parser, node);
      break;

    default:
      break;
    }

  return node;
}

/*
 * pt_chop_to_one_select_item () -
 *   return: none
 *   parser(in):
 *   node(in/out): an EXISTS subquery
 */
static void
pt_chop_to_one_select_item (PARSER_CONTEXT * parser, PT_NODE * node)
{
  if (pt_is_query (node))
    {
      if (node->node_type == PT_SELECT)
	{
	  /* chop to one select item */
	  if (node->info.query.q.select.list
	      && node->info.query.q.select.list->next)
	    {
	      parser_free_tree (parser, node->info.query.q.select.list->next);
	      node->info.query.q.select.list->next = NULL;
	    }
	}
      else
	{
	  pt_chop_to_one_select_item (parser, node->info.query.q.union_.arg1);
	  pt_chop_to_one_select_item (parser, node->info.query.q.union_.arg2);
	}

      /* remove unneeded order by */
      if (node->info.query.order_by && !node->info.query.q.select.connect_by)
	{
	  parser_free_tree (parser, node->info.query.order_by);
	  node->info.query.order_by = NULL;
	}
    }
}

/*
 * pt_append_query_select_list () - append to the query's lists attrs
 *
 *  result: query with all lists updated
 *  parser(in):
 *  query(in):
 *  attrs(in): list of attributes to append to the query
 */
PT_NODE *
pt_append_query_select_list (PARSER_CONTEXT * parser, PT_NODE * query,
			     PT_NODE * attrs)
{
  if (!attrs)
    {
      return query;
    }

  switch (query->node_type)
    {
    case PT_SELECT:
      {
	PT_NODE *select_list = query->info.query.q.select.list;
	query->info.query.q.select.list = parser_append_node (attrs,
							      select_list);
	break;
      }
    case PT_DIFFERENCE:
    case PT_INTERSECTION:
    case PT_UNION:
      {
	query->info.query.q.union_.arg1 =
	  pt_append_query_select_list (parser,
				       query->info.query.q.union_.arg1,
				       attrs);
	query->info.query.q.union_.arg1 =
	  pt_append_query_select_list (parser,
				       query->info.query.q.union_.arg2,
				       attrs);

	break;
      }
    default:
      {
	break;
      }
    }

  return query;
}

/*
 * pt_wrap_select_list_with_cast_op () - cast the nodes of a select list
 *				to the type new_type, using the specified
 *				precision and scale
 *   return	   : NO_ERROR on success, error code on failure
 *   parser(in)	   : parser context
 *   query(in)	   : the select query
 *   new_type(in)  : the new_type
 *   p(in)	   : precision
 *   s(in)	   : scale
 *   data_type(in) : the data type of new_type
 *   force_wrap(in): forces wrapping with cast for collatable nodes
 */
int
pt_wrap_select_list_with_cast_op (PARSER_CONTEXT * parser, PT_NODE * query,
				  PT_TYPE_ENUM new_type, int p, int s,
				  PT_NODE * data_type, bool force_wrap)
{
  int i = 0;
  PT_NODE *new_node = NULL;

  switch (query->node_type)
    {
    case PT_SELECT:
      {
	PT_NODE *item = NULL;
	PT_NODE *prev = NULL;
	PT_NODE *new_node = NULL;
	PT_NODE *select_list = NULL;
	PT_NODE *node_list = NULL;

	/* values query's select_list is pt_node_list-->pt_node_list-->... */
	if (PT_IS_VALUE_QUERY (query))
	  {
	    for (node_list = query->info.query.q.select.list;
		 node_list != NULL; node_list = node_list->next)
	      {
		select_list = node_list->info.node_list.list;

		prev = NULL;
		for (item = select_list; item != NULL;
		     prev = item, item = item->next)
		  {
		    new_node = NULL;
		    if (item->type_enum == new_type && !force_wrap)
		      {
			continue;
		      }

		    new_node =
		      pt_wrap_with_cast_op (parser, item, new_type, p, s,
					    data_type);
		    if (new_node == NULL)
		      {
			return ER_FAILED;
		      }

		    if (new_node != item)
		      {
			item = new_node;
			PT_SET_VALUE_QUERY (item);
			/* first node in the list */
			if (prev == NULL)
			  {
			    node_list->info.node_list.list = item;
			  }
			else
			  {
			    prev->next = item;
			  }
		      }
		  }
	      }
	  }
	else
	  {
	    select_list = pt_get_select_list (parser, query);

	    for (item = select_list; item != NULL;
		 prev = item, item = item->next)
	      {
		if (item->is_hidden_column)
		  {
		    continue;
		  }
		new_node = NULL;
		if (item->type_enum == new_type && !force_wrap)
		  {
		    continue;
		  }
		new_node =
		  pt_wrap_with_cast_op (parser, item, new_type, p, s,
					data_type);
		if (new_node == NULL)
		  {
		    return ER_FAILED;
		  }

		if (new_node != item)
		  {
		    item = new_node;
		    /* first node in the list */
		    if (prev == NULL)
		      {
			query->info.query.q.select.list = item;
		      }
		    else
		      {
			prev->next = item;
		      }
		  }
	      }
	  }
	break;
      }
    case PT_DIFFERENCE:
    case PT_INTERSECTION:
    case PT_UNION:
      {
	int err = NO_ERROR;
	/* wrap with cast union select values for queries arg1 and arg2 */
	err =
	  pt_wrap_select_list_with_cast_op (parser,
					    query->info.query.q.union_.arg1,
					    new_type, p, s, data_type,
					    force_wrap);
	if (err != NO_ERROR)
	  {
	    return err;
	  }

	err =
	  pt_wrap_select_list_with_cast_op (parser,
					    query->info.query.q.union_.arg2,
					    new_type, p, s, data_type,
					    force_wrap);
	if (err != NO_ERROR)
	  {
	    return err;
	  }

	break;
      }
    default:
      return 0;
      break;
    }
  return NO_ERROR;
}

/*
 * pt_wrap_collection_with_cast_op  () - wrap a node with a cast to a
 *					 collection type
 *
 *   return: the wrapped node or null on error
 *   parser(in)	  : parser context
 *   arg(in/out)  : the node to be wrapped
 *   set_type(in) : the collection type
 *   data_type(in): the type of the elements of the collection
 *   p(in)	  : the precision of the collection elements
 *   s(in)	  : the scale of the collection elements
 */

PT_NODE *
pt_wrap_collection_with_cast_op (PARSER_CONTEXT * parser, PT_NODE * arg,
				 PT_TYPE_ENUM set_type, PT_NODE * set_data)
{
  PT_NODE *new_att, *set_dt, *next_att;

  if (arg->node_type == PT_FUNCTION && set_data->next == NULL)
    {
      /* if the argument is function and domain to cast has only one type */
      switch (arg->info.function.function_type)
	{
	case F_SET:
	case F_MULTISET:
	case F_SEQUENCE:
	  {
	    PT_NODE **first = &arg->info.function.arg_list,
	      *arg_list = *first, *prev = NULL;
	    bool is_numeric = PT_IS_NUMERIC_TYPE (set_data->type_enum);

	    /* walk through set members and cast them to set_data->type_enum
	       if needed */
	    while (arg_list)
	      {
		next_att = arg_list->next;

		if (set_data->type_enum != arg_list->type_enum && (!is_numeric
								   ||
								   !PT_IS_NUMERIC_TYPE
								   (arg_list->
								    type_enum)))
		  {
		    /* Set the expected domain of host variable to type
		       set_data so that at runtime the host variable should
		       be casted to it if needed */
		    if (arg_list->type_enum == PT_TYPE_MAYBE)
		      {
			arg_list->expected_domain =
			  pt_data_type_to_db_domain (parser, set_data, NULL);
			pt_preset_hostvar (parser, arg_list);
		      }
		    else
		      {
			new_att =
			  pt_wrap_with_cast_op (parser, arg_list,
						set_data->type_enum,
						0, 0, set_data);
			if (!new_att)
			  {
			    PT_ERRORm (parser, arg,
				       MSGCAT_SET_PARSER_SEMANTIC,
				       MSGCAT_SEMANTIC_OUT_OF_MEMORY);
			    return NULL;
			  }

			if (prm_get_bool_value
			    (PRM_ID_RETURN_NULL_ON_FUNCTION_ERRORS))
			  {
			    PT_EXPR_INFO_SET_FLAG (arg_list,
						   PT_EXPR_INFO_CAST_NOFAIL);
			  }

			if (prev)
			  {
			    prev->next = new_att;
			    new_att->next = next_att;
			  }
			else
			  {
			    *first = new_att;
			  }
			arg_list = new_att;
		      }
		  }
		prev = arg_list;
		arg_list = next_att;
	      }
	  }
	  return arg;
	default:
	  break;
	}

    }

  next_att = arg->next;
  arg->next = NULL;
  new_att = parser_new_node (parser, PT_EXPR);
  set_dt = parser_new_node (parser, PT_DATA_TYPE);

  if (!new_att || !set_dt)
    {
      PT_ERRORm (parser, arg, MSGCAT_SET_PARSER_SEMANTIC,
		 MSGCAT_SEMANTIC_OUT_OF_MEMORY);
      return NULL;
    }

  /* move alias */
  new_att->line_number = arg->line_number;
  new_att->column_number = arg->column_number;
  new_att->alias_print = arg->alias_print;
  new_att->is_hidden_column = arg->is_hidden_column;
  arg->alias_print = NULL;


  /* set the data type of the collection */
  set_dt->type_enum = set_type;
  set_dt->data_type = parser_copy_tree_list (parser, set_data);

  new_att->type_enum = set_type;
  new_att->info.expr.op = PT_CAST;
  new_att->info.expr.cast_type = set_dt;
  new_att->info.expr.arg1 = arg;
  new_att->next = next_att;

  new_att->data_type = set_data;
  PT_EXPR_INFO_SET_FLAG (new_att, PT_EXPR_INFO_CAST_SHOULD_FOLD);
  return new_att;
}

/*
 * pt_wrap_with_cast_op () -
 *   return:
 *   parser(in):
 *   arg(in/out):
 *   new_type(in):
 *   p(in):
 *   s(in):
 *   desired_dt(in):
 */
PT_NODE *
pt_wrap_with_cast_op (PARSER_CONTEXT * parser, PT_NODE * arg,
		      PT_TYPE_ENUM new_type, int p, int s,
		      PT_NODE * desired_dt)
{
  PT_NODE *new_att, *new_dt, *next_att;

  next_att = arg->next;
  arg->next = NULL;
  new_att = parser_new_node (parser, PT_EXPR);
  if (new_att == NULL)
    {
      return NULL;
    }

  if (PT_IS_COLLECTION_TYPE (new_type))
    {
      new_dt = parser_new_node (parser, PT_DATA_TYPE);
      if (new_dt == NULL)
	{
	  return NULL;
	}
      new_dt->type_enum = new_type;
      new_dt->info.data_type.precision = p;
      new_dt->info.data_type.dec_precision = s;

      if (desired_dt != NULL && false
	  && !PT_IS_COLLECTION_TYPE (desired_dt->type_enum))
	{
	  /* desired_dt contains a list of types of elements from the
	   * collection
	   */
	  new_dt->data_type = parser_copy_tree_list (parser, desired_dt);
	}
      /* If desired_dt is not null but is a collection type, we can't actually
       * make a decision here because we can't make a distinction between
       * collection of collections and simple collections. In the case of
       * collection of collections, the correct type of each element will be
       * set when the collection is validated in the context in which it is
       * used
       */
    }
  else if (desired_dt == NULL)
    {
      new_dt = parser_new_node (parser, PT_DATA_TYPE);
      if (new_dt == NULL)
	{
	  return NULL;
	}
      new_dt->type_enum = new_type;
      new_dt->info.data_type.precision = p;
      new_dt->info.data_type.dec_precision = s;
      if (arg->data_type != NULL
	  && PT_HAS_COLLATION (arg->data_type->type_enum))
	{
	  new_dt->info.data_type.units = arg->data_type->info.data_type.units;
	  new_dt->info.data_type.collation_id =
	    arg->data_type->info.data_type.collation_id;
	}

    }
  else
    {
      new_dt = parser_copy_tree_list (parser, desired_dt);
    }

  /* move alias */
  new_att->line_number = arg->line_number;
  new_att->column_number = arg->column_number;
  new_att->alias_print = arg->alias_print;
  new_att->is_hidden_column = arg->is_hidden_column;
  arg->alias_print = NULL;

  new_att->type_enum = new_type;
  new_att->info.expr.op = PT_CAST;
  new_att->info.expr.cast_type = new_dt;
  new_att->info.expr.arg1 = arg;
  new_att->next = next_att;

  new_att->data_type = parser_copy_tree_list (parser, new_dt);
  PT_EXPR_INFO_SET_FLAG (new_att, PT_EXPR_INFO_CAST_SHOULD_FOLD);

  return new_att;
}

/*
 * pt_preset_hostvar () -
 *   return: none
 *   parser(in):
 *   hv_node(in):
 */
void
pt_preset_hostvar (PARSER_CONTEXT * parser, PT_NODE * hv_node)
{
  if (parser->host_var_count <= hv_node->info.host_var.index)
    {
      /* automated parameters are not needed an expected domain */
      return;
    }

  parser->host_var_expected_domains[hv_node->info.host_var.index] =
    hv_node->expected_domain;
}

/* pt_set_expected_domain - set the expected tomain of a PT_NODE
 *  return: void
 *  node (in/out) : the node to wich to set the expected domain
 *  domain (in)	  : the expected domain
 */
void
pt_set_expected_domain (PT_NODE * node, TP_DOMAIN * domain)
{
  PT_NODE *_or_next = NULL;
  node->expected_domain = domain;
  _or_next = node->or_next;
  while (_or_next)
    {
      if (_or_next->type_enum == PT_TYPE_MAYBE
	  && _or_next->expected_domain == NULL)
	{
	  _or_next->expected_domain = domain;
	}
      _or_next = _or_next->or_next;
    }
}

/*
 * pt_is_able_to_determine_return_type () -
 *   return: true if the type of the return value can be determined
 *             regardless of its arguments, otherwise false.
 *   op(in):
 */
static bool
pt_is_able_to_determine_return_type (const PT_OP_TYPE op)
{
  switch (op)
    {
    case PT_CAST:
    case PT_TO_NUMBER:
    case PT_TO_CHAR:
    case PT_TO_DATE:
    case PT_TO_TIME:
    case PT_TO_TIMESTAMP:
    case PT_TO_DATETIME:
    case PT_POSITION:
    case PT_FINDINSET:
    case PT_OCTET_LENGTH:
    case PT_BIT_LENGTH:
    case PT_CHAR_LENGTH:
    case PT_TIME_FORMAT:
    case PT_TIMESTAMP:
    case PT_UNIX_TIMESTAMP:
    case PT_SIGN:
    case PT_CHR:
    case PT_ADD_MONTHS:
    case PT_LAST_DAY:
    case PT_MONTHS_BETWEEN:
    case PT_DATE_ADD:
    case PT_ADDDATE:
    case PT_FORMAT:
    case PT_DATE_SUB:
    case PT_SUBDATE:
    case PT_DATE_FORMAT:
    case PT_STR_TO_DATE:
    case PT_SIN:
    case PT_COS:
    case PT_TAN:
    case PT_ASIN:
    case PT_ACOS:
    case PT_ATAN:
    case PT_ATAN2:
    case PT_COT:
    case PT_LOG:
    case PT_EXP:
    case PT_SQRT:
    case PT_DEGREES:
    case PT_LN:
    case PT_LOG2:
    case PT_LOG10:
    case PT_POWER:
    case PT_FIELD:
    case PT_LOCATE:
    case PT_STRCMP:
    case PT_RADIANS:
    case PT_BIT_AND:
    case PT_BIT_OR:
    case PT_BIT_XOR:
    case PT_BIT_NOT:
    case PT_BITSHIFT_LEFT:
    case PT_BITSHIFT_RIGHT:
    case PT_BIT_COUNT:
    case PT_DATEDIFF:
    case PT_TIMEDIFF:
    case PT_DATEF:
    case PT_TIMEF:
    case PT_ISNULL:
    case PT_RAND:
    case PT_DRAND:
    case PT_RANDOM:
    case PT_DRANDOM:
    case PT_BIT_TO_BLOB:
    case PT_BLOB_FROM_FILE:
    case PT_BLOB_LENGTH:
    case PT_BLOB_TO_BIT:
    case PT_CHAR_TO_BLOB:
    case PT_CHAR_TO_CLOB:
    case PT_CLOB_FROM_FILE:
    case PT_CLOB_LENGTH:
    case PT_CLOB_TO_CHAR:
    case PT_TYPEOF:
    case PT_YEARF:
    case PT_MONTHF:
    case PT_DAYF:
    case PT_DAYOFMONTH:
    case PT_HOURF:
    case PT_MINUTEF:
    case PT_SECONDF:
    case PT_QUARTERF:
    case PT_WEEKDAY:
    case PT_DAYOFWEEK:
    case PT_DAYOFYEAR:
    case PT_TODAYS:
    case PT_FROMDAYS:
    case PT_TIMETOSEC:
    case PT_SECTOTIME:
    case PT_WEEKF:
    case PT_MAKEDATE:
    case PT_MAKETIME:
    case PT_BIN:
    case PT_CASE:
    case PT_DECODE:
    case PT_LIKE:
    case PT_NOT_LIKE:
    case PT_RLIKE:
    case PT_NOT_RLIKE:
    case PT_RLIKE_BINARY:
    case PT_NOT_RLIKE_BINARY:
    case PT_EVALUATE_VARIABLE:
    case PT_DEFINE_VARIABLE:
    case PT_HEX:
    case PT_ASCII:
    case PT_CONV:
    case PT_TO_ENUMERATION_VALUE:
    case PT_INET_ATON:
    case PT_INET_NTOA:
    case PT_CHARSET:
    case PT_COERCIBILITY:
    case PT_COLLATION:
    case PT_WIDTH_BUCKET:
      return true;

    default:
      return false;
    }
}

/*
 * pt_get_common_datetime_type () -
 *   return:
 *   common_type(in):
 *   arg1_type(in):
 *   arg2_type(in):
 *   arg1(in):
 *   arg1(in):
 */
static PT_TYPE_ENUM
pt_get_common_datetime_type (PARSER_CONTEXT * parser,
			     PT_TYPE_ENUM common_type,
			     PT_TYPE_ENUM arg1_type,
			     PT_TYPE_ENUM arg2_type,
			     PT_NODE * arg1, PT_NODE * arg2)
{
  PT_TYPE_ENUM arg_type, arg_base_type;
  PT_NODE *arg_ptr = NULL;
  PT_NODE *arg_base = NULL;

  assert (arg1_type != arg2_type);

  if (arg1_type == common_type)
    {
      arg_base_type = arg1_type;
      arg_base = arg1;
      arg_type = arg2_type;
      arg_ptr = arg2;
    }
  else
    {
      arg_base_type = arg2_type;
      arg_base = arg2;
      arg_type = arg1_type;
      arg_ptr = arg1;
    }

  if (PT_IS_CHAR_STRING_TYPE (arg_type))
    {
      if (pt_coerce_str_to_time_date_utime_datetime
	  (parser, arg_ptr, &arg_type) == NO_ERROR)
	{
	  PT_TYPE_ENUM result_type = pt_common_type (arg_base_type, arg_type);

	  if (arg_type != arg_base_type)
	    {
	      return pt_get_common_datetime_type (parser, result_type,
						  arg_base_type, arg_type,
						  arg_base, arg_ptr);
	    }

	  return result_type;
	}
    }
  else if (PT_IS_DATE_TIME_TYPE (arg_type))
    {
      if (pt_coerce_value (parser, arg_ptr, arg_ptr, common_type, NULL) ==
	  NO_ERROR)
	{
	  return common_type;
	}
    }
  else if (PT_IS_NUMERIC_TYPE (arg_type))
    {
      return common_type;
    }

  return PT_TYPE_NONE;
}

/*
 * pt_eval_expr_type () -
 *   return:
 *   parser(in):
 *   node(in):
 */
static PT_NODE *
pt_eval_expr_type (PARSER_CONTEXT * parser, PT_NODE * node)
{
  PT_OP_TYPE op;
  PT_NODE *arg1 = NULL, *arg2 = NULL, *arg3 = NULL;
  PT_NODE *arg1_hv = NULL, *arg2_hv = NULL, *arg3_hv = NULL;
  PT_TYPE_ENUM arg1_type = PT_TYPE_NONE, arg2_type = PT_TYPE_NONE;
  PT_TYPE_ENUM arg3_type = PT_TYPE_NONE, common_type = PT_TYPE_NONE;
  int arg1_cnt = -1, arg2_cnt = -1;
  TP_DOMAIN *d;
  PT_NODE *cast_type;
  PT_NODE *new_att;
  PT_TYPE_ENUM new_type;
  int first_node;
  PT_NODE *expr = NULL;
  bool check_expr_coll = true;

  /* by the time we get here, the leaves have already been typed.
   * this is because this function is called from a post function
   *  of a parser_walk_tree, after all leaves have been visited.
   */

  op = node->info.expr.op;
  if (op == PT_EQ || op == PT_NULLSAFE_EQ || op == PT_IS_IN
      || op == PT_EQ_SOME)
    {
      /* handle special cases for the enumeration type */
      node = pt_fix_enumeration_comparison (parser, node);
      if (node == NULL)
	{
	  return NULL;
	}
      op = node->info.expr.op;
      if (pt_has_error (parser))
	{
	  goto error;
	}
    }
  /* shortcut for FUNCTION HOLDER */
  if (op == PT_FUNCTION_HOLDER)
    {
      PT_NODE *func = NULL;
      /* this may be a 2nd pass, tree may be already const folded */
      if (node->info.expr.arg1->node_type == PT_FUNCTION)
	{
	  func = pt_eval_function_type (parser, node->info.expr.arg1);
	  node->type_enum = func->type_enum;
	  if (node->data_type == NULL && func->data_type != NULL)
	    {
	      node->data_type = parser_copy_tree (parser, func->data_type);
	    }
	}
      else
	{
	  assert (node->info.expr.arg1->node_type == PT_VALUE);
	}
      return node;
    }


  arg1 = node->info.expr.arg1;
  if (arg1)
    {
      arg1_type = arg1->type_enum;

      if (arg1->node_type == PT_HOST_VAR && arg1->type_enum == PT_TYPE_MAYBE)
	{
	  arg1_hv = arg1;
	}

      /* Special case handling for unary operators on host variables
       * (-?) or (prior ?) or (connect_by_root ?)
       */
      if (arg1->node_type == PT_EXPR
	  && (arg1->info.expr.op == PT_UNARY_MINUS
	      || arg1->info.expr.op == PT_PRIOR
	      || arg1->info.expr.op == PT_CONNECT_BY_ROOT
	      || arg1->info.expr.op == PT_QPRIOR
	      || arg1->info.expr.op == PT_BIT_NOT
	      || arg1->info.expr.op == PT_BIT_COUNT)
	  && arg1->type_enum == PT_TYPE_MAYBE
	  && arg1->info.expr.arg1->node_type == PT_HOST_VAR
	  && arg1->info.expr.arg1->type_enum == PT_TYPE_MAYBE)
	{
	  arg1_hv = arg1->info.expr.arg1;
	}
    }

  arg2 = node->info.expr.arg2;
  if (arg2)
    {
      if (arg2->or_next == NULL)
	{
	  arg2_type = arg2->type_enum;
	}
      else
	{
	  PT_NODE *temp;
	  PT_TYPE_ENUM temp_type;

	  common_type = PT_TYPE_NONE;
	  /* do traverse multi-args in RANGE operator */
	  for (temp = arg2; temp; temp = temp->or_next)
	    {
	      temp_type = pt_common_type (arg1_type, temp->type_enum);
	      if (temp_type != PT_TYPE_NONE)
		{
		  common_type = (common_type == PT_TYPE_NONE)
		    ? temp_type : pt_common_type (common_type, temp_type);
		}
	    }
	  arg2_type = common_type;
	}

      if (arg2->node_type == PT_HOST_VAR && arg2->type_enum == PT_TYPE_MAYBE)
	{
	  arg2_hv = arg2;
	}

      /* Special case handling for unary operators on host variables
       * (-?) or (prior ?) or (connect_by_root ?)
       */
      if (arg2->node_type == PT_EXPR
	  && (arg2->info.expr.op == PT_UNARY_MINUS
	      || arg2->info.expr.op == PT_PRIOR
	      || arg2->info.expr.op == PT_CONNECT_BY_ROOT
	      || arg2->info.expr.op == PT_QPRIOR
	      || arg2->info.expr.op == PT_BIT_NOT
	      || arg2->info.expr.op == PT_BIT_COUNT)
	  && arg2->type_enum == PT_TYPE_MAYBE
	  && arg2->info.expr.arg1->node_type == PT_HOST_VAR
	  && arg2->info.expr.arg1->type_enum == PT_TYPE_MAYBE)
	{
	  arg2_hv = arg2->info.expr.arg1;
	}
    }

  arg3 = node->info.expr.arg3;
  if (arg3)
    {
      arg3_type = arg3->type_enum;
      if (arg3->node_type == PT_HOST_VAR && arg3->type_enum == PT_TYPE_MAYBE)
	{
	  arg3_hv = arg3;
	}
    }

  /*
   * At this point, arg1_hv is non-NULL (and equal to arg1) if it represents
   * a dynamic host variable, i.e., a host var parameter that hasn't had
   * a value supplied at compile time.  Same for arg2_hv and arg3_hv...
   */
  common_type = arg1_type;
  expr = node;

  /* adjust expression definition to fit the signature implementation */
  switch (op)
    {
    case PT_PLUS:
      {
	if (arg1_type == PT_TYPE_NULL || arg2_type == PT_TYPE_NULL)
	  {
	    if (prm_get_bool_value (PRM_ID_ORACLE_STYLE_EMPTY_STRING) == false
		|| (!PT_IS_STRING_TYPE (arg1_type)
		    && !PT_IS_STRING_TYPE (arg2_type)))
	      {
		node->type_enum = PT_TYPE_NULL;
		goto error;
	      }
	  }
	if (arg1_type == PT_TYPE_MAYBE || arg2_type == PT_TYPE_MAYBE)
	  {
	    node->type_enum = PT_TYPE_MAYBE;
	    goto cannot_use_signature;
	  }
	if (prm_get_integer_value (PRM_ID_COMPAT_MODE) == COMPAT_MYSQL)
	  {
	    /* in mysql mode, PT_PLUS is not defined on date and number */
	    break;
	  }
	/* PT_PLUS has four overloads for which we cannot apply symmetric rule
	   1. DATE/TIME type + NUMBER
	   2. NUMBER + DATE/TIME type
	   3. DATE/TIME type + STRING
	   4. STRING + DATE/TIME type
	   STRING/NUMBER operand involved is coerced to BIGINT.
	   For these overloads, PT_PLUS is a syntactic sugar for the ADD_DATE
	   expression. Even though both PLUS and MINUS have this behavior,
	   we cannot treat them in the same place because, for this case,
	   PT_PLUS is commutative and PT_MINUS isn't
	 */
	if (PT_IS_DATE_TIME_TYPE (arg1_type)
	    && (PT_IS_NUMERIC_TYPE (arg2_type)
		|| PT_IS_CHAR_STRING_TYPE (arg2_type)
		|| arg2_type == PT_TYPE_ENUMERATION
		|| arg2_type == PT_TYPE_MAYBE))
	  {
	    if (!PT_IS_DISCRETE_NUMBER_TYPE (arg2_type))
	      {
		/* coerce first argument to BIGINT */
		int err = pt_coerce_expression_argument (parser, node, &arg2,
							 PT_TYPE_BIGINT,
							 NULL);
		if (err != NO_ERROR)
		  {
		    node->type_enum = PT_TYPE_NONE;
		    goto error;
		  }
	      }
	    node->info.expr.arg2 = arg2;
	    node->type_enum = arg1_type;
	    goto error;
	  }
	if (PT_IS_DATE_TIME_TYPE (arg2_type)
	    && (PT_IS_NUMERIC_TYPE (arg1_type)
		|| PT_IS_CHAR_STRING_TYPE (arg1_type)
		|| arg1_type == PT_TYPE_ENUMERATION
		|| arg1_type == PT_TYPE_MAYBE))
	  {
	    if (!PT_IS_DISCRETE_NUMBER_TYPE (arg1_type))
	      {
		int err = pt_coerce_expression_argument (parser, node, &arg1,
							 PT_TYPE_BIGINT,
							 NULL);
		if (err != NO_ERROR)
		  {
		    node->type_enum = PT_TYPE_NONE;
		    goto error;
		  }
	      }
	    node->info.expr.arg1 = arg1;
	    node->type_enum = arg2_type;
	    goto error;
	  }
	break;
      }
    case PT_MINUS:
      {
	if (arg1_type == PT_TYPE_NULL || arg2_type == PT_TYPE_NULL)
	  {
	    node->type_enum = PT_TYPE_NULL;
	    goto error;
	  }
	if (arg1_type == PT_TYPE_MAYBE || arg2_type == PT_TYPE_MAYBE)
	  {
	    node->type_enum = PT_TYPE_MAYBE;
	    goto cannot_use_signature;
	  }
	if (prm_get_integer_value (PRM_ID_COMPAT_MODE) == COMPAT_MYSQL)
	  {
	    /* in mysql mode - does is not defined on date and number */
	    break;
	  }
	if (PT_IS_DATE_TIME_TYPE (arg1_type)
	    && (PT_IS_NUMERIC_TYPE (arg2_type)
		|| arg2_type == PT_TYPE_ENUMERATION))
	  {
	    if (!PT_IS_DISCRETE_NUMBER_TYPE (arg2_type))
	      {
		/* coerce arg2 to bigint */
		int err = pt_coerce_expression_argument (parser, expr, &arg2,
							 PT_TYPE_BIGINT,
							 NULL);
		if (err != NO_ERROR)
		  {
		    node->type_enum = PT_TYPE_NONE;
		    goto error;
		  }
		node->info.expr.arg2 = arg2;
	      }
	    node->type_enum = arg1_type;
	    goto error;
	  }
	break;
      }
    case PT_BETWEEN_AND:
    case PT_BETWEEN_GE_LE:
    case PT_BETWEEN_GE_LT:
    case PT_BETWEEN_GT_LE:
    case PT_BETWEEN_GT_LT:
      {
	/* these expressions will be handled by PT_BETWEEN */
	node->type_enum = pt_common_type (arg1_type, arg2_type);
	goto error;
	break;
      }
    case PT_BETWEEN:
    case PT_NOT_BETWEEN:
      {
	/* between and rage operators are written like:
	   PT_BETWEEN(arg1, PT_BETWEEN_AND(arg2,arg3))
	   We convert it to PT_BETWEEN(arg1, arg2, arg2) to be able to
	   decide the correct common type of all arguments and we will
	   convert it back once we apply the correct casts */
	if (pt_is_between_range_op (arg2->info.expr.op))
	  {
	    arg2 = node->info.expr.arg2;
	    node->info.expr.arg2 = arg2->info.expr.arg1;
	    node->info.expr.arg3 = arg2->info.expr.arg2;
	  }
	break;
      }
    case PT_LIKE:
    case PT_NOT_LIKE:
      {
	/* [NOT] LIKE operators with an escape clause are parsed like
	   PT_LIKE(arg1, PT_LIKE_ESCAPE(arg2, arg3)).
	   We convert it to PT_LIKE(arg1, arg2, arg3) to be able
	   to decide the correct common type of all arguments and we
	   will convert it back once we apply the correct casts.

	   A better approach would be to modify the parser to output
	   PT_LIKE(arg1, arg2, arg3) directly. */

	if (arg2->node_type == PT_EXPR
	    && arg2->info.expr.op == PT_LIKE_ESCAPE)
	  {
	    arg2 = node->info.expr.arg2;
	    node->info.expr.arg2 = arg2->info.expr.arg1;
	    node->info.expr.arg3 = arg2->info.expr.arg2;
	  }
	break;
      }

    case PT_LIKE_LOWER_BOUND:
    case PT_LIKE_UPPER_BOUND:
      /* Check if arguments have been handled by PT_LIKE and only the result
       * type needs to be set */
      if (arg1->type_enum == PT_TYPE_MAYBE && arg1->expected_domain)
	{
	  node->type_enum =
	    pt_db_to_type_enum (TP_DOMAIN_TYPE (arg1->expected_domain));
	  goto error;
	}
      break;

    case PT_IS_IN:
    case PT_IS_NOT_IN:
      if (arg2->node_type == PT_VALUE)
	{
	  if (PT_IS_COLLECTION_TYPE (arg2->type_enum)
	      && arg2->info.value.data_value.set
	      && arg2->info.value.data_value.set->next == NULL)
	    {
	      /* only one element in set. convert expr as EQ/NE expr. */
	      PT_NODE *new_arg2;

	      new_arg2 = arg2->info.value.data_value.set;

	      /* free arg2 */
	      arg2->info.value.data_value.set = NULL;
	      parser_free_tree (parser, node->info.expr.arg2);

	      /* rewrite arg2 */
	      node->info.expr.arg2 = new_arg2;
	      node->info.expr.op = (op == PT_IS_IN) ? PT_EQ : PT_NE;
	    }
	  else if (PT_IS_NULL_NODE (arg2))
	    {
	      return node;
	    }
	}
      break;

    case PT_TO_CHAR:
      if (PT_IS_CHAR_STRING_TYPE (arg1_type))
	{
	  arg1->line_number = node->line_number;
	  arg1->column_number = node->column_number;
	  arg1->alias_print = node->alias_print;
	  node->alias_print = NULL;
	  arg1->next = node->next;
	  node->next = NULL;
	  if (arg1->node_type == PT_EXPR)
	    {
	      arg1->info.expr.location = node->info.expr.location;
	    }
	  else if (arg1->node_type == PT_VALUE)
	    {
	      arg1->info.value.location = node->info.expr.location;
	    }
	  node->info.expr.arg1 = NULL;
	  parser_free_node (parser, node);
	  return arg1;
	}
      else if (PT_IS_NUMERIC_TYPE (arg1_type))
	{
	  bool has_user_format = false;
	  bool has_user_lang = false;
	  const char *lang_str;

	  assert (arg3 != NULL && arg3->node_type == PT_VALUE
		  && arg3_type == PT_TYPE_INTEGER);
	  /* change locale from date_lang (set by grammar) to number_lang */
	  (void) lang_get_lang_id_from_flag (arg3->info.value.data_value.i,
					     &has_user_format,
					     &has_user_lang);
	  if (!has_user_lang)
	    {
	      int lang_flag;
	      lang_str = prm_get_string_value (PRM_ID_INTL_NUMBER_LANG);
	      (void) lang_set_flag_from_lang (lang_str, has_user_format,
					      has_user_lang, &lang_flag);
	      arg3->info.value.data_value.i = lang_flag;
	      arg3->info.value.db_value_is_initialized = 0;
	      pt_value_to_db (parser, arg3);
	    }
	}

      break;

    default:
      break;
    }

  if (pt_apply_expressions_definition (parser, &expr) != NO_ERROR)
    {
      expr = NULL;
      node->type_enum = PT_TYPE_NONE;
      goto error;
    }

  if (expr != NULL && PT_GET_COLLATION_MODIFIER (expr) != -1)
    {
      if (!(PT_HAS_COLLATION (arg1_type) || PT_HAS_COLLATION (arg2_type)
	    || PT_HAS_COLLATION (arg3_type)
	    || PT_HAS_COLLATION (node->type_enum)
	    || (expr->info.expr.op == PT_CAST && arg1_type == PT_TYPE_MAYBE)))
	{
	  if (!pt_has_error (parser))
	    {
	      PT_ERRORm (parser, node, MSGCAT_SET_PARSER_SEMANTIC,
			 MSGCAT_SEMANTIC_COLLATE_NOT_ALLOWED);
	    }
	  node->type_enum = PT_TYPE_NONE;
	  goto error;
	}
    }

  if (expr != NULL)
    {
      assert (check_expr_coll);
      if (pt_check_expr_collation (parser, &expr) != NO_ERROR)
	{
	  expr = NULL;
	  node->type_enum = PT_TYPE_NONE;
	  return node;
	}
    }

  check_expr_coll = false;

  if (expr != NULL)
    {
      expr = pt_wrap_expr_w_exp_dom_cast (parser, expr);

      node = expr;
      expr = NULL;

      switch (op)
	{
	case PT_BETWEEN:
	case PT_NOT_BETWEEN:
	  {
	    /* between and rage operators are written like:
	       PT_BETWEEN(arg1, PT_BETWEEN_AND(arg2,arg3))
	       We convert it to PT_BETWEEN(arg1, arg2, arg2) to be able to
	       decide the correct common type of all arguments and we will
	       convert it back once we apply the correct casts */
	    if (pt_is_between_range_op (arg2->info.expr.op))
	      {
		arg2->info.expr.arg1 = node->info.expr.arg2;
		arg2->info.expr.arg2 = node->info.expr.arg3;
		node->info.expr.arg2 = arg2;
		node->info.expr.arg3 = NULL;
	      }
	    break;
	  }
	case PT_LIKE:
	case PT_NOT_LIKE:
	  {
	    /* convert PT_LIKE(arg1, arg2, arg3) back to
	       PT_LIKE(arg1, PT_LIKE_ESCAPE(arg2, arg3))

	       A better approach would be to modify the parser to output
	       PT_LIKE(arg1, arg2, arg3) directly. */

	    if (arg2->node_type == PT_EXPR
		&& arg2->info.expr.op == PT_LIKE_ESCAPE)
	      {

		arg2->info.expr.arg1 = node->info.expr.arg2;
		arg2->info.expr.arg2 = node->info.expr.arg3;
		node->info.expr.arg2 = arg2;
		node->info.expr.arg3 = NULL;
	      }
	    break;
	  }
	case PT_RAND:
	case PT_RANDOM:
	  {
	    /* to keep mysql compatibility we should consider a NULL argument
	       as the value 0. This is the only place where we can perform
	       this check */
	    arg1 = node->info.expr.arg1;
	    if (arg1 && arg1->type_enum == PT_TYPE_NULL
		&& arg1->node_type == PT_VALUE)
	      {
		arg1->type_enum = arg1_type = PT_TYPE_INTEGER;
		db_make_int (&arg1->info.value.db_value, 0);
	      }
	    break;
	  }
	case PT_DRAND:
	case PT_DRANDOM:
	  {
	    /* to keep mysql compatibility we should consider a NULL argument
	       as the value 0. This is the only place where we can perform
	       this check */
	    arg1 = node->info.expr.arg1;
	    if (arg1 && arg1->type_enum == PT_TYPE_NULL
		&& arg1->node_type == PT_VALUE)
	      {
		arg1->type_enum = arg1_type = PT_TYPE_DOUBLE;
		db_make_double (&arg1->info.value.db_value, 0);
	      }
	    break;
	  }

	case PT_EXTRACT:
	  if (arg1_type == PT_TYPE_MAYBE)
	    {
	      assert (node->type_enum == PT_TYPE_INTEGER);
	    }
	  else if (arg1_type == PT_TYPE_NA || arg1_type == PT_TYPE_NULL)
	    {
	      node->type_enum = arg1_type;
	    }
	  else if (PT_IS_CHAR_STRING_TYPE (arg1_type) ||
		   PT_IS_DATE_TIME_TYPE (arg1_type))
	    {
	      int incompatible_extract_type = false;

	      node->type_enum = PT_TYPE_NONE;
	      switch (node->info.expr.qualifier)
		{
		case PT_YEAR:
		case PT_MONTH:
		case PT_DAY:
		  if (PT_IS_CHAR_STRING_TYPE (arg1_type))
		    {
		      arg1_type = PT_TYPE_NONE;
		      if (pt_check_and_coerce_to_date (parser, arg1) ==
			  NO_ERROR)
			{
			  arg1_type = PT_TYPE_DATE;
			}
		      else
			{
			  parser_free_tree (parser, parser->error_msgs);
			  parser->error_msgs = NULL;

			  /* try coercing to utime */
			  if (pt_coerce_value (parser, arg1, arg1,
					       PT_TYPE_TIMESTAMP,
					       NULL) == NO_ERROR)
			    {
			      arg1_type = PT_TYPE_TIMESTAMP;
			    }
			  else
			    {
			      parser_free_tree (parser, parser->error_msgs);
			      parser->error_msgs = NULL;

			      /* try coercing to datetime */
			      if (pt_coerce_value (parser, arg1, arg1,
						   PT_TYPE_DATETIME,
						   NULL) == NO_ERROR)
				{
				  arg1_type = PT_TYPE_DATETIME;
				}
			    }
			}
		    }

		  if (arg1_type == PT_TYPE_DATE
		      || arg1_type == PT_TYPE_TIMESTAMP
		      || arg1_type == PT_TYPE_DATETIME)
		    {
		      node->type_enum = PT_TYPE_INTEGER;
		    }
		  else if (arg1_type == PT_TYPE_TIME)
		    {
		      incompatible_extract_type = true;
		    }
		  break;

		case PT_HOUR:
		case PT_MINUTE:
		case PT_SECOND:
		  if (PT_IS_CHAR_STRING_TYPE (arg1_type))
		    {
		      arg1_type = PT_TYPE_NONE;
		      if (pt_check_and_coerce_to_time (parser, arg1) ==
			  NO_ERROR)
			{
			  arg1_type = PT_TYPE_TIME;
			}
		      else
			{
			  parser_free_tree (parser, parser->error_msgs);
			  parser->error_msgs = NULL;

			  /* try coercing to utime */
			  if (pt_coerce_value (parser, arg1, arg1,
					       PT_TYPE_TIMESTAMP,
					       NULL) == NO_ERROR)
			    {
			      arg1_type = PT_TYPE_TIMESTAMP;
			    }
			  else
			    {
			      parser_free_tree (parser, parser->error_msgs);
			      parser->error_msgs = NULL;

			      /* try coercing to datetime */
			      if (pt_coerce_value (parser, arg1, arg1,
						   PT_TYPE_DATETIME,
						   NULL) == NO_ERROR)
				{
				  arg1_type = PT_TYPE_DATETIME;
				}
			    }
			}
		    }

		  if (arg1_type == PT_TYPE_TIME
		      || arg1_type == PT_TYPE_TIMESTAMP
		      || arg1_type == PT_TYPE_DATETIME)
		    {
		      node->type_enum = PT_TYPE_INTEGER;
		    }
		  else if (arg1_type == PT_TYPE_DATE)
		    {
		      incompatible_extract_type = true;
		    }
		  break;

		case PT_MILLISECOND:
		  if (PT_IS_CHAR_STRING_TYPE (arg1_type))
		    {
		      arg1_type = PT_TYPE_NONE;
		      /* try coercing to datetime */
		      if (pt_coerce_value (parser, arg1, arg1,
					   PT_TYPE_DATETIME,
					   NULL) == NO_ERROR)
			{
			  arg1_type = PT_TYPE_DATETIME;
			}
		    }

		  if (arg1_type == PT_TYPE_DATETIME)
		    {
		      node->type_enum = PT_TYPE_INTEGER;
		    }
		  else if (arg1_type == PT_TYPE_DATE
			   || arg1_type == PT_TYPE_TIME
			   || arg1_type == PT_TYPE_TIMESTAMP)
		    {
		      incompatible_extract_type = true;
		    }
		  break;
		default:
		  break;
		}

	      if (incompatible_extract_type)
		{
		  PT_ERRORmf2 (parser, node, MSGCAT_SET_PARSER_SEMANTIC,
			       MSGCAT_SEMANTIC_CANT_EXTRACT_FROM,
			       pt_show_misc_type (node->info.expr.qualifier),
			       pt_show_type_enum (arg1->type_enum));
		  return node;
		}

	      if (node->type_enum != PT_TYPE_NONE
		  && (node->data_type = parser_new_node (parser,
							 PT_DATA_TYPE)) !=
		  NULL)
		{
		  node->data_type->type_enum = node->type_enum;
		}
	    }
	  else
	    {
	      /* argument is not date, string, MAYBE or NULL */
	      node->type_enum = PT_TYPE_NONE;
	    }
	  break;
	case PT_COALESCE:
	  if (common_type != PT_TYPE_NONE
	      && arg1_type != PT_TYPE_NA && arg1_type != PT_TYPE_NULL
	      && arg2_type != PT_TYPE_NA && arg2_type != PT_TYPE_NULL
	      && PT_IS_COLLECTION_TYPE (common_type))
	    {
	      pt_propagate_types (parser, node, arg1->data_type,
				  arg2->data_type);
	    }
	  break;
	case PT_TIMEDIFF:
	  if (PT_IS_DATE_TIME_TYPE (arg1_type)
	      && PT_IS_DATE_TIME_TYPE (arg2_type))
	    {
	      if (arg1_type == PT_TYPE_TIME || arg1_type == PT_TYPE_DATE)
		{
		  if (arg2_type != arg1_type)
		    {
		      node->type_enum = PT_TYPE_NONE;
		    }
		}
	      else
		{
		  /* arg1_type is PT_TYPE_DATETIME or PT_TYPE_TIMESTAMP. */
		  if (arg2_type == PT_TYPE_TIME || arg2_type == PT_TYPE_DATE)
		    {
		      node->type_enum = PT_TYPE_NONE;
		    }
		}
	    }
	  break;
	default:
	  break;
	}
      goto error;
    }

  if (pt_is_symmetric_op (op))
    {
      /*
       * At most one of these next two cases will hold... these will
       * make a dynamic host var (one about whose type we know nothing
       * at this point) assume the type of its "mate" in a symmetric
       * dyadic operator.
       */
      if (arg1_hv && arg2_type != PT_TYPE_NONE && arg2_type != PT_TYPE_MAYBE)
	{
	  if (arg1_hv != arg1)
	    {
	      /* special case of the unary minus on host var
	         no error will be returned in this case */
	      (void) pt_coerce_value (parser, arg1_hv, arg1_hv, arg2_type,
				      arg2->data_type);
	      arg1_type = arg1->type_enum = arg1_hv->type_enum;
	      d = pt_xasl_type_enum_to_domain (arg1_type);
	      SET_EXPECTED_DOMAIN (arg1, d);
	      SET_EXPECTED_DOMAIN (arg1_hv, d);
	      pt_preset_hostvar (parser, arg1_hv);
	    }
	  else
	    {
	      /* no error will be returned in this case */
	      (void) pt_coerce_value (parser, arg1, arg1, arg2_type,
				      arg2->data_type);
	      arg1_type = arg1->type_enum;
	      d = pt_xasl_type_enum_to_domain (arg1_type);
	      SET_EXPECTED_DOMAIN (arg1, d);
	      pt_preset_hostvar (parser, arg1);
	    }
	}

      if (arg2_hv && arg1_type != PT_TYPE_NONE && arg1_type != PT_TYPE_MAYBE)
	{
	  if (arg2_hv != arg2)
	    {
	      /* special case of the unary minus on host var
	         no error will be returned in this case */
	      (void) pt_coerce_value (parser, arg2_hv, arg2_hv, arg1_type,
				      arg1->data_type);
	      arg2_type = arg2->type_enum = arg2_hv->type_enum;
	      d = pt_xasl_type_enum_to_domain (arg2_type);
	      SET_EXPECTED_DOMAIN (arg2, d);
	      SET_EXPECTED_DOMAIN (arg2_hv, d);
	      pt_preset_hostvar (parser, arg2_hv);
	    }
	  else
	    {
	      /* no error will be returned in this case */
	      (void) pt_coerce_value (parser, arg2, arg2, arg1_type,
				      arg1->data_type);
	      arg2_type = arg2->type_enum;
	      d = pt_xasl_type_enum_to_domain (arg2_type);
	      SET_EXPECTED_DOMAIN (arg2, d);
	      pt_preset_hostvar (parser, arg2);
	    }
	}

      if (arg2)
	{
	  if (pt_is_enumeration_special_comparison (arg1, op, arg2))
	    {
	      /* In case of 'ENUM = const' or 'ENUM IN ...' we need to convert
	         the right argument to the ENUM type in order to preserve an
	         eventual index scan on left argument */
	      common_type = PT_TYPE_ENUMERATION;
	    }
	  else
	    {
	      common_type = pt_common_type_op (arg1_type, op, arg2_type);
	    }
	}

      if (pt_is_symmetric_type (common_type))
	{
	  PT_NODE *data_type;

	  if (arg1_type != common_type)
	    {
	      /*
	       * pt_coerce_value may fail here, but it shouldn't be
	       * considered a real failure yet, because it could still
	       * be rescued by the gruesome date/time stuff below.
	       * DON'T set common_type here, or you'll surely screw up
	       * the next case when you least expect it.
	       */
	      data_type =
		(PT_IS_NUMERIC_TYPE (common_type)
		 || PT_IS_STRING_TYPE (common_type))
		? NULL
		: (PT_IS_COLLECTION_TYPE (common_type))
		? arg1->data_type : arg2->data_type;

	      pt_coerce_value (parser, arg1, arg1, common_type, data_type);
	      arg1_type = arg1->type_enum;
	    }

	  if (arg2 && arg2_type != common_type)
	    {
	      /* Same warning as above... */
	      data_type =
		(PT_IS_NUMERIC_TYPE (common_type)
		 || PT_IS_STRING_TYPE (common_type))
		? NULL
		: (PT_IS_COLLECTION_TYPE (common_type))
		? arg2->data_type : arg1->data_type;

	      pt_coerce_value (parser, arg2, arg2, common_type, data_type);
	      arg2_type = arg2->type_enum;
	    }
	}
    }
  else
    {
      if (!PT_DOES_FUNCTION_HAVE_DIFFERENT_ARGS (op))
	{
	  if (arg2 && arg2->type_enum == PT_TYPE_MAYBE)
	    {
	      if (PT_IS_NUMERIC_TYPE (arg1_type)
		  || PT_IS_STRING_TYPE (arg1_type))
		{
		  d = pt_node_to_db_domain (parser, arg1, NULL);
		  d = tp_domain_cache (d);
		  SET_EXPECTED_DOMAIN (arg2, d);
		  if (arg2->node_type == PT_HOST_VAR)
		    {
		      pt_preset_hostvar (parser, arg2);
		    }
		}
	    }

	  if (PT_IS_FUNCTION (arg2)
	      && PT_IS_COLLECTION_TYPE (arg2->type_enum))
	    {
	      /* a IN (?, ...) */
	      PT_NODE *temp;

	      for (temp = arg2->info.function.arg_list; temp;
		   temp = temp->next)
		{
		  if (temp->node_type == PT_HOST_VAR
		      && temp->type_enum == PT_TYPE_MAYBE)
		    {
		      if (arg1_type != PT_TYPE_NONE
			  && arg1_type != PT_TYPE_MAYBE)
			{
			  (void) pt_coerce_value (parser, temp, temp,
						  arg1_type, arg1->data_type);
			  d = pt_xasl_type_enum_to_domain (temp->type_enum);
			  SET_EXPECTED_DOMAIN (temp, d);
			  if (temp->node_type == PT_HOST_VAR)
			    {
			      pt_preset_hostvar (parser, temp);
			    }
			}
		    }
		}
	    }

	  if (arg3 && arg3->type_enum == PT_TYPE_MAYBE)
	    {
	      if (PT_IS_NUMERIC_TYPE (arg1_type)
		  || PT_IS_STRING_TYPE (arg1_type))
		{
		  d = pt_node_to_db_domain (parser, arg1, NULL);
		  d = tp_domain_cache (d);
		  SET_EXPECTED_DOMAIN (arg3, d);
		  if (arg3->node_type == PT_HOST_VAR)
		    {
		      pt_preset_hostvar (parser, arg3);
		    }
		}
	    }
	}
    }

  if ((common_type == PT_TYPE_NA || common_type == PT_TYPE_NULL)
      && node->or_next)
    {
      common_type = node->or_next->type_enum;
    }

  node->type_enum = common_type;

  if (node->type_enum == PT_TYPE_MAYBE
      && pt_is_able_to_determine_return_type (op))
    {
      /* Because we can determine the return type of the expression
       * regardless of its argument, go further to determine it.
       *
       * temporary reset to NONE.
       */
      node->type_enum = PT_TYPE_NONE;
    }

  if (node->type_enum == PT_TYPE_MAYBE)
    {
      /* There can be a unbinded host variable at compile time */
      if (PT_DOES_FUNCTION_HAVE_DIFFERENT_ARGS (op))
	{
	  /* don't touch the args. leave it as it is */
	  return node;
	}

      if (arg1_type == PT_TYPE_MAYBE)
	{
	  if (node->expected_domain
	      && (TP_IS_NUMERIC_TYPE (TP_DOMAIN_TYPE (node->expected_domain))
		  ||
		  TP_IS_STRING_TYPE (TP_DOMAIN_TYPE (node->expected_domain))))
	    {
	      SET_EXPECTED_DOMAIN (arg1, node->expected_domain);
	      if (arg1->node_type == PT_HOST_VAR)
		{
		  pt_preset_hostvar (parser, arg1);
		}
	    }
	  else if (arg2 && (PT_IS_NUMERIC_TYPE (arg2_type)
			    || PT_IS_STRING_TYPE (arg2_type)))
	    {
	      if (arg1->node_type == PT_NAME)
		{		/* subquery derived table */
		  PT_NODE *new_att;
		  int p, s;

		  if (arg2->data_type)
		    {
		      p = arg2->data_type->info.data_type.precision;
		      s = arg2->data_type->info.data_type.dec_precision;
		    }
		  else
		    {
		      p = TP_FLOATING_PRECISION_VALUE;
		      s = TP_FLOATING_PRECISION_VALUE;
		    }

		  new_att = pt_wrap_with_cast_op (parser, arg1, arg2_type,
						  p, s, arg2->data_type);
		  if (new_att == NULL)
		    {
		      node->type_enum = PT_TYPE_NONE;
		      goto error;
		    }
		  node->info.expr.arg1 = arg1 = new_att;
		  arg1_type = arg2_type;
		}
	      else
		{		/* has a hostvar */
		  d = pt_node_to_db_domain (parser, arg2, NULL);
		  d = tp_domain_cache (d);
		  SET_EXPECTED_DOMAIN (arg1, d);
		  if (arg1->node_type == PT_HOST_VAR)
		    {
		      pt_preset_hostvar (parser, arg1);
		    }
		}
	    }
	}

      if (arg2_type == PT_TYPE_MAYBE)
	{
	  if (node->expected_domain
	      && (TP_IS_NUMERIC_TYPE (TP_DOMAIN_TYPE (node->expected_domain))
		  ||
		  TP_IS_STRING_TYPE (TP_DOMAIN_TYPE (node->expected_domain))))
	    {
	      SET_EXPECTED_DOMAIN (arg2, node->expected_domain);
	      if (arg2->node_type == PT_HOST_VAR)
		{
		  pt_preset_hostvar (parser, arg2);
		}
	    }
	  else if (arg1 && (PT_IS_NUMERIC_TYPE (arg1_type)
			    || PT_IS_STRING_TYPE (arg1_type)))
	    {
	      if (arg2->node_type == PT_NAME)
		{		/* subquery derived table */
		  PT_NODE *new_att;
		  int p, s;

		  if (arg1->data_type)
		    {
		      p = arg1->data_type->info.data_type.precision;
		      s = arg1->data_type->info.data_type.dec_precision;
		    }
		  else
		    {
		      p = TP_FLOATING_PRECISION_VALUE;
		      s = TP_FLOATING_PRECISION_VALUE;
		    }

		  new_att = pt_wrap_with_cast_op (parser, arg2, arg1_type,
						  p, s, arg1->data_type);
		  if (new_att == NULL)
		    {
		      node->type_enum = PT_TYPE_NONE;
		      goto error;
		    }
		  node->info.expr.arg2 = arg2 = new_att;
		  arg2_type = arg1_type;
		}
	      else
		{		/* has a hostvar */
		  d = pt_node_to_db_domain (parser, arg1, NULL);
		  d = tp_domain_cache (d);
		  SET_EXPECTED_DOMAIN (arg2, d);
		  if (arg2->node_type == PT_HOST_VAR)
		    {
		      pt_preset_hostvar (parser, arg2);
		    }
		}
	    }
	}

      return node;
    }				/* if node->type_enum == PT_TYPE_MAYBE */

  switch (op)
    {
    case PT_IF:
      {
	DB_TYPE dbtype2 = DB_TYPE_UNKNOWN, dbtype3 = DB_TYPE_UNKNOWN;
	if (arg1_type == PT_TYPE_MAYBE)
	  {
	    d = tp_domain_resolve_default (DB_TYPE_STRING);
	    SET_EXPECTED_DOMAIN (arg1, d);
	    pt_preset_hostvar (parser, arg1);
	  }
	else if (arg1_type != PT_TYPE_LOGICAL && arg2_type != PT_TYPE_NULL)
	  {
	    node->type_enum = PT_TYPE_NONE;
	    break;
	  }
	if (arg2_type == PT_TYPE_MAYBE || arg3_type == PT_TYPE_MAYBE)
	  {
	    if (arg2_type == PT_TYPE_MAYBE && arg3_type == PT_TYPE_MAYBE)
	      {
		if (node->expected_domain != NULL)
		  {
		    /* the expected domain might have been set by a different
		       pass through the tree */
		    common_type =
		      pt_db_to_type_enum (TP_DOMAIN_TYPE
					  (node->expected_domain));
		  }
		else
		  {
		    common_type = PT_TYPE_VARCHAR;
		  }
	      }
	    else
	      {
		common_type =
		  (arg2_type == PT_TYPE_MAYBE) ? arg3_type : arg2_type;
	      }
	  }
	else
	  {
	    /* We have to decide a common type for arg2 and arg3.
	       We cannot use pt_common_type_op because this function is designed
	       mostly for arithmetic expression. This is why we use the
	       tp_is_more_general_type here.
	     */
	    dbtype2 = pt_type_enum_to_db (arg2_type);
	    dbtype3 = pt_type_enum_to_db (arg3_type);
	    if (tp_more_general_type (dbtype2, dbtype3) > 0)
	      {
		common_type = arg2_type;
	      }
	    else
	      {
		common_type = arg3_type;
	      }
	  }
	if (common_type == PT_TYPE_LOGICAL)
	  {
	    /* we will end up with logical here if arg3 and arg2 are
	       logical */
	    common_type = PT_TYPE_INTEGER;
	  }
	if (pt_coerce_expression_argument (parser, node, &arg2, common_type,
					   NULL) != NO_ERROR)
	  {
	    node->type_enum = PT_TYPE_NONE;
	    break;
	  }
	node->info.expr.arg2 = arg2;

	if (pt_coerce_expression_argument (parser, node, &arg3, common_type,
					   NULL) != NO_ERROR)
	  {
	    node->type_enum = PT_TYPE_NONE;
	    break;
	  }
	node->info.expr.arg3 = arg3;
	node->type_enum = common_type;

	if (PT_HAS_COLLATION (common_type))
	  {
	    check_expr_coll = true;
	  }
	break;
      }

    case PT_FIELD:
      if ((arg1 && arg1_type == PT_TYPE_LOGICAL) ||
	  (arg2 && arg2_type == PT_TYPE_LOGICAL) ||
	  pt_list_has_logical_nodes (arg3))
	{
	  node->type_enum = PT_TYPE_NONE;
	  break;
	}

      check_expr_coll = true;

      node->type_enum = common_type = PT_TYPE_INTEGER;

      if (arg3_type != PT_TYPE_NULL && arg3_type != PT_TYPE_NA
	  && arg3_type != PT_TYPE_MAYBE)
	{
	  first_node =
	    (arg3->next && arg3->next->info.value.data_value.i == 1);

	  if (!((first_node ? PT_IS_STRING_TYPE (arg1_type) : true)
		&& (arg2_type != PT_TYPE_NULL && arg2_type != PT_TYPE_NA
		    ? PT_IS_STRING_TYPE (arg2_type) : true)
		&& (PT_IS_STRING_TYPE (arg3_type)
		    || arg3_type == PT_TYPE_ENUMERATION))
	      && !((first_node ? PT_IS_NUMERIC_TYPE (arg1_type) : true)
		   && (arg2_type != PT_TYPE_NULL
		       && arg2_type !=
		       PT_TYPE_NA ? PT_IS_NUMERIC_TYPE (arg2_type) : true)
		   && (PT_IS_NUMERIC_TYPE (arg3_type)
		       || arg3_type == PT_TYPE_ENUMERATION)))
	    {
	      /* cast to type of first parameter */

	      new_type = arg3_type == PT_TYPE_CHAR ? PT_TYPE_VARCHAR :
		(arg3_type == PT_TYPE_NCHAR ? PT_TYPE_VARNCHAR : arg3_type);

	      if (first_node)
		{
		  new_att = pt_wrap_with_cast_op (parser, arg1, new_type,
						  TP_FLOATING_PRECISION_VALUE,
						  0, NULL);
		  if (new_att == NULL)
		    {
		      node->type_enum = PT_TYPE_NONE;
		      goto error;
		    }
		  PT_EXPR_INFO_SET_FLAG (new_att, PT_EXPR_INFO_CAST_NOFAIL);
		  node->info.expr.arg1 = arg1 = new_att;
		  arg1_type = new_type;
		}

	      new_att =
		pt_wrap_with_cast_op (parser, arg2, new_type,
				      TP_FLOATING_PRECISION_VALUE, 0, NULL);
	      if (new_att == NULL)
		{
		  node->type_enum = PT_TYPE_NONE;
		  goto error;
		}
	      PT_EXPR_INFO_SET_FLAG (new_att, PT_EXPR_INFO_CAST_NOFAIL);
	      node->info.expr.arg2 = arg2 = new_att;
	      arg2_type = new_type;
	    }
	}
      break;

    case PT_CONNECT_BY_ROOT:
      node->type_enum = node->info.expr.arg1->type_enum;
      break;

    case PT_ASSIGN:
      node->type_enum = arg1_type;
      node->data_type = parser_copy_tree_list (parser, arg1->data_type);

      if (PT_IS_N_COLUMN_UPDATE_EXPR (arg1))
	{
	  if (PT_IS_QUERY_NODE_TYPE (arg2->node_type))
	    {
	      PT_NODE *att_a, *att_b;

	      att_a = arg1->info.expr.arg1;
	      att_b = pt_get_select_list (parser, arg2);
	      if (pt_length_of_list (att_a) ==
		  pt_length_of_select_list (att_b, EXCLUDE_HIDDEN_COLUMNS))
		{
		  for (; att_a && att_b;
		       att_a = att_a->next, att_b = att_b->next)
		    {
		      if (att_b->type_enum != att_a->type_enum
			  && pt_coerce_value (parser, att_b, att_b,
					      att_a->type_enum,
					      att_a->data_type) != NO_ERROR)
			{
			  node->type_enum = PT_TYPE_NONE;
			  break;
			}
		    }
		}
	      else
		{
		  node->type_enum = PT_TYPE_NONE;
		}
	    }
	  else
	    {
	      node->type_enum = PT_TYPE_NONE;
	    }
	}
      else
	{
	  /* arg2 can be NULL here, if CAST is not compatible */
	  if (arg2 == NULL ||
	      (arg2_type != arg1_type
	       && pt_coerce_value (parser, arg2, arg2,
				   arg1_type, arg1->data_type) != NO_ERROR))
	    {
	      node->type_enum = PT_TYPE_NONE;
	    }
	}
      break;

    case PT_LIKE_ESCAPE:
      /* PT_LIKE_ESCAPE is handled by PT_LIKE */
      break;

    case PT_EXISTS:
      if (common_type != PT_TYPE_NONE
	  && (pt_is_query (arg1) || PT_IS_COLLECTION_TYPE (arg1_type)))
	{
	  node->type_enum = PT_TYPE_LOGICAL;
	  if (pt_is_query (arg1))
	    {
	      pt_chop_to_one_select_item (parser, arg1);
	    }
	}
      else if (arg1_type == PT_TYPE_NA || arg1_type == PT_TYPE_NULL)
	{
	  node->type_enum = PT_TYPE_NULL;
	}
      else
	{
	  node->type_enum = PT_TYPE_NONE;
	}
      break;

    case PT_RANGE:
      node->type_enum = PT_TYPE_NONE;
      if (arg2 == NULL)
	{
	  goto error;
	}
      node->type_enum = PT_TYPE_LOGICAL;
      break;

    case PT_OID_OF_DUPLICATE_KEY:
      /* The argument should already have the type of the spec's OID; see
         pt_dup_key_update_stmt () */
      node->data_type =
	parser_copy_tree (parser, node->info.expr.arg1->data_type);
      node->type_enum = PT_TYPE_OBJECT;
      break;

    case PT_STR_TO_DATE:
      {
	int type = 0;

	assert (arg3_type == PT_TYPE_INTEGER);

	if (arg2_type == PT_TYPE_NULL)
	  {
	    node->type_enum = PT_TYPE_NULL;
	    break;
	  }
	if (!PT_IS_VALUE_NODE (arg2) && !PT_IS_INPUT_HOSTVAR (arg2))
	  {
	    node->type_enum = PT_TYPE_NONE;
	    break;
	  }
	if (!PT_IS_CHAR_STRING_TYPE (arg1_type))
	  {
	    if (arg1_type == PT_TYPE_MAYBE)
	      {
		d = pt_xasl_type_enum_to_domain (PT_TYPE_VARCHAR);
		SET_EXPECTED_DOMAIN (arg1, d);
		pt_preset_hostvar (parser, arg1);
	      }
	    else
	      {
		new_att =
		  pt_wrap_with_cast_op (parser, arg1, PT_TYPE_VARCHAR,
					TP_FLOATING_PRECISION_VALUE, 0, NULL);
		if (new_att == NULL)
		  {
		    node->type_enum = PT_TYPE_NONE;
		    goto error;
		  }
		if (prm_get_bool_value
		    (PRM_ID_RETURN_NULL_ON_FUNCTION_ERRORS))
		  {
		    PT_EXPR_INFO_SET_FLAG (new_att, PT_EXPR_INFO_CAST_NOFAIL);
		  }
		node->info.expr.arg1 = arg1 = new_att;
	      }
	  }

	if (arg2_hv && arg2_type == PT_TYPE_MAYBE)
	  {
	    d = tp_domain_resolve_default (DB_TYPE_VARCHAR);
	    SET_EXPECTED_DOMAIN (arg2, d);
	    pt_preset_hostvar (parser, arg2);
	  }

	if (arg2->node_type == PT_VALUE)
	  {
	    type = db_check_time_date_format (arg2->info.value.data_value.
					      str->bytes);
	  }
	else
	  {
	    type = 3;
	  }

	/* default is date (i.e.: -> when no format supplied) */
	node->type_enum = PT_TYPE_DATE;
	if (type == 1)
	  {
	    node->type_enum = PT_TYPE_TIME;
	  }
	else if (type == 2)
	  {
	    node->type_enum = PT_TYPE_DATE;
	  }
	else if (type == 3)
	  {
	    node->type_enum = PT_TYPE_DATETIME;
	  }
	else if (arg1_type == PT_TYPE_NULL || arg2_type == PT_TYPE_NULL)
	  {
	    /* if other value, the db_str_to_date will return NULL */
	    node->type_enum = PT_TYPE_NULL;
	  }
	break;
      }

    case PT_DATE_ADD:
    case PT_DATE_SUB:
      {
	if (arg1_hv && arg1_type == PT_TYPE_MAYBE)
	  {
	    /* Though arg1 can be date/timestamp/datetime/string,
	     * assume it is a string which is the most general.
	     */
	    d = tp_domain_resolve_default (DB_TYPE_STRING);
	    SET_EXPECTED_DOMAIN (arg1, d);
	    pt_preset_hostvar (parser, arg1);
	    common_type = node->type_enum = PT_TYPE_VARCHAR;
	  }
	if (arg2_hv && arg2_type == PT_TYPE_MAYBE)
	  {
	    d = tp_domain_resolve_default (DB_TYPE_STRING);
	    SET_EXPECTED_DOMAIN (arg2, d);
	    pt_preset_hostvar (parser, arg2);
	  }

	/* arg1 -> date or string, arg2 -> integer or string acc to unit */
	if ((PT_HAS_DATE_PART (arg1_type)
	     || PT_IS_CHAR_STRING_TYPE (arg1_type)
	     || arg1_type == PT_TYPE_MAYBE)
	    && (PT_IS_CHAR_STRING_TYPE (arg2_type)
		|| PT_IS_NUMERIC_TYPE (arg2_type)
		|| arg2_type == PT_TYPE_MAYBE))
	  {
	    /* if arg2 is integer, unit must be one of
	     * MILLISECOND, SECOND, MINUTE, HOUR, DAY, WEEK, MONTH, QUARTER,
	     * YEAR.
	     */
	    int is_single_unit;

	    is_single_unit = (arg3
			      && (arg3->info.expr.qualifier == PT_MILLISECOND
				  || arg3->info.expr.qualifier == PT_SECOND
				  || arg3->info.expr.qualifier == PT_MINUTE
				  || arg3->info.expr.qualifier == PT_HOUR
				  || arg3->info.expr.qualifier == PT_DAY
				  || arg3->info.expr.qualifier == PT_WEEK
				  || arg3->info.expr.qualifier == PT_MONTH
				  || arg3->info.expr.qualifier == PT_QUARTER
				  || arg3->info.expr.qualifier == PT_YEAR));

	    if (arg1_type == PT_TYPE_DATETIME
		|| arg1_type == PT_TYPE_TIMESTAMP)
	      {
		node->type_enum = PT_TYPE_DATETIME;
	      }
	    else if (arg1_type == PT_TYPE_DATE)
	      {
		if (arg3->info.expr.qualifier == PT_DAY
		    || arg3->info.expr.qualifier == PT_WEEK
		    || arg3->info.expr.qualifier == PT_MONTH
		    || arg3->info.expr.qualifier == PT_QUARTER
		    || arg3->info.expr.qualifier == PT_YEAR
		    || arg3->info.expr.qualifier == PT_YEAR_MONTH)
		  {
		    node->type_enum = PT_TYPE_DATE;
		  }
		else
		  {
		    node->type_enum = PT_TYPE_DATETIME;
		  }
	      }
	    else
	      {
		node->type_enum = PT_TYPE_VARCHAR;
		common_type = node->type_enum;
	      }
	  }
	else if (arg1_type == PT_TYPE_NULL || arg2_type == PT_TYPE_NULL)
	  {
	    node->type_enum = PT_TYPE_NULL;
	  }
	else
	  {
	    /* if we got here, we have a type incompatibility; however, if one
	       of the arguments is of type MAYBE, the error will not be caught.
	       NOTE: see label 'error' at end of function */
	    if (arg1 && arg1->type_enum == PT_TYPE_MAYBE)
	      {
		arg1 = NULL;
	      }

	    if (arg2 && arg2->type_enum == PT_TYPE_MAYBE)
	      {
		arg2 = NULL;
	      }

	    if (arg3 && arg3->type_enum == PT_TYPE_MAYBE)
	      {
		arg3 = NULL;
	      }

	    /* set type to NONE so error message is shown */
	    node->type_enum = PT_TYPE_NONE;
	  }
	break;
      }
    case PT_CAST:
      cast_type = node->info.expr.cast_type;

      if (PT_EXPR_INFO_IS_FLAGED (node, PT_EXPR_INFO_CAST_COLL_MODIFIER)
	  && cast_type == NULL)
	{
	  /* cast_type should be the same as arg1 type */
	  cast_type = parser_copy_tree (parser, arg1->data_type);

	  /* for HV argument, attempt to resolve using the expected domain
	   * of expression's node or arg1 node */
	  if (cast_type == NULL && node->expected_domain != NULL &&
	      TP_TYPE_HAS_COLLATION (TP_DOMAIN_TYPE (node->expected_domain)))
	    {
	      cast_type =
		pt_domain_to_data_type (parser, node->expected_domain);
	    }

	  if (cast_type == NULL && arg1->expected_domain != NULL &&
	      TP_TYPE_HAS_COLLATION (TP_DOMAIN_TYPE (arg1->expected_domain)))
	    {
	      /* create data type from expected domain */
	      cast_type =
		pt_domain_to_data_type (parser, arg1->expected_domain);
	    }

	  if (cast_type != NULL)
	    {
	      node->info.expr.cast_type = cast_type;
	      cast_type->info.data_type.collation_id =
		PT_GET_COLLATION_MODIFIER (node);
	    }
	}

      if (cast_type && pt_check_cast_op (parser, node))
	{
	  node->type_enum = cast_type->type_enum;
	  if (pt_is_set_type (cast_type))
	    {
	      /* use canonical set data type */
	      node->data_type = parser_copy_tree_list (parser,
						       cast_type->data_type);
	    }
	  else if (PT_IS_COMPLEX_TYPE (cast_type->type_enum))
	    {
	      node->data_type = parser_copy_tree_list (parser, cast_type);
	    }

	  /* TODO : this requires a generic fix: maybe 'arg1_hv' should never be set
	   * to arg1->info.expr.arg1;
	   * arg1 may not be necessarily a HV node, but arg1_hv may be a direct
	   * link to argument of arg1 (in case arg1 is an expression with
	   * unary operator)- see code at beginning of function when arguments
	   * are checked;
	   * for now, this fix is enough for PT_CAST ( PT_UNARY_MINUS (.. )*/
	  if (arg1_hv && arg1_type == PT_TYPE_MAYBE
	      && arg1->node_type == PT_HOST_VAR)
	    {
	      d = pt_node_to_db_domain (parser, node, NULL);
	      SET_EXPECTED_DOMAIN (arg1, d);
	      pt_preset_hostvar (parser, arg1);
	    }
	}
      else
	{
	  node->type_enum = PT_TYPE_NONE;
	}
      break;

    case PT_DECODE:
    case PT_CASE:
      if (arg3_hv
	  && pt_coerce_value (parser, arg3, arg3, PT_TYPE_LOGICAL,
			      NULL) != NO_ERROR)
	{
	  node->type_enum = PT_TYPE_NONE;
	  break;
	}

      if (arg3_type != PT_TYPE_NA && arg3_type != PT_TYPE_NULL
	  && arg3_type != PT_TYPE_LOGICAL && arg3_type != PT_TYPE_MAYBE)
	{
	  PT_ERRORm (parser, arg3, MSGCAT_SET_PARSER_SEMANTIC,
		     MSGCAT_SEMANTIC_WANT_LOGICAL_CASE_COND);
	  return node;
	}

      arg3 = NULL;
      common_type = node->info.expr.recursive_type;
      if (common_type == PT_TYPE_NONE)
	{
	  common_type = pt_common_type_op (arg1_type, op, arg2_type);
	}
      if (common_type == PT_TYPE_NONE)
	{
	  node->type_enum = PT_TYPE_NONE;
	  break;
	}
      if (common_type == PT_TYPE_MAYBE)
	{
	  common_type = (arg1_type == PT_TYPE_MAYBE &&
			 arg2_type != PT_TYPE_NULL) ? arg2_type : arg1_type;
	}

      if (common_type == PT_TYPE_MAYBE)
	{
	  /* both args are MAYBE, default to VARCHAR */
	  common_type = PT_TYPE_VARCHAR;
	}

      if (PT_IS_COLLECTION_TYPE (common_type))
	{
	  /* we're not casting collections */
	  pt_propagate_types (parser, node, arg1->data_type, arg2->data_type);
	  node->type_enum = common_type;
	  break;
	}
      node->type_enum = common_type;
      if (arg1_type != common_type && arg1_type != PT_TYPE_NULL)
	{
	  /* cast arg1_type to common type */
	  if (pt_coerce_expression_argument (parser, node, &arg1, common_type,
					     NULL) != NO_ERROR)
	    {
	      node->type_enum = PT_TYPE_NONE;
	      goto error;
	    }
	  node->info.expr.arg1 = arg1;
	}

      if (arg2_type != common_type)
	{
	  /* arg2 is a case list, we need to walk it and cast all arguments */
	  PT_NODE *nextcase = arg2;
	  PT_NODE *prev = node;
	  while (nextcase)
	    {
	      if (nextcase->type_enum == common_type
		  || nextcase->type_enum == PT_TYPE_NULL)
		{
		  break;
		}

	      if (nextcase->node_type == PT_EXPR
		  && (nextcase->info.expr.op == PT_CASE
		      || nextcase->info.expr.op == PT_DECODE))
		{
		  /* cast nextcase->arg1 to common type */
		  arg1 = nextcase->info.expr.arg1;
		  arg1_type = arg1->type_enum;
		  if (arg1_type != common_type && arg1_type != PT_TYPE_NULL)
		    {
		      if (pt_coerce_expression_argument (parser, nextcase,
							 &arg1, common_type,
							 NULL) != NO_ERROR)
			{
			  /* abandon implicit casting and return error */
			  node->type_enum = PT_TYPE_NONE;
			  goto error;
			}
		      nextcase->info.expr.arg1 = arg1;
		      /* nextcase was already evaluated and may have a
		         data_type set. We need to replace it with the cast
		         data_type */
		      nextcase->type_enum = common_type;
		      if (nextcase->data_type)
			{
			  parser_free_tree (parser, nextcase->data_type);
			  nextcase->data_type
			    = parser_copy_tree_list (parser, arg1->data_type);
			}
		    }
		  /* set nextcase to nextcase->arg2 and continue */
		  prev = nextcase;
		  nextcase = nextcase->info.expr.arg2;
		  continue;
		}
	      else
		{
		  /* cast nextcase to common type */
		  if (pt_coerce_expression_argument (parser, prev, &nextcase,
						     common_type, NULL)
		      != NO_ERROR)
		    {
		      /* abandon implicit casting and return error */
		      node->type_enum = PT_TYPE_NONE;
		      goto error;
		    }
		  prev->info.expr.arg2 = nextcase;
		  nextcase = NULL;
		}
	    }
	}
      break;

    case PT_PATH_EXPR_SET:
      /* temporary setting with the first element */
      node->type_enum = node->info.expr.arg1->type_enum;
      break;

    default:
      node->type_enum = PT_TYPE_NONE;
      break;
    }

  if (PT_IS_PARAMETERIZED_TYPE (common_type)
      && pt_upd_domain_info (parser, (op == PT_IF) ? arg3 : arg1, arg2, op,
			     common_type, node) != NO_ERROR)
    {
      node->type_enum = PT_TYPE_NONE;
    }

  if (check_expr_coll && pt_check_expr_collation (parser, &node) != NO_ERROR)
    {
      node->type_enum = PT_TYPE_NONE;
      return node;
    }

cannot_use_signature:
  if (expr != NULL)
    {
      expr = pt_wrap_expr_w_exp_dom_cast (parser, expr);
      node = expr;
      expr = NULL;
    }

error:
  if (node->type_enum == PT_TYPE_NONE)
    {
      if ((arg1 && arg1->type_enum == PT_TYPE_MAYBE)
	  || (arg2 && arg2->type_enum == PT_TYPE_MAYBE)
	  || (arg3 && arg3->type_enum == PT_TYPE_MAYBE))
	{
	  node->type_enum = PT_TYPE_MAYBE;
	  return node;
	}

      if (!pt_has_error (parser))
	{
	  if (arg2 && arg3)
	    {
	      assert (arg1 != NULL);
	      PT_ERRORmf4 (parser, node, MSGCAT_SET_PARSER_SEMANTIC,
			   MSGCAT_SEMANTIC_OP_NOT_DEFINED_ON_3,
			   pt_show_binopcode (op),
			   pt_show_type_enum (arg1->type_enum),
			   pt_show_type_enum (arg2->type_enum),
			   pt_show_type_enum (arg3->type_enum));
	    }
	  else if (arg2)
	    {
	      assert (arg1 != NULL);
	      PT_ERRORmf3 (parser, node, MSGCAT_SET_PARSER_SEMANTIC,
			   MSGCAT_SEMANTIC_OP_NOT_DEFINED_ON,
			   pt_show_binopcode (op),
			   pt_show_type_enum (arg1->type_enum),
			   pt_show_type_enum (arg2->type_enum));
	    }
	  else if (arg1)
	    {
	      PT_ERRORmf2 (parser, node, MSGCAT_SET_PARSER_SEMANTIC,
			   MSGCAT_SEMANTIC_OP_NOT_DEFINED_ON_1,
			   pt_show_binopcode (op),
			   pt_show_type_enum (arg1->type_enum));
	    }
	  else
	    {
	      PT_ERRORmf2 (parser, node, MSGCAT_SET_PARSER_SEMANTIC,
			   MSGCAT_SEMANTIC_OP_NOT_DEFINED_ON_1,
			   pt_show_binopcode (op),
			   pt_show_type_enum (PT_TYPE_NONE));
	    }
	}
    }

  return node;
}

/*
 * pt_eval_opt_type () -
 *   return:
 *   parser(in):
 *   node(in/out):
 */
static PT_NODE *
pt_eval_opt_type (PARSER_CONTEXT * parser, PT_NODE * node)
{
  PT_MISC_TYPE option;
  PT_NODE *arg1, *arg2;

  switch (node->node_type)
    {
    case PT_GET_OPT_LVL:
      option = node->info.get_opt_lvl.option;
      if (option == PT_OPT_COST)
	{
	  arg1 = node->info.get_opt_lvl.args;
	  if (PT_IS_CHAR_STRING_TYPE (arg1->type_enum))
	    {
	      node->type_enum = PT_TYPE_VARCHAR;
	    }
	  else
	    {
	      PT_ERRORmf (parser, node, MSGCAT_SET_PARSER_SEMANTIC,
			  MSGCAT_SEMANTIC_WANT_TYPE,
			  pt_show_type_enum (PT_TYPE_CHAR));
	      node->type_enum = PT_TYPE_NONE;
	      node = NULL;
	    }
	}
      else
	{
	  node->type_enum = PT_TYPE_INTEGER;
	}
      break;

    case PT_SET_OPT_LVL:
      node->type_enum = PT_TYPE_NONE;
      option = node->info.set_opt_lvl.option;
      arg1 = node->info.set_opt_lvl.val;

      switch (option)
	{
	case PT_OPT_LVL:
	  if (arg1->type_enum != PT_TYPE_INTEGER)
	    {
	      PT_ERRORmf (parser, node, MSGCAT_SET_PARSER_SEMANTIC,
			  MSGCAT_SEMANTIC_WANT_TYPE,
			  pt_show_type_enum (PT_TYPE_INTEGER));
	      node = NULL;
	    }
	  break;

	case PT_OPT_COST:
	  arg2 = arg1->next;
	  if (!PT_IS_CHAR_STRING_TYPE (arg1->type_enum)
	      || !PT_IS_CHAR_STRING_TYPE (arg2->type_enum))
	    {
	      PT_ERRORmf (parser, node, MSGCAT_SET_PARSER_SEMANTIC,
			  MSGCAT_SEMANTIC_WANT_TYPE,
			  pt_show_type_enum (PT_TYPE_CHAR));
	      node = NULL;
	    }
	  break;

	default:
	  break;
	}
      break;

    default:
      break;
    }

  return node;
}

/*
 * pt_common_type () -
 *   return: returns the type into which these two types can be coerced
 *           or PT_TYPE_NONE if no such type exists
 *   arg1_type(in): a data type
 *   arg2_type(in): a data type
 */

PT_TYPE_ENUM
pt_common_type (PT_TYPE_ENUM arg1_type, PT_TYPE_ENUM arg2_type)
{
  PT_TYPE_ENUM common_type = PT_TYPE_NONE;

  if (arg1_type == arg2_type)
    {
      if (arg1_type == PT_TYPE_ENUMERATION)
	{
	  /* The common type between two ENUMs is string */
	  common_type = PT_TYPE_VARCHAR;
	}
      else
	{
	  common_type = arg1_type;
	}
    }
  else if ((PT_IS_NUMERIC_TYPE (arg1_type) && PT_IS_STRING_TYPE (arg2_type))
	   || (PT_IS_NUMERIC_TYPE (arg2_type)
	       && PT_IS_STRING_TYPE (arg1_type)))
    {
      common_type = PT_TYPE_DOUBLE;
    }
  else if ((PT_IS_NUMERIC_TYPE (arg1_type) && arg2_type == PT_TYPE_MAYBE)
	   || (PT_IS_NUMERIC_TYPE (arg2_type) && arg1_type == PT_TYPE_MAYBE))
    {
      common_type = PT_TYPE_DOUBLE;
    }
  else
    {
      switch (arg1_type)
	{
	case PT_TYPE_DOUBLE:
	  switch (arg2_type)
	    {
	    case PT_TYPE_SMALLINT:
	    case PT_TYPE_INTEGER:
	    case PT_TYPE_BIGINT:
	    case PT_TYPE_FLOAT:
	    case PT_TYPE_DOUBLE:
	    case PT_TYPE_NUMERIC:
	    case PT_TYPE_LOGICAL:
	    case PT_TYPE_ENUMERATION:
	      common_type = PT_TYPE_DOUBLE;
	      break;
	    case PT_TYPE_MONETARY:
	      common_type = PT_TYPE_MONETARY;
	      break;
	    default:
	      /* badly formed expression */
	      common_type = PT_TYPE_NONE;
	      break;
	    }
	  break;

	case PT_TYPE_NUMERIC:
	  switch (arg2_type)
	    {
	    case PT_TYPE_NUMERIC:
	    case PT_TYPE_SMALLINT:
	    case PT_TYPE_INTEGER:
	    case PT_TYPE_BIGINT:
	    case PT_TYPE_LOGICAL:
	    case PT_TYPE_ENUMERATION:
	      common_type = PT_TYPE_NUMERIC;
	      break;
	    case PT_TYPE_FLOAT:
	    case PT_TYPE_DOUBLE:
	      common_type = PT_TYPE_DOUBLE;
	      break;
	    case PT_TYPE_MONETARY:
	      common_type = PT_TYPE_MONETARY;
	      break;
	    default:
	      common_type = PT_TYPE_NONE;
	      break;
	    }
	  break;

	case PT_TYPE_FLOAT:
	  switch (arg2_type)
	    {
	    case PT_TYPE_SMALLINT:
	    case PT_TYPE_INTEGER:
	    case PT_TYPE_BIGINT:
	    case PT_TYPE_FLOAT:
	    case PT_TYPE_LOGICAL:
	    case PT_TYPE_ENUMERATION:
	      common_type = PT_TYPE_FLOAT;
	      break;
	    case PT_TYPE_DOUBLE:
	    case PT_TYPE_NUMERIC:
	      common_type = PT_TYPE_DOUBLE;
	      break;
	    case PT_TYPE_MONETARY:
	      common_type = PT_TYPE_MONETARY;
	      break;
	    default:
	      common_type = PT_TYPE_NONE;
	      break;
	    }
	  break;

	case PT_TYPE_INTEGER:
	  switch (arg2_type)
	    {
	    case PT_TYPE_SMALLINT:
	    case PT_TYPE_INTEGER:
	    case PT_TYPE_LOGICAL:
	    case PT_TYPE_ENUMERATION:
	      common_type = PT_TYPE_INTEGER;
	      break;
	    case PT_TYPE_BIGINT:
	      common_type = PT_TYPE_BIGINT;
	      break;
	    case PT_TYPE_FLOAT:
	      common_type = PT_TYPE_FLOAT;
	      break;
	    case PT_TYPE_DOUBLE:
	      common_type = PT_TYPE_DOUBLE;
	      break;
	    case PT_TYPE_DATE:
	      common_type = PT_TYPE_DATE;
	      break;
	    case PT_TYPE_MONETARY:
	      common_type = PT_TYPE_MONETARY;
	      break;
	    case PT_TYPE_TIMESTAMP:
	      common_type = PT_TYPE_TIMESTAMP;
	      break;
	    case PT_TYPE_DATETIME:
	      common_type = PT_TYPE_DATETIME;
	      break;
	    case PT_TYPE_TIME:
	      common_type = PT_TYPE_TIME;
	      break;
	    case PT_TYPE_NUMERIC:
	      common_type = PT_TYPE_NUMERIC;
	      break;
	    default:
	      common_type = PT_TYPE_NONE;
	      break;
	    }
	  break;

	case PT_TYPE_SMALLINT:
	  switch (arg2_type)
	    {
	    case PT_TYPE_SMALLINT:
	    case PT_TYPE_LOGICAL:
	    case PT_TYPE_ENUMERATION:
	      common_type = PT_TYPE_SMALLINT;
	      break;
	    case PT_TYPE_INTEGER:
	      common_type = PT_TYPE_INTEGER;
	      break;
	    case PT_TYPE_BIGINT:
	      common_type = PT_TYPE_BIGINT;
	      break;
	    case PT_TYPE_FLOAT:
	      common_type = PT_TYPE_FLOAT;
	      break;
	    case PT_TYPE_DOUBLE:
	      common_type = PT_TYPE_DOUBLE;
	      break;
	    case PT_TYPE_DATE:
	      common_type = PT_TYPE_DATE;
	      break;
	    case PT_TYPE_MONETARY:
	      common_type = PT_TYPE_MONETARY;
	      break;
	    case PT_TYPE_TIMESTAMP:
	      common_type = PT_TYPE_TIMESTAMP;
	      break;
	    case PT_TYPE_DATETIME:
	      common_type = PT_TYPE_DATETIME;
	      break;
	    case PT_TYPE_TIME:
	      common_type = PT_TYPE_TIME;
	      break;
	    case PT_TYPE_NUMERIC:
	      common_type = PT_TYPE_NUMERIC;
	      break;
	    default:
	      common_type = PT_TYPE_NONE;
	      break;
	    }
	  break;

	case PT_TYPE_BIGINT:
	  switch (arg2_type)
	    {
	    case PT_TYPE_SMALLINT:
	    case PT_TYPE_INTEGER:
	    case PT_TYPE_BIGINT:
	    case PT_TYPE_LOGICAL:
	    case PT_TYPE_ENUMERATION:
	      common_type = PT_TYPE_BIGINT;
	      break;
	    case PT_TYPE_FLOAT:
	      common_type = PT_TYPE_FLOAT;
	      break;
	    case PT_TYPE_DOUBLE:
	      common_type = PT_TYPE_DOUBLE;
	      break;
	    case PT_TYPE_DATE:
	      common_type = PT_TYPE_DATE;
	      break;
	    case PT_TYPE_MONETARY:
	      common_type = PT_TYPE_MONETARY;
	      break;
	    case PT_TYPE_TIMESTAMP:
	      common_type = PT_TYPE_TIMESTAMP;
	      break;
	    case PT_TYPE_DATETIME:
	      common_type = PT_TYPE_DATETIME;
	      break;
	    case PT_TYPE_TIME:
	      common_type = PT_TYPE_TIME;
	      break;
	    case PT_TYPE_NUMERIC:
	      common_type = PT_TYPE_NUMERIC;
	      break;
	    default:
	      common_type = PT_TYPE_NONE;
	      break;
	    }
	  break;

	case PT_TYPE_MONETARY:
	  switch (arg2_type)
	    {
	    case PT_TYPE_MONETARY:
	    case PT_TYPE_SMALLINT:
	    case PT_TYPE_INTEGER:
	    case PT_TYPE_BIGINT:
	    case PT_TYPE_FLOAT:
	    case PT_TYPE_DOUBLE:
	    case PT_TYPE_NUMERIC:
	    case PT_TYPE_LOGICAL:
	    case PT_TYPE_ENUMERATION:
	      common_type = PT_TYPE_MONETARY;
	      break;
	    default:
	      common_type = PT_TYPE_NONE;
	      break;
	    }
	  break;
	case PT_TYPE_DATETIME:
	  switch (arg2_type)
	    {
	    case PT_TYPE_SMALLINT:
	    case PT_TYPE_INTEGER:
	    case PT_TYPE_BIGINT:
	    case PT_TYPE_CHAR:
	    case PT_TYPE_VARCHAR:
	    case PT_TYPE_ENUMERATION:
	    case PT_TYPE_NCHAR:
	    case PT_TYPE_VARNCHAR:
	    case PT_TYPE_DATETIME:
	    case PT_TYPE_TIMESTAMP:
	    case PT_TYPE_DATE:
	      common_type = PT_TYPE_DATETIME;
	      break;

	    default:
	      common_type = PT_TYPE_NONE;
	      break;
	    }
	  break;

	case PT_TYPE_TIMESTAMP:
	  switch (arg2_type)
	    {
	    case PT_TYPE_SMALLINT:
	    case PT_TYPE_INTEGER:
	    case PT_TYPE_BIGINT:
	      if (prm_get_integer_value (PRM_ID_COMPAT_MODE) != COMPAT_MYSQL)
		{
		  common_type = PT_TYPE_TIMESTAMP;
		}
	      else
		{
		  common_type = PT_TYPE_BIGINT;
		}
	      break;

	    case PT_TYPE_CHAR:
	    case PT_TYPE_VARCHAR:
	    case PT_TYPE_NCHAR:
	    case PT_TYPE_VARNCHAR:
	    case PT_TYPE_ENUMERATION:
	    case PT_TYPE_TIMESTAMP:
	    case PT_TYPE_DATE:
	      common_type = PT_TYPE_TIMESTAMP;
	      break;

	    case PT_TYPE_DATETIME:
	      common_type = PT_TYPE_DATETIME;
	      break;

	    default:
	      common_type = PT_TYPE_NONE;
	      break;
	    }
	  break;

	case PT_TYPE_TIME:
	  switch (arg2_type)
	    {
	    case PT_TYPE_SMALLINT:
	    case PT_TYPE_INTEGER:
	    case PT_TYPE_BIGINT:
	      if (prm_get_integer_value (PRM_ID_COMPAT_MODE) != COMPAT_MYSQL)
		{
		  common_type = PT_TYPE_TIME;
		}
	      else
		{
		  common_type = arg2_type;
		}
	      break;
	    case PT_TYPE_CHAR:
	    case PT_TYPE_VARCHAR:
	    case PT_TYPE_NCHAR:
	    case PT_TYPE_VARNCHAR:
	    case PT_TYPE_ENUMERATION:
	    case PT_TYPE_TIME:
	      common_type = PT_TYPE_TIME;
	      break;
	    default:
	      common_type = PT_TYPE_NONE;
	      break;
	    }
	  break;

	case PT_TYPE_DATE:
	  switch (arg2_type)
	    {
	    case PT_TYPE_SMALLINT:
	    case PT_TYPE_INTEGER:
	    case PT_TYPE_BIGINT:
	      if (prm_get_integer_value (PRM_ID_COMPAT_MODE) != COMPAT_MYSQL)
		{
		  common_type = PT_TYPE_DATE;
		}
	      else
		{
		  common_type = arg2_type;
		}
	      break;

	    case PT_TYPE_VARCHAR:
	    case PT_TYPE_CHAR:
	    case PT_TYPE_VARNCHAR:
	    case PT_TYPE_NCHAR:
	    case PT_TYPE_ENUMERATION:
	    case PT_TYPE_DATE:
	      common_type = PT_TYPE_DATE;
	      break;

	    case PT_TYPE_TIMESTAMP:
	      common_type = PT_TYPE_TIMESTAMP;
	      break;

	    case PT_TYPE_DATETIME:
	      common_type = PT_TYPE_DATETIME;
	      break;

	    default:
	      common_type = PT_TYPE_NONE;
	      break;
	    }
	  break;
	case PT_TYPE_CHAR:
	  switch (arg2_type)
	    {
	    case PT_TYPE_DATE:
	    case PT_TYPE_TIME:
	    case PT_TYPE_TIMESTAMP:
	    case PT_TYPE_DATETIME:
	    case PT_TYPE_VARCHAR:
	    case PT_TYPE_CHAR:
	      common_type = arg2_type;
	      break;
	    case PT_TYPE_ENUMERATION:
	      common_type = PT_TYPE_VARCHAR;
	      break;
	    default:
	      common_type = PT_TYPE_NONE;
	      break;
	    }
	  break;

	case PT_TYPE_VARCHAR:
	  switch (arg2_type)
	    {
	    case PT_TYPE_DATE:
	    case PT_TYPE_TIME:
	    case PT_TYPE_TIMESTAMP:
	    case PT_TYPE_DATETIME:
	    case PT_TYPE_VARCHAR:
	      common_type = arg2_type;
	      break;
	    case PT_TYPE_CHAR:
	    case PT_TYPE_ENUMERATION:
	      common_type = PT_TYPE_VARCHAR;
	      break;
	    default:
	      common_type = PT_TYPE_NONE;
	      break;
	    }
	  break;

	case PT_TYPE_NCHAR:
	  switch (arg2_type)
	    {
	    case PT_TYPE_DATE:
	    case PT_TYPE_TIME:
	    case PT_TYPE_TIMESTAMP:
	    case PT_TYPE_DATETIME:
	    case PT_TYPE_NCHAR:
	    case PT_TYPE_VARNCHAR:
	      common_type = arg2_type;
	      break;
	    default:
	      common_type = PT_TYPE_NONE;
	      break;
	    }
	  break;

	case PT_TYPE_VARNCHAR:
	  switch (arg2_type)
	    {
	    case PT_TYPE_DATE:
	    case PT_TYPE_TIME:
	    case PT_TYPE_TIMESTAMP:
	    case PT_TYPE_DATETIME:
	    case PT_TYPE_VARNCHAR:
	      common_type = arg2_type;
	      break;
	    case PT_TYPE_NCHAR:
	      common_type = PT_TYPE_VARNCHAR;
	      break;
	    default:
	      common_type = PT_TYPE_NONE;
	      break;
	    }
	  break;

	case PT_TYPE_VARBIT:
	  switch (arg2_type)
	    {
	    case PT_TYPE_VARBIT:
	    case PT_TYPE_BIT:
	      common_type = PT_TYPE_VARBIT;
	      break;
	    default:
	      common_type = PT_TYPE_NONE;
	      break;
	    }
	  break;

	case PT_TYPE_BIT:
	  switch (arg2_type)
	    {
	    case PT_TYPE_BIT:
	      common_type = PT_TYPE_BIT;
	      break;
	    case PT_TYPE_VARBIT:
	      common_type = PT_TYPE_VARBIT;
	      break;
	    default:
	      common_type = PT_TYPE_NONE;
	      break;
	    }
	  break;

	case PT_TYPE_OBJECT:
	  switch (arg2_type)
	    {
	    case PT_TYPE_OBJECT:
	      common_type = PT_TYPE_OBJECT;
	      break;
	    default:
	      common_type = PT_TYPE_NONE;
	      break;
	    }
	  break;

	case PT_TYPE_SET:
	case PT_TYPE_MULTISET:
	case PT_TYPE_SEQUENCE:
	  switch (arg2_type)
	    {
	    case PT_TYPE_SET:
	    case PT_TYPE_MULTISET:
	    case PT_TYPE_SEQUENCE:
	      common_type = PT_TYPE_MULTISET;
	      break;
	    default:
	      common_type = PT_TYPE_NONE;
	      break;
	    }
	  break;

	case PT_TYPE_LOGICAL:
	  switch (arg2_type)
	    {
	    case PT_TYPE_LOGICAL:
	      common_type = PT_TYPE_LOGICAL;
	      break;
	    case PT_TYPE_DOUBLE:
	      common_type = PT_TYPE_DOUBLE;
	      break;
	    case PT_TYPE_NUMERIC:
	      common_type = PT_TYPE_NUMERIC;
	      break;
	    case PT_TYPE_FLOAT:
	      common_type = PT_TYPE_FLOAT;
	      break;
	    case PT_TYPE_INTEGER:
	      common_type = PT_TYPE_INTEGER;
	      break;
	    case PT_TYPE_SMALLINT:
	      common_type = PT_TYPE_SMALLINT;
	      break;
	    case PT_TYPE_BIGINT:
	      common_type = PT_TYPE_BIGINT;
	      break;
	    case PT_TYPE_MONETARY:
	      common_type = PT_TYPE_MONETARY;
	      break;
	    default:
	      common_type = PT_TYPE_NONE;
	      break;
	    }
	  break;

	case PT_TYPE_ENUMERATION:
	  switch (arg2_type)
	    {
	    case PT_TYPE_SMALLINT:
	    case PT_TYPE_INTEGER:
	    case PT_TYPE_FLOAT:
	    case PT_TYPE_BIGINT:
	    case PT_TYPE_NUMERIC:
	    case PT_TYPE_DOUBLE:
	    case PT_TYPE_MONETARY:
	    case PT_TYPE_VARCHAR:
	    case PT_TYPE_ENUMERATION:
	      common_type = arg2_type;
	      break;
	    case PT_TYPE_CHAR:
	      common_type = PT_TYPE_VARCHAR;
	      break;
	    default:
	      common_type = pt_common_type (PT_TYPE_VARCHAR, arg2_type);
	      break;
	    }
	  break;

	default:
	  common_type = PT_TYPE_NONE;
	  break;
	}
    }

  if (common_type == PT_TYPE_NONE)
    {
      if (arg1_type == PT_TYPE_MAYBE || arg2_type == PT_TYPE_MAYBE)
	{
	  common_type = PT_TYPE_MAYBE;
	}
      else if (arg1_type == PT_TYPE_NA || arg1_type == PT_TYPE_NULL)
	{
	  common_type = arg2_type;
	}
      else if (arg2_type == PT_TYPE_NA || arg2_type == PT_TYPE_NULL)
	{
	  common_type = arg1_type;
	}
    }

  return common_type;
}

/*
 * pt_common_type_op () - return the result type of t1 op t2
 *   return: returns the result type of t1 op t2
 *           or PT_TYPE_NONE if no such type exists
 *   t1(in): a data type
 *   op(in): a binary operator
 *   t2(in): a data type
 */

static PT_TYPE_ENUM
pt_common_type_op (PT_TYPE_ENUM t1, PT_OP_TYPE op, PT_TYPE_ENUM t2)
{
  PT_TYPE_ENUM result_type;

  if (pt_is_op_hv_late_bind (op)
      && (t1 == PT_TYPE_MAYBE || t2 == PT_TYPE_MAYBE))
    {
      result_type = PT_TYPE_MAYBE;
    }
  else
    {
      result_type = pt_common_type (t1, t2);
    }

  switch (op)
    {
    case PT_MINUS:
    case PT_TIMES:
      if (result_type == PT_TYPE_SEQUENCE)
	{
	  result_type = PT_TYPE_MULTISET;
	}
      else if ((PT_IS_STRING_TYPE (t1) && PT_IS_NUMERIC_TYPE (t2))
	       || (PT_IS_NUMERIC_TYPE (t1) && PT_IS_STRING_TYPE (t2)))
	{
	  /* + and - have their own way of handling this situation */
	  return PT_TYPE_NONE;
	}
      break;
    case PT_SUPERSET:
    case PT_SUPERSETEQ:
    case PT_SUBSET:
    case PT_SUBSETEQ:
      if (result_type == PT_TYPE_SEQUENCE)
	{
	  result_type = PT_TYPE_NONE;
	}
      break;
    case PT_IFNULL:
      if (result_type == PT_TYPE_MAYBE)
	{
	  result_type = PT_TYPE_VARCHAR;
	}
      break;
    case PT_COALESCE:
      if (t1 == PT_TYPE_MAYBE)
	{
	  if (t2 == PT_TYPE_MAYBE || t2 == PT_TYPE_NULL)
	    {
	      result_type = PT_TYPE_MAYBE;
	    }
	  else
	    {
	      result_type = t2;
	    }
	}
      else if (t2 == PT_TYPE_MAYBE)
	{
	  if (t1 == PT_TYPE_MAYBE || t1 == PT_TYPE_NULL)
	    {
	      result_type = PT_TYPE_MAYBE;
	    }
	  else
	    {
	      result_type = t1;
	    }
	}
      else if (PT_IS_DATE_TIME_TYPE (result_type) && (t1 != t2))
	{
	  if (PT_IS_DATE_TIME_TYPE (t1))
	    {
	      result_type =
		(PT_IS_DATE_TIME_TYPE (t2)) ? PT_TYPE_DATETIME : t1;
	    }
	  else if (PT_IS_DATE_TIME_TYPE (t2))
	    {
	      result_type =
		(PT_IS_DATE_TIME_TYPE (t1)) ? PT_TYPE_DATETIME : t2;
	    }
	}
      break;
    default:
      break;
    }
  /*
   * true + true must not be logical.
   * Same goes for true*true, (i or j)+(i and j) etc.
   * Basic rule: if both operands are logical but the operation is not logical,
   * then the resulting type MUST be integer. Otherwise strange things happen
   * while generating xasl: predicate expressions with wrong operators.
   */
  if (t1 == PT_TYPE_LOGICAL && t2 == PT_TYPE_LOGICAL
      && !pt_is_operator_logical (op))
    {
      result_type = PT_TYPE_INTEGER;
    }

  return result_type;
}


/*
 * pt_upd_domain_info () - preparing a node for a binary operation
 * 			   involving a parameterized type
 *   return: NO_ERROR on success, non-zero for ERROR
 *   parser(in):
 *   arg1(in):
 *   arg2(in):
 *   op(in):
 *   common_type(in):
 *   node(out):
 */

static int
pt_upd_domain_info (PARSER_CONTEXT * parser,
		    PT_NODE * arg1, PT_NODE * arg2,
		    PT_OP_TYPE op, PT_TYPE_ENUM common_type, PT_NODE * node)
{
  int arg1_prec = 0;
  int arg1_dec_prec = 0;
  int arg1_units = 0;
  int arg2_prec = 0;
  int arg2_dec_prec = 0;
  int arg2_units = 0;
  PT_NODE *dt = NULL;
  bool do_detect_collation = true;

  if (node->data_type)
    {				/* node has already been resolved */
      return NO_ERROR;
    }

  /* Retrieve the domain information for arg1 & arg2 */
  if (arg1 && arg1->data_type)
    {
      arg1_prec = arg1->data_type->info.data_type.precision;
      arg1_dec_prec = arg1->data_type->info.data_type.dec_precision;
      arg1_units = arg1->data_type->info.data_type.units;
    }
  else if (arg1 && arg1->type_enum == PT_TYPE_INTEGER)
    {
      arg1_prec = TP_INTEGER_PRECISION;
      arg1_dec_prec = 0;
      arg1_units = 0;
    }
  else if (arg1 && arg1->type_enum == PT_TYPE_BIGINT)
    {
      arg1_prec = TP_BIGINT_PRECISION;
      arg1_dec_prec = 0;
      arg1_units = 0;
    }
  else if (arg1 && arg1->type_enum == PT_TYPE_SMALLINT)
    {
      arg1_prec = TP_SMALLINT_PRECISION;
      arg1_dec_prec = 0;
      arg1_units = 0;
    }
  else if (arg1 && arg1->type_enum == PT_TYPE_NUMERIC)
    {
      arg1_prec = DB_DEFAULT_NUMERIC_PRECISION;
      arg1_dec_prec = DB_DEFAULT_NUMERIC_SCALE;
      arg1_units = 0;
    }
  else if (arg1 && arg1->type_enum == PT_TYPE_MAYBE)
    {
      arg1_prec = TP_FLOATING_PRECISION_VALUE;
      arg1_dec_prec = 0;
      arg1_units = 0;
    }
  else
    {
      arg1_prec = 0;
      arg1_dec_prec = 0;
      arg1_units = 0;
    }

  if (arg2 && arg2->data_type)
    {
      arg2_prec = arg2->data_type->info.data_type.precision;
      arg2_dec_prec = arg2->data_type->info.data_type.dec_precision;
      arg2_units = arg2->data_type->info.data_type.units;
    }
  else if (arg2 && arg2->type_enum == PT_TYPE_INTEGER)
    {
      arg2_prec = TP_INTEGER_PRECISION;
      arg2_dec_prec = 0;
      arg2_units = 0;
    }
  else if (arg2 && arg2->type_enum == PT_TYPE_BIGINT)
    {
      arg2_prec = TP_BIGINT_PRECISION;
      arg2_dec_prec = 0;
      arg2_units = 0;
    }
  else if (arg2 && arg2->type_enum == PT_TYPE_SMALLINT)
    {
      arg2_prec = TP_SMALLINT_PRECISION;
      arg2_dec_prec = 0;
      arg2_units = 0;
    }
  else if (arg2 && arg2->type_enum == PT_TYPE_NUMERIC)
    {
      arg2_prec = DB_DEFAULT_NUMERIC_PRECISION;
      arg2_dec_prec = DB_DEFAULT_NUMERIC_SCALE;
      arg2_units = 0;
    }
  else if (arg2 && arg2->type_enum == PT_TYPE_MAYBE)
    {
      arg2_prec = TP_FLOATING_PRECISION_VALUE;
      arg2_dec_prec = 0;
      arg2_units = 0;
    }
  else
    {
      arg2_prec = 0;
      arg2_dec_prec = 0;
      arg2_units = 0;
    }

  if (op == PT_MINUS || op == PT_PLUS || op == PT_STRCAT
      || op == PT_SYS_CONNECT_BY_PATH
      || op == PT_PRIOR || op == PT_CONNECT_BY_ROOT
      || op == PT_QPRIOR
      || op == PT_UNARY_MINUS || op == PT_FLOOR
      || op == PT_CEIL || op == PT_ABS || op == PT_ROUND
      || op == PT_TRUNC || op == PT_CASE
      || op == PT_NULLIF || op == PT_COALESCE
      || op == PT_NVL || op == PT_NVL2 || op == PT_DECODE
      || op == PT_LEAST || op == PT_GREATEST || op == PT_CHR
      || op == PT_BIT_NOT || op == PT_BIT_AND || op == PT_BIT_OR
      || op == PT_BIT_XOR || op == PT_BITSHIFT_LEFT
      || op == PT_BITSHIFT_RIGHT || op == PT_DIV || op == PT_MOD
      || op == PT_IF || op == PT_IFNULL
      || op == PT_CONCAT || op == PT_CONCAT_WS || op == PT_FIELD
      || op == PT_UNIX_TIMESTAMP || op == PT_BIT_COUNT || op == PT_REPEAT
      || op == PT_SPACE || op == PT_MD5 || op == PT_TIMEF)
    {
      dt = parser_new_node (parser, PT_DATA_TYPE);
      if (dt == NULL)
	{
	  return ER_OUT_OF_VIRTUAL_MEMORY;
	}
    }
  else if (common_type == PT_TYPE_NUMERIC
	   && (op == PT_TIMES || op == PT_POWER || op == PT_DIVIDE
	       || op == PT_MODULUS))
    {
      dt = parser_new_node (parser, PT_DATA_TYPE);
      if (dt == NULL)
	{
	  return ER_OUT_OF_VIRTUAL_MEMORY;
	}
    }

  switch (op)
    {
    case PT_MINUS:
    case PT_PLUS:
      if (arg1_prec == TP_FLOATING_PRECISION_VALUE
	  || arg2_prec == TP_FLOATING_PRECISION_VALUE)
	{
	  dt->info.data_type.precision = TP_FLOATING_PRECISION_VALUE;
	  dt->info.data_type.dec_precision = 0;
	  dt->info.data_type.units = 0;
	}
      else if (common_type == PT_TYPE_NUMERIC)
	{
	  int integral_digits1, integral_digits2;

	  integral_digits1 = arg1_prec - arg1_dec_prec;
	  integral_digits2 = arg2_prec - arg2_dec_prec;
	  dt->info.data_type.dec_precision = MAX (arg1_dec_prec,
						  arg2_dec_prec);
	  dt->info.data_type.precision = (dt->info.data_type.dec_precision +
					  MAX (integral_digits1,
					       integral_digits2) + 1);
	  dt->info.data_type.units = 0;
	}
      else
	{
	  dt->info.data_type.precision = arg1_prec + arg2_prec;
	  dt->info.data_type.dec_precision = 0;
	  dt->info.data_type.units = arg1_units;
	}
      break;

    case PT_REPEAT:
      {
	int precision = TP_FLOATING_PRECISION_VALUE;
	if (arg1_prec != TP_FLOATING_PRECISION_VALUE
	    && arg2->node_type == PT_VALUE
	    && arg2->type_enum == PT_TYPE_INTEGER)
	  {
	    precision = arg1_prec * arg2->info.value.data_value.i;
	  }
	dt->info.data_type.precision = precision;
	dt->info.data_type.dec_precision = 0;
	dt->info.data_type.units = arg1_units;
      }
      break;

    case PT_SPACE:
      if (arg1->node_type == PT_VALUE)
	{
	  switch (arg1->type_enum)
	    {
	    case PT_TYPE_SMALLINT:
	      if (arg1->info.value.data_value.b < 0)
		{
		  dt->info.data_type.precision = 0;
		}
	      else
		{
		  dt->info.data_type.precision =
		    arg1->info.value.data_value.b;
		}
	      break;
	    case PT_TYPE_INTEGER:
	      if (arg1->info.value.data_value.i > DB_MAX_VARCHAR_PRECISION)
		{
		  node->type_enum = PT_TYPE_NULL;
		}
	      else
		{
		  if (arg1->info.value.data_value.i < 0)
		    {
		      dt->info.data_type.precision = 0;
		    }
		  else
		    {
		      dt->info.data_type.precision =
			arg1->info.value.data_value.i;
		    }
		}
	      break;
	    case PT_TYPE_BIGINT:
	      if (arg1->info.value.data_value.bigint >
		  DB_MAX_VARCHAR_PRECISION)
		{
		  node->type_enum = PT_TYPE_NULL;
		}
	      else
		{
		  if (arg1->info.value.data_value.bigint < 0)
		    {
		      dt->info.data_type.precision = 0;
		    }
		  else
		    {
		      dt->info.data_type.precision =
			(int) arg1->info.value.data_value.bigint;
		    }
		}
	      break;
	    default:
	      dt->info.data_type.precision = TP_FLOATING_PRECISION_VALUE;
	      break;
	    }
	}
      else
	dt->info.data_type.precision = TP_FLOATING_PRECISION_VALUE;
      break;

    case PT_CONCAT:
    case PT_CONCAT_WS:
    case PT_FIELD:
    case PT_STRCAT:
    case PT_SYS_CONNECT_BY_PATH:
      dt->info.data_type.precision = TP_FLOATING_PRECISION_VALUE;
      dt->info.data_type.dec_precision = 0;
      dt->info.data_type.units = 0;
      break;

    case PT_TIMES:
    case PT_POWER:
      if (common_type == PT_TYPE_NUMERIC)
	{
	  if (arg1_prec == TP_FLOATING_PRECISION_VALUE
	      || arg2_prec == TP_FLOATING_PRECISION_VALUE)
	    {
	      dt->info.data_type.precision = TP_FLOATING_PRECISION_VALUE;
	      dt->info.data_type.dec_precision = 0;
	      dt->info.data_type.units = 0;
	    }
	  else
	    {
	      dt->info.data_type.precision = arg1_prec + arg2_prec + 1;
	      dt->info.data_type.dec_precision = (arg1_dec_prec
						  + arg2_dec_prec);
	      dt->info.data_type.units = 0;
	    }
	}
      break;

    case PT_DIVIDE:
    case PT_MODULUS:
      if (common_type == PT_TYPE_NUMERIC)
	{
	  if (arg1_prec == TP_FLOATING_PRECISION_VALUE
	      || arg2_prec == TP_FLOATING_PRECISION_VALUE)
	    {
	      dt->info.data_type.precision = TP_FLOATING_PRECISION_VALUE;
	      dt->info.data_type.dec_precision = 0;
	      dt->info.data_type.units = 0;
	    }
	  else
	    {
	      int scaleup = 0;

	      if (arg2_dec_prec > 0)
		{
		  scaleup = (MAX (arg1_dec_prec, arg2_dec_prec) +
			     arg2_dec_prec - arg1_dec_prec);
		}
	      dt->info.data_type.precision = arg1_prec + scaleup;
	      dt->info.data_type.dec_precision = ((arg1_dec_prec >
						   arg2_dec_prec) ?
						  arg1_dec_prec :
						  arg2_dec_prec);
	      dt->info.data_type.units = 0;
	      if (!prm_get_bool_value (PRM_ID_COMPAT_NUMERIC_DIVISION_SCALE)
		  && op == PT_DIVIDE)
		{
		  if (dt->info.data_type.dec_precision <
		      DB_DEFAULT_NUMERIC_DIVISION_SCALE)
		    {
		      int org_prec, org_scale, new_prec, new_scale;
		      int scale_delta;

		      org_prec = MIN (38, dt->info.data_type.precision);
		      org_scale = dt->info.data_type.dec_precision;
		      scale_delta = (DB_DEFAULT_NUMERIC_DIVISION_SCALE
				     - org_scale);
		      new_scale = org_scale + scale_delta;
		      new_prec = org_prec + scale_delta;
		      if (new_prec > DB_MAX_NUMERIC_PRECISION)
			{
			  new_scale -= (new_prec - DB_MAX_NUMERIC_PRECISION);
			  new_prec = DB_MAX_NUMERIC_PRECISION;
			}

		      dt->info.data_type.precision = new_prec;
		      dt->info.data_type.dec_precision = new_scale;
		    }
		}
	    }
	}
      break;
    case PT_TIMEF:
      dt->info.data_type.precision = 12;
      dt->info.data_type.dec_precision = 0;
      dt->info.data_type.units = 0;
      break;

    case PT_UNARY_MINUS:
    case PT_FLOOR:
    case PT_CEIL:
    case PT_ABS:
    case PT_ROUND:
    case PT_TRUNC:
    case PT_PRIOR:
    case PT_CONNECT_BY_ROOT:
    case PT_QPRIOR:
    case PT_BIT_NOT:
    case PT_BIT_AND:
    case PT_BIT_OR:
    case PT_BIT_XOR:
    case PT_BIT_COUNT:
    case PT_BITSHIFT_LEFT:
    case PT_BITSHIFT_RIGHT:
    case PT_DIV:
    case PT_MOD:
      dt->info.data_type.precision = arg1_prec;
      dt->info.data_type.dec_precision = arg1_dec_prec;
      dt->info.data_type.units = arg1_units;
      break;

    case PT_IF:
    case PT_IFNULL:
    case PT_CASE:
    case PT_NULLIF:
    case PT_COALESCE:
    case PT_NVL:
    case PT_NVL2:
    case PT_DECODE:
    case PT_LEAST:
    case PT_GREATEST:
      if (arg1_prec == TP_FLOATING_PRECISION_VALUE
	  || arg2_prec == TP_FLOATING_PRECISION_VALUE)
	{
	  dt->info.data_type.precision = TP_FLOATING_PRECISION_VALUE;
	  dt->info.data_type.dec_precision = 0;
	  dt->info.data_type.units = 0;
	}
      else if (common_type == PT_TYPE_NUMERIC)
	{
	  int integral_digits1, integral_digits2;

	  integral_digits1 = arg1_prec - arg1_dec_prec;
	  integral_digits2 = arg2_prec - arg2_dec_prec;
	  dt->info.data_type.dec_precision = MAX (arg1_dec_prec,
						  arg2_dec_prec);
	  dt->info.data_type.precision = (MAX (integral_digits1,
					       integral_digits2)
					  + dt->info.data_type.dec_precision);
	  dt->info.data_type.units = 0;
	}
      else if ((arg1->type_enum != arg2->type_enum)
	       && pt_is_op_with_forced_common_type (op))
	{
	  dt->info.data_type.precision = TP_FLOATING_PRECISION_VALUE;
	  dt->info.data_type.dec_precision = 0;
	  dt->info.data_type.units = 0;
	}
      else
	{
	  dt->info.data_type.precision = MAX (arg1_prec, arg2_prec);
	  dt->info.data_type.dec_precision = 0;
	  if (arg1 && arg1->type_enum == common_type)
	    {
	      dt->info.data_type.units = arg1_units;
	    }
	  else
	    {
	      dt->info.data_type.units = arg2_units;
	    }
	}
      break;

    case PT_BIT_TO_BLOB:
    case PT_CHAR_TO_BLOB:
    case PT_BLOB_FROM_FILE:
      assert (dt == NULL);
      dt = pt_make_prim_data_type (parser, PT_TYPE_BLOB);
      if (dt == NULL)
	{
	  return ER_OUT_OF_VIRTUAL_MEMORY;
	}
      break;

    case PT_BLOB_LENGTH:
    case PT_CLOB_LENGTH:
      assert (dt == NULL);
      dt = pt_make_prim_data_type (parser, PT_TYPE_BIGINT);
      if (dt == NULL)
	{
	  return ER_OUT_OF_VIRTUAL_MEMORY;
	}
      break;

    case PT_BLOB_TO_BIT:
      assert (dt == NULL);
      dt = pt_make_prim_data_type (parser, PT_TYPE_VARBIT);
      if (dt == NULL)
	{
	  return ER_OUT_OF_VIRTUAL_MEMORY;
	}
      break;
    case PT_CLOB_TO_CHAR:
      assert (dt == NULL);
      dt = pt_make_prim_data_type (parser, PT_TYPE_VARCHAR);
      if (dt == NULL)
	{
	  return ER_OUT_OF_VIRTUAL_MEMORY;
	}
      if (arg2 != NULL)
	{
	  assert (arg2->node_type == PT_VALUE);
	  assert (arg2->type_enum == PT_TYPE_INTEGER);
	  dt->info.data_type.units = arg2->info.value.data_value.i;
	  dt->info.data_type.collation_id =
	    LANG_GET_BINARY_COLLATION (dt->info.data_type.units);
	  do_detect_collation = false;
	}
      break;


    case PT_CHAR_TO_CLOB:
    case PT_CLOB_FROM_FILE:
      assert (dt == NULL);
      dt = pt_make_prim_data_type (parser, PT_TYPE_CLOB);
      if (dt == NULL)
	{
	  return ER_OUT_OF_VIRTUAL_MEMORY;
	}
      break;
    case PT_LIST_DBS:
    case PT_TO_CHAR:
    case PT_USER:
    case PT_DATABASE:
    case PT_SCHEMA:
    case PT_CURRENT_USER:
    case PT_INET_NTOA:
      assert (dt == NULL);
      dt = pt_make_prim_data_type (parser, PT_TYPE_VARCHAR);
      break;
    case PT_SUBSTRING_INDEX:
      assert (dt == NULL);
      dt = parser_copy_tree_list (parser, arg1->data_type);
      break;
    case PT_VERSION:
      assert (dt == NULL);
      dt = pt_make_prim_data_type (parser, PT_TYPE_VARCHAR);
      /* Set a large enough precision so that CUBRID is able to handle
         versions like 'xxx.xxx.xxx.xxxx'. */
      dt->info.data_type.precision = 16;
      break;
    case PT_CHR:
      assert (dt != NULL);
      dt->info.data_type.precision = TP_FLOATING_PRECISION_VALUE;
      if (arg2 != NULL)
	{
	  assert (arg2->node_type == PT_VALUE);
	  assert (arg2->type_enum == PT_TYPE_INTEGER);
	  dt->info.data_type.units = arg2->info.value.data_value.i;
	  dt->info.data_type.collation_id =
	    LANG_GET_BINARY_COLLATION (dt->info.data_type.units);
	  do_detect_collation = false;
	}
      break;

    case PT_MD5:
      assert (dt != NULL);
      dt->info.data_type.precision = 32;
      break;
    case PT_LAST_INSERT_ID:
      assert (dt == NULL);
      /* last insert id returns NUMERIC (38, 0) */
      dt = pt_make_prim_data_type (parser, PT_TYPE_NUMERIC);
      dt->info.data_type.precision = DB_MAX_NUMERIC_PRECISION;
      dt->info.data_type.dec_precision = 0;
      break;

    case PT_BIN:
      assert (dt == NULL);
      dt = pt_make_prim_data_type (parser, node->type_enum);
      if (dt == NULL)
	{
	  break;
	}
      /* bin returns the binary representation of a BIGINT */
      dt->info.data_type.precision = (sizeof (DB_BIGINT) * 8);
      break;

    case PT_ADDTIME:
      if (node->type_enum == PT_TYPE_VARCHAR)
	{
	  assert (dt == NULL);
	  dt = pt_make_prim_data_type (parser, node->type_enum);
	  if (dt == NULL)
	    {
	      break;
	    }
	  /* holds at most a DATETIME type */
	  dt->info.data_type.precision = 26;
	}
      break;

    case PT_TRIM:
    case PT_LTRIM:
    case PT_RTRIM:
    case PT_MID:
    case PT_REPLACE:
    case PT_TRANSLATE:
    case PT_TIME_FORMAT:
    case PT_FORMAT:
    case PT_TYPEOF:
    case PT_LPAD:
    case PT_RPAD:
    case PT_HEX:
    case PT_SUBSTRING:
    case PT_CHARSET:
    case PT_COERCIBILITY:
    case PT_COLLATION:
      assert (dt == NULL);
      dt = pt_make_prim_data_type (parser, node->type_enum);
      dt->info.data_type.precision = TP_FLOATING_PRECISION_VALUE;
      break;

    case PT_SUBDATE:
    case PT_ADDDATE:
    case PT_DATE_SUB:
    case PT_DATE_ADD:
    case PT_DATEF:
      if (PT_IS_STRING_TYPE (node->type_enum))
	{
	  assert (dt == NULL);
	  dt = pt_make_prim_data_type (parser, node->type_enum);
	  dt->info.data_type.precision = 32;
	}
      break;

    case PT_FROM_UNIXTIME:
    case PT_DATE_FORMAT:
      if (PT_IS_STRING_TYPE (node->type_enum))
	{
	  assert (dt == NULL);
	  dt = pt_make_prim_data_type (parser, node->type_enum);
	  dt->info.data_type.precision = TP_FLOATING_PRECISION_VALUE;
	}
      break;

    case PT_LOWER:
    case PT_UPPER:
    case PT_REVERSE:
    case PT_LEFT:
    case PT_RIGHT:
      assert (dt == NULL);
      if (PT_IS_HOSTVAR (arg1))
	{
	  /* we resolved the node type to a variable char type
	   * (VARCHAR orVARNCHAR) but we have to set an unknown precision
	   * here
	   */
	  dt = pt_make_prim_data_type (parser, node->type_enum);
	  dt->info.data_type.precision = TP_FLOATING_PRECISION_VALUE;
	}
      else
	{
	  dt = parser_copy_tree_list (parser, arg1->data_type);
	}
      break;

    case PT_TO_NUMBER:
      {
	int prec = 0, scale = 0;
	pt_to_regu_resolve_domain (&prec, &scale, arg2);
	dt = pt_make_prim_data_type_fortonum (parser, prec, scale);
	break;
      }

    case PT_EXEC_STATS:
      assert (dt == NULL);
      dt = pt_make_prim_data_type (parser, PT_TYPE_BIGINT);
      break;

    default:
      break;
    }

  if (dt)
    {
      /* in any case the precision can't be greater than
       * the max precision for the result.
       */
      switch (common_type)
	{
	case PT_TYPE_CHAR:
	  dt->info.data_type.precision =
	    ((dt->info.data_type.precision > DB_MAX_CHAR_PRECISION)
	     ? DB_MAX_CHAR_PRECISION : dt->info.data_type.precision);
	  break;

	case PT_TYPE_VARCHAR:
	  dt->info.data_type.precision =
	    ((dt->info.data_type.precision > DB_MAX_VARCHAR_PRECISION)
	     ? DB_MAX_VARCHAR_PRECISION : dt->info.data_type.precision);
	  break;

	case PT_TYPE_NCHAR:
	  dt->info.data_type.precision =
	    ((dt->info.data_type.precision > DB_MAX_NCHAR_PRECISION)
	     ? DB_MAX_NCHAR_PRECISION : dt->info.data_type.precision);
	  break;

	case PT_TYPE_VARNCHAR:
	  dt->info.data_type.precision =
	    ((dt->info.data_type.precision > DB_MAX_VARNCHAR_PRECISION)
	     ? DB_MAX_VARNCHAR_PRECISION : dt->info.data_type.precision);
	  break;

	case PT_TYPE_BIT:
	  dt->info.data_type.precision =
	    ((dt->info.data_type.precision > DB_MAX_BIT_PRECISION)
	     ? DB_MAX_BIT_PRECISION : dt->info.data_type.precision);
	  break;

	case PT_TYPE_VARBIT:
	  dt->info.data_type.precision =
	    ((dt->info.data_type.precision > DB_MAX_VARBIT_PRECISION)
	     ? DB_MAX_VARBIT_PRECISION : dt->info.data_type.precision);
	  break;

	case PT_TYPE_NUMERIC:
	  if (dt->info.data_type.dec_precision > DB_MAX_NUMERIC_PRECISION)
	    {
	      dt->info.data_type.dec_precision =
		(dt->info.data_type.dec_precision -
		 (dt->info.data_type.precision - DB_MAX_NUMERIC_PRECISION));
	    }

	  dt->info.data_type.precision =
	    ((dt->info.data_type.precision > DB_MAX_NUMERIC_PRECISION)
	     ? DB_MAX_NUMERIC_PRECISION : dt->info.data_type.precision);
	  break;

	case PT_TYPE_ENUMERATION:
	  if (arg1 != NULL && arg1->type_enum == PT_TYPE_ENUMERATION)
	    {
	      dt = parser_copy_tree_list (parser, arg1->data_type);
	    }
	  else if (arg2 != NULL && arg2->type_enum == PT_TYPE_ENUMERATION)
	    {
	      dt = parser_copy_tree_list (parser, arg2->data_type);
	    }
	  break;

	default:
	  break;
	}

      /* Basic collation inference, add specific code in
       * 'pt_check_expr_collation' */
      if (do_detect_collation && PT_HAS_COLLATION (common_type))
	{
	  if (arg1 != NULL && PT_HAS_COLLATION (arg1->type_enum)
	      && arg2 != NULL && !PT_HAS_COLLATION (arg2->type_enum)
	      && arg1->data_type != NULL)
	    {
	      dt->info.data_type.units =
		arg1->data_type->info.data_type.units;
	      dt->info.data_type.collation_id =
		arg1->data_type->info.data_type.collation_id;
	    }
	  else if (arg2 != NULL && PT_HAS_COLLATION (arg2->type_enum)
		   && arg2 != NULL && !PT_HAS_COLLATION (arg1->type_enum)
		   && arg2->data_type != NULL)
	    {
	      dt->info.data_type.units =
		arg2->data_type->info.data_type.units;
	      dt->info.data_type.collation_id =
		arg2->data_type->info.data_type.collation_id;
	    }
	  else
	    {
	      dt->info.data_type.units = (int) LANG_SYS_CODESET;
	      dt->info.data_type.collation_id = LANG_SYS_COLLATION;
	    }
	}
      node->data_type = dt;
      node->data_type->type_enum = common_type;
    }

  return NO_ERROR;
}

/*
 * pt_check_and_coerce_to_time () - check if explicit time format and
 * 				    coerce to time
 *   return: NO_ERROR on success, non-zero for ERROR
 *   parser(in):
 *   src(in/out): a pointer to the original PT_VALUE
 */
static int
pt_check_and_coerce_to_time (PARSER_CONTEXT * parser, PT_NODE * src)
{
  DB_VALUE *db_src = NULL;
  char *cp;
  DB_TYPE dbtype;
  int cp_len;

  db_src = pt_value_to_db (parser, src);
  if (!db_src)
    {
      return ER_TIME_CONVERSION;
    }

  dbtype = DB_VALUE_TYPE (db_src);
  if (dbtype != DB_TYPE_VARCHAR &&
      dbtype != DB_TYPE_CHAR &&
      dbtype != DB_TYPE_VARNCHAR && dbtype != DB_TYPE_NCHAR)
    {
      return ER_TIME_CONVERSION;
    }

  if (DB_GET_STRING (db_src) == NULL)
    {
      return ER_TIME_CONVERSION;
    }

  cp = DB_GET_STRING (db_src);
  cp_len = DB_GET_STRING_SIZE (db_src);
  if (db_string_check_explicit_time (cp, cp_len) == false)
    {
      return ER_TIME_CONVERSION;
    }

  return pt_coerce_value (parser, src, src, PT_TYPE_TIME, NULL);
}

/*
 * pt_check_and_coerce_to_date () - check if explicit date format and
 * 				    coerce to date
 *   return: NO_ERROR on success, non-zero for ERROR
 *   parser(in):
 *   src(in/out): node to be checked
 */
static int
pt_check_and_coerce_to_date (PARSER_CONTEXT * parser, PT_NODE * src)
{
  DB_VALUE *db_src = NULL;
  char *str = NULL;
  int str_len;

  assert (src != NULL);

  if (!PT_IS_CHAR_STRING_TYPE (src->type_enum))
    {
      return ER_DATE_CONVERSION;
    }

  db_src = pt_value_to_db (parser, src);
  if (DB_IS_NULL (db_src))
    {
      return ER_DATE_CONVERSION;
    }

  if (DB_GET_STRING (db_src) == NULL)
    {
      return ER_DATE_CONVERSION;
    }

  str = DB_GET_STRING (db_src);
  str_len = DB_GET_STRING_SIZE (db_src);
  if (!db_string_check_explicit_date (str, str_len))
    {
      return ER_DATE_CONVERSION;
    }
  return pt_coerce_value (parser, src, src, PT_TYPE_DATE, NULL);
}

/*
 * pt_coerce_str_to_time_date_utime_datetime () - try to coerce into
 * 				     a date, time or utime
 *   return: NO_ERROR on success, non-zero for ERROR
 *   parser(in):
 *   src(in/out): a pointer to the original PT_VALUE
 *   result_type(out): the result type of the coerced result
 */
static int
pt_coerce_str_to_time_date_utime_datetime (PARSER_CONTEXT * parser,
					   PT_NODE * src,
					   PT_TYPE_ENUM * result_type)
{
  int result = -1;

  if (src == NULL
      || !PT_IS_CHAR_STRING_TYPE (src->type_enum) || pt_has_error (parser))
    {
      return result;
    }

  /* try coercing to time */
  if (pt_check_and_coerce_to_time (parser, src) == NO_ERROR)
    {
      *result_type = PT_TYPE_TIME;
      result = NO_ERROR;
    }
  else
    {
      /* get rid of error msg from previous coercion */
      parser_free_tree (parser, parser->error_msgs);
      parser->error_msgs = NULL;

      /* try coercing to date */
      if (pt_check_and_coerce_to_date (parser, src) == NO_ERROR)
	{
	  *result_type = PT_TYPE_DATE;
	  result = NO_ERROR;
	}
      else
	{
	  parser_free_tree (parser, parser->error_msgs);
	  parser->error_msgs = NULL;

	  /* try coercing to utime */
	  if (pt_coerce_value (parser, src, src,
			       PT_TYPE_TIMESTAMP, NULL) == NO_ERROR)
	    {
	      *result_type = PT_TYPE_TIMESTAMP;
	      result = NO_ERROR;
	    }
	  else
	    {
	      parser_free_tree (parser, parser->error_msgs);
	      parser->error_msgs = NULL;

	      /* try coercing to datetime */
	      if (pt_coerce_value (parser, src, src,
				   PT_TYPE_DATETIME, NULL) == NO_ERROR)
		{
		  *result_type = PT_TYPE_DATETIME;
		  result = NO_ERROR;
		}
	    }
	}
    }

  return result;
}


/*
 * pt_coerce_3args () - try to coerce 3 opds into their common_type if any
 *   return: returns 1 on success, 0 otherwise
 *   parser(in): the parser context
 *   arg1(in): 1st operand
 *   arg2(in): 2nd operand
 *   arg3(in): 3rd operand
 */

static int
pt_coerce_3args (PARSER_CONTEXT * parser,
		 PT_NODE * arg1, PT_NODE * arg2, PT_NODE * arg3)
{
  PT_TYPE_ENUM common_type;
  PT_NODE *data_type = NULL;
  int result = 1;

  common_type = pt_common_type (arg1->type_enum,
				pt_common_type (arg2->type_enum,
						arg3->type_enum));
  if (common_type != PT_TYPE_NONE)
    {
      /* try to coerce non-identical args into the common type */
      if (PT_IS_NUMERIC_TYPE (common_type) || PT_IS_STRING_TYPE (common_type))
	{
	  data_type = NULL;
	}
      else
	{
	  if (arg1->type_enum == common_type)
	    data_type = arg1->data_type;
	  else if (arg2->type_enum == common_type)
	    data_type = arg2->data_type;
	  else if (arg3->type_enum == common_type)
	    data_type = arg3->data_type;
	}

      if (arg1->type_enum != common_type)
	{
	  result = (result
		    && (pt_coerce_value (parser, arg1, arg1,
					 common_type,
					 (PT_IS_COLLECTION_TYPE (common_type)
					  ? arg1->data_type : data_type)) ==
			NO_ERROR));
	}

      if (arg2->type_enum != common_type)
	{
	  result = (result
		    && (pt_coerce_value (parser, arg2, arg2,
					 common_type,
					 (PT_IS_COLLECTION_TYPE (common_type)
					  ? arg2->data_type : data_type)) ==
			NO_ERROR));
	}

      if (arg3->type_enum != common_type)
	{
	  result = (result
		    && (pt_coerce_value (parser, arg3, arg3,
					 common_type,
					 (PT_IS_COLLECTION_TYPE (common_type)
					  ? arg3->data_type : data_type)) ==
			NO_ERROR));
	}
    }
  else
    {
      result = 0;
    }

  return result;
}

/* pt_character_length_for_type() -
    return: number of characters that a value of the given type can possibly
	    occuppy when cast to a CHAR type.
    node(in): node with type whose character length is to be returned.
    coerce_type(in): string type that node will be cast to
*/
static int
pt_character_length_for_node (PT_NODE * node, const PT_TYPE_ENUM coerce_type)
{
  int precision = TP_FLOATING_PRECISION_VALUE;

  switch (node->type_enum)
    {
    case PT_TYPE_DOUBLE:
      precision = TP_DOUBLE_AS_CHAR_LENGTH;
      break;
    case PT_TYPE_FLOAT:
      precision = TP_FLOAT_AS_CHAR_LENGTH;
      break;
    case PT_TYPE_MONETARY:
      precision = TP_MONETARY_AS_CHAR_LENGTH;
      break;
    case PT_TYPE_BIGINT:
      precision = TP_BIGINT_AS_CHAR_LENGTH;
      break;
    case PT_TYPE_INTEGER:
      precision = TP_INTEGER_AS_CHAR_LENGTH;
      break;
    case PT_TYPE_SMALLINT:
      precision = TP_SMALLINT_AS_CHAR_LENGTH;
      break;
    case PT_TYPE_TIME:
      precision = TP_TIME_AS_CHAR_LENGTH;
      break;
    case PT_TYPE_DATE:
      precision = TP_DATE_AS_CHAR_LENGTH;
      break;
    case PT_TYPE_TIMESTAMP:
      precision = TP_TIMESTAMP_AS_CHAR_LENGTH;
      break;
    case PT_TYPE_DATETIME:
      precision = TP_DATETIME_AS_CHAR_LENGTH;
      break;
    case PT_TYPE_NUMERIC:
      precision = node->data_type->info.data_type.precision;
      if (precision == 0 || precision == DB_DEFAULT_PRECISION)
	{
	  precision = DB_DEFAULT_NUMERIC_PRECISION;
	}
      precision++;		/* for sign */

      if (node->data_type->info.data_type.dec_precision
	  &&
	  (node->data_type->info.data_type.dec_precision != DB_DEFAULT_SCALE
	   || node->data_type->info.data_type.dec_precision
	   != DB_DEFAULT_NUMERIC_SCALE))
	{
	  precision++;		/* for decimal point */
	}
      break;
    case PT_TYPE_CHAR:
      precision = node->data_type->info.data_type.precision;
      if (precision == DB_DEFAULT_PRECISION)
	{
	  precision = DB_MAX_CHAR_PRECISION;
	}
      break;
    case PT_TYPE_VARCHAR:
      precision = node->data_type->info.data_type.precision;
      if (precision == DB_DEFAULT_PRECISION)
	{
	  precision = DB_MAX_VARCHAR_PRECISION;
	}
      break;
    case PT_TYPE_NCHAR:
      precision = node->data_type->info.data_type.precision;
      if (precision == DB_DEFAULT_PRECISION)
	{
	  precision = DB_MAX_NCHAR_PRECISION;
	}
      break;
    case PT_TYPE_VARNCHAR:
      precision = node->data_type->info.data_type.precision;
      if (precision == DB_DEFAULT_PRECISION)
	{
	  precision = DB_MAX_VARNCHAR_PRECISION;
	}
      break;
    case PT_TYPE_NULL:
    case PT_TYPE_NA:
      precision = 0;
      break;

    default:
      /* for host vars */
      switch (coerce_type)
	{
	case PT_TYPE_VARCHAR:
	  precision = DB_MAX_VARCHAR_PRECISION;
	  break;
	case PT_TYPE_VARNCHAR:
	  precision = DB_MAX_VARNCHAR_PRECISION;
	  break;
	default:
	  precision = TP_FLOATING_PRECISION_VALUE;
	  break;
	}
      break;
    }

  return precision;
}

/*
 * pt_eval_function_type () -
 *   return: returns a node of the same type.
 *   parser(in): parser global context info for reentrancy
 *   node(in): a parse tree node of type PT_FUNCTION denoting an
 *             an expression with aggregate functions.
 */

static PT_NODE *
pt_eval_function_type (PARSER_CONTEXT * parser, PT_NODE * node)
{
  PT_NODE *arg_list;
  PT_TYPE_ENUM arg_type;
  FUNC_TYPE fcode;
  bool check_agg_single_arg = false;
  bool is_agg_function = false;
  PT_NODE *prev = NULL;
  PT_NODE *arg = NULL;
  is_agg_function = pt_is_aggregate_function (parser, node);
  arg_list = node->info.function.arg_list;
  fcode = node->info.function.function_type;
  if (!arg_list && fcode != PT_COUNT_STAR && fcode != PT_GROUPBY_NUM
      && fcode != PT_ROW_NUMBER && fcode != PT_RANK && fcode != PT_DENSE_RANK)
    {
      PT_ERRORmf (parser, node, MSGCAT_SET_PARSER_SEMANTIC,
		  MSGCAT_SEMANTIC_FUNCTION_NO_ARGS,
		  pt_short_print (parser, node));
      return node;
    }

  /* to avoid "node->next" ambiguities, wrap any logical node within the arg
   * list with a cast to integer. This way, the CNF trees do not mix up with
   * the arg list.
   */
  for (arg = arg_list; arg != NULL; prev = arg, arg = arg->next)
    {
      if (arg->type_enum == PT_TYPE_LOGICAL)
	{
	  arg =
	    pt_wrap_with_cast_op (parser, arg, PT_TYPE_INTEGER, 0, 0, NULL);
	  if (arg == NULL)
	    {
	      /* the error message is set by pt_wrap_with_cast_op */
	      node->type_enum = PT_TYPE_NONE;
	      return node;
	    }
	  if (prev != NULL)
	    {
	      prev->next = arg;
	    }
	  else
	    {
	      node->info.function.arg_list = arg_list = arg;
	    }
	}
    }

  if (pt_list_has_logical_nodes (arg_list))
    {
      PT_ERRORmf2 (parser, node, MSGCAT_SET_PARSER_SEMANTIC,
		   MSGCAT_SEMANTIC_FUNC_NOT_DEFINED_ON,
		   pt_show_function (fcode), "boolean");
      return node;
    }

  /*
   * Should only get one arg to function; set to 0 if the function
   * accepts more than one.
   */
  check_agg_single_arg = true;
  arg_type = (arg_list) ? arg_list->type_enum : PT_TYPE_NONE;

  switch (fcode)
    {
    case PT_STDDEV:
    case PT_STDDEV_POP:
    case PT_STDDEV_SAMP:
    case PT_VARIANCE:
    case PT_VAR_POP:
    case PT_VAR_SAMP:
    case PT_AVG:
      if (arg_type != PT_TYPE_MAYBE && arg_type != PT_TYPE_NULL
	  && arg_type != PT_TYPE_NA)
	{
	  if (!PT_IS_NUMERIC_TYPE (arg_type))
	    {
	      arg_list = pt_wrap_with_cast_op (parser, arg_list,
					       PT_TYPE_DOUBLE,
					       TP_FLOATING_PRECISION_VALUE,
					       0, NULL);
	      if (arg_list == NULL)
		{
		  return node;
		}
	    }
	  node->info.function.arg_list = arg_list;
	}

      arg_type = PT_TYPE_DOUBLE;
      break;

    case PT_AGG_BIT_AND:
    case PT_AGG_BIT_OR:
    case PT_AGG_BIT_XOR:
      if (!PT_IS_DISCRETE_NUMBER_TYPE (arg_type)
	  && arg_type != PT_TYPE_MAYBE
	  && arg_type != PT_TYPE_NULL && arg_type != PT_TYPE_NA)
	{
	  /* cast arg_list to bigint */
	  arg_list = pt_wrap_with_cast_op (parser, arg_list, PT_TYPE_BIGINT,
					   0, 0, NULL);
	  if (arg_list == NULL)
	    {
	      return node;
	    }
	  arg_type = PT_TYPE_BIGINT;
	  node->info.function.arg_list = arg_list;
	}
      break;

    case PT_SUM:
      if (!PT_IS_NUMERIC_TYPE (arg_type)
	  && arg_type != PT_TYPE_MAYBE
	  && arg_type != PT_TYPE_NULL
	  && arg_type != PT_TYPE_NA && !pt_is_set_type (arg_list))
	{
	  /* To display the sum as integer and not scientific */
	  PT_TYPE_ENUM cast_type =
	    (arg_type ==
	     PT_TYPE_ENUMERATION ? PT_TYPE_INTEGER : PT_TYPE_DOUBLE);

	  /* cast arg_list to double or integer */
	  arg_list =
	    pt_wrap_with_cast_op (parser, arg_list, cast_type, 0, 0, NULL);
	  if (arg_list == NULL)
	    {
	      return node;
	    }
	  arg_type = cast_type;
	  node->info.function.arg_list = arg_list;
	}
      break;

    case PT_MAX:
    case PT_MIN:
      if (!PT_IS_NUMERIC_TYPE (arg_type)
	  && !PT_IS_STRING_TYPE (arg_type)
	  && !PT_IS_DATE_TIME_TYPE (arg_type)
	  && arg_type != PT_TYPE_ENUMERATION
	  && arg_type != PT_TYPE_MAYBE
	  && arg_type != PT_TYPE_NULL && arg_type != PT_TYPE_NA)
	{
	  PT_ERRORmf2 (parser, node, MSGCAT_SET_PARSER_SEMANTIC,
		       MSGCAT_SEMANTIC_INCOMPATIBLE_OPDS,
		       pt_show_function (fcode),
		       pt_show_type_enum (arg_type));
	}
      break;

    case PT_LEAD:
    case PT_LAG:
    case PT_COUNT:
      break;

    case PT_GROUP_CONCAT:
      {
	PT_TYPE_ENUM sep_type;
	sep_type = (arg_list->next) ? arg_list->next->type_enum :
	  PT_TYPE_NONE;
	check_agg_single_arg = false;

	if (!PT_IS_NUMERIC_TYPE (arg_type)
	    && !PT_IS_STRING_TYPE (arg_type)
	    && !PT_IS_DATE_TIME_TYPE (arg_type)
	    && arg_type != PT_TYPE_ENUMERATION
	    && arg_type != PT_TYPE_MAYBE
	    && arg_type != PT_TYPE_NULL && arg_type != PT_TYPE_NA)
	  {
	    PT_ERRORmf2 (parser, node, MSGCAT_SET_PARSER_SEMANTIC,
			 MSGCAT_SEMANTIC_INCOMPATIBLE_OPDS,
			 pt_show_function (fcode),
			 pt_show_type_enum (arg_type));
	    break;
	  }

	if (!PT_IS_STRING_TYPE (sep_type) && sep_type != PT_TYPE_NONE)
	  {
	    PT_ERRORmf2 (parser, node, MSGCAT_SET_PARSER_SEMANTIC,
			 MSGCAT_SEMANTIC_INCOMPATIBLE_OPDS,
			 pt_show_function (fcode),
			 pt_show_type_enum (sep_type));
	    break;
	  }

	if ((sep_type == PT_TYPE_NCHAR || sep_type == PT_TYPE_VARNCHAR)
	    && arg_type != PT_TYPE_NCHAR && arg_type != PT_TYPE_VARNCHAR)
	  {
	    PT_ERRORmf3 (parser, node, MSGCAT_SET_PARSER_SEMANTIC,
			 MSGCAT_SEMANTIC_OP_NOT_DEFINED_ON,
			 pt_show_function (fcode),
			 pt_show_type_enum (arg_type),
			 pt_show_type_enum (sep_type));
	    break;
	  }

	if ((arg_type == PT_TYPE_NCHAR || arg_type == PT_TYPE_VARNCHAR)
	    && sep_type != PT_TYPE_NCHAR && sep_type != PT_TYPE_VARNCHAR
	    && sep_type != PT_TYPE_NONE)
	  {
	    PT_ERRORmf3 (parser, node, MSGCAT_SET_PARSER_SEMANTIC,
			 MSGCAT_SEMANTIC_OP_NOT_DEFINED_ON,
			 pt_show_function (fcode),
			 pt_show_type_enum (arg_type),
			 pt_show_type_enum (sep_type));
	    break;
	  }

	if ((arg_type == PT_TYPE_BIT || arg_type == PT_TYPE_VARBIT)
	    && sep_type != PT_TYPE_BIT && sep_type != PT_TYPE_VARBIT
	    && sep_type != PT_TYPE_NONE)
	  {
	    PT_ERRORmf3 (parser, node, MSGCAT_SET_PARSER_SEMANTIC,
			 MSGCAT_SEMANTIC_OP_NOT_DEFINED_ON,
			 pt_show_function (fcode),
			 pt_show_type_enum (arg_type),
			 pt_show_type_enum (sep_type));
	    break;
	  }
      }
      break;

    case PT_NTILE:
      if (!PT_IS_DISCRETE_NUMBER_TYPE (arg_type)
	  && arg_type != PT_TYPE_MAYBE
	  && arg_type != PT_TYPE_NULL && arg_type != PT_TYPE_NA)
	{
	  /* cast arg_list to double */
	  arg_list = pt_wrap_with_cast_op (parser, arg_list, PT_TYPE_DOUBLE,
					   0, 0, NULL);
	  if (arg_list == NULL)
	    {
	      return node;
	    }

	  arg_type = PT_TYPE_INTEGER;
	  node->info.function.arg_list = arg_list;
	}
      break;

    case F_ELT:
      {
	/* all types used in the arguments list */
	bool has_arg_type[PT_TYPE_MAX - PT_TYPE_MIN] = { false };

	/* a subset of argument types given to ELT that can not be cast to [N]CHAR VARYING */
	PT_TYPE_ENUM bad_types[4] = {
	  PT_TYPE_NONE, PT_TYPE_NONE, PT_TYPE_NONE, PT_TYPE_NONE
	};

	PT_NODE *arg = arg_list;

	size_t i = 0;		/* used to index has_arg_type */
	size_t num_bad = 0;	/* used to index bad_types */

	memset (has_arg_type, 0, sizeof (has_arg_type));

	/* check the index argument (null, numeric or host var) */
	if (PT_IS_NUMERIC_TYPE (arg->type_enum) ||
	    PT_IS_CHAR_STRING_TYPE (arg->type_enum) ||
	    arg->type_enum == PT_TYPE_NONE ||
	    arg->type_enum == PT_TYPE_NA ||
	    arg->type_enum == PT_TYPE_NULL || arg->type_enum == PT_TYPE_MAYBE)
	  {
	    arg = arg->next;
	  }
	else
	  {
	    arg_type = PT_TYPE_NONE;
	    PT_ERRORmf2 (parser, node, MSGCAT_SET_PARSER_SEMANTIC,
			 MSGCAT_SEMANTIC_FUNC_NOT_DEFINED_ON_INDEX,
			 pt_show_function (fcode),
			 pt_show_type_enum (arg->type_enum));
	    break;
	  }

	/* make a list of all other argument types (null, [N]CHAR [VARYING], or host var) */
	while (arg)
	  {
	    if (arg->type_enum < PT_TYPE_MAX)
	      {
		has_arg_type[arg->type_enum - PT_TYPE_MIN] = true;
		arg = arg->next;
	      }
	    else
	      {
		assert (false);	/* invalid data type */
		arg_type = PT_TYPE_NONE;
		PT_ERRORmf2 (parser, node, MSGCAT_SET_PARSER_SEMANTIC,
			     MSGCAT_SEMANTIC_FUNC_NOT_DEFINED_ON,
			     pt_show_function (fcode),
			     pt_show_type_enum (arg->type_enum));
		break;
	      }
	  }

	/* look for unsupported argument types in the list */
	while (i < (sizeof (has_arg_type) / sizeof (has_arg_type[0])))
	  {
	    if (has_arg_type[i])
	      {
		if (!(PT_IS_NUMERIC_TYPE (PT_TYPE_MIN + i) ||
		      PT_IS_CHAR_STRING_TYPE (PT_TYPE_MIN + i) ||
		      PT_IS_DATE_TIME_TYPE (PT_TYPE_MIN + i) ||
		      PT_TYPE_MIN + i == PT_TYPE_ENUMERATION ||
		      PT_TYPE_MIN + i == PT_TYPE_LOGICAL ||
		      PT_TYPE_MIN + i == PT_TYPE_NONE ||
		      PT_TYPE_MIN + i == PT_TYPE_NA ||
		      PT_TYPE_MIN + i == PT_TYPE_NULL ||
		      PT_TYPE_MIN + i == PT_TYPE_MAYBE))
		  {
		    /* type is not NULL, unknown and is not known coercible
		       to [N]CHAR VARYING */
		    size_t k = 0;

		    while (k < num_bad && bad_types[k] != PT_TYPE_MIN + i)
		      {
			k++;
		      }

		    if (k == num_bad)
		      {
			bad_types[num_bad++] = PT_TYPE_MIN + i;

			if (num_bad ==
			    sizeof (bad_types) / sizeof (bad_types[0]))
			  {
			    break;
			  }
		      }
		  }
	      }

	    i++;
	  }

	/* check string category (CHAR or NCHAR) for any string arguments */
	if ((num_bad < sizeof (bad_types) / sizeof (bad_types[0]) - 1)
	    && (has_arg_type[PT_TYPE_CHAR - PT_TYPE_MIN]
		|| has_arg_type[PT_TYPE_VARCHAR - PT_TYPE_MIN])
	    && (has_arg_type[PT_TYPE_NCHAR - PT_TYPE_MIN]
		|| has_arg_type[PT_TYPE_VARNCHAR - PT_TYPE_MIN]))
	  {
	    if (has_arg_type[PT_TYPE_CHAR - PT_TYPE_MIN])
	      {
		bad_types[num_bad++] = PT_TYPE_CHAR;
	      }
	    else
	      {
		bad_types[num_bad++] = PT_TYPE_VARCHAR;
	      }

	    if (has_arg_type[PT_TYPE_NCHAR - PT_TYPE_MIN])
	      {
		bad_types[num_bad++] = PT_TYPE_NCHAR;
	      }
	    else
	      {
		bad_types[num_bad++] = PT_TYPE_VARNCHAR;
	      }
	  }

	/* report any unsupported arguments */
	switch (num_bad)
	  {
	  case 1:
	    arg_type = PT_TYPE_NONE;
	    PT_ERRORmf2 (parser, node, MSGCAT_SET_PARSER_SEMANTIC,
			 MSGCAT_SEMANTIC_FUNC_NOT_DEFINED_ON,
			 pt_show_function (fcode),
			 pt_show_type_enum (bad_types[0]));
	    break;
	  case 2:
	    arg_type = PT_TYPE_NONE;
	    PT_ERRORmf3 (parser, node, MSGCAT_SET_PARSER_SEMANTIC,
			 MSGCAT_SEMANTIC_FUNC_NOT_DEFINED_ON_2,
			 pt_show_function (fcode),
			 pt_show_type_enum (bad_types[0]),
			 pt_show_type_enum (bad_types[1]));
	    break;
	  case 3:
	    arg_type = PT_TYPE_NONE;
	    PT_ERRORmf4 (parser, node, MSGCAT_SET_PARSER_SEMANTIC,
			 MSGCAT_SEMANTIC_FUNC_NOT_DEFINED_ON_3,
			 pt_show_function (fcode),
			 pt_show_type_enum (bad_types[0]),
			 pt_show_type_enum (bad_types[1]),
			 pt_show_type_enum (bad_types[2]));
	    break;
	  case 4:
	    arg_type = PT_TYPE_NONE;
	    PT_ERRORmf5 (parser, node, MSGCAT_SET_PARSER_SEMANTIC,
			 MSGCAT_SEMANTIC_FUNC_NOT_DEFINED_ON_4,
			 pt_show_function (fcode),
			 pt_show_type_enum (bad_types[0]),
			 pt_show_type_enum (bad_types[1]),
			 pt_show_type_enum (bad_types[2]),
			 pt_show_type_enum (bad_types[3]));
	    break;
	  }
      }
      break;

    case F_INSERT_SUBSTRING:
      {
	PT_TYPE_ENUM arg1_type = PT_TYPE_NONE, arg2_type = PT_TYPE_NONE,
	  arg3_type = PT_TYPE_NONE, arg4_type = PT_TYPE_NONE;
	PT_NODE *arg_array[NUM_F_INSERT_SUBSTRING_ARGS];
	int num_args = 0;
	/* arg_list to array */
	if (pt_node_list_to_array (parser, arg_list, arg_array,
				   NUM_F_INSERT_SUBSTRING_ARGS,
				   &num_args) != NO_ERROR)
	  {
	    break;
	  }
	if (num_args != NUM_F_INSERT_SUBSTRING_ARGS)
	  {
	    assert (false);
	    break;
	  }

	arg1_type = arg_array[0]->type_enum;
	arg2_type = arg_array[1]->type_enum;
	arg3_type = arg_array[2]->type_enum;
	arg4_type = arg_array[3]->type_enum;
	/* check arg2 and arg3 */
	if (!PT_IS_NUMERIC_TYPE (arg2_type)
	    && !PT_IS_CHAR_STRING_TYPE (arg2_type)
	    && arg2_type != PT_TYPE_MAYBE
	    && arg2_type != PT_TYPE_NULL && arg2_type != PT_TYPE_NA)
	  {
	    arg_type = PT_TYPE_NONE;
	    PT_ERRORmf5 (parser, node, MSGCAT_SET_PARSER_SEMANTIC,
			 MSGCAT_SEMANTIC_FUNC_NOT_DEFINED_ON_4,
			 pt_show_function (fcode),
			 pt_show_type_enum (arg1_type),
			 pt_show_type_enum (arg2_type),
			 pt_show_type_enum (arg3_type),
			 pt_show_type_enum (arg4_type));
	    break;
	  }

	if (!PT_IS_NUMERIC_TYPE (arg3_type)
	    && !PT_IS_CHAR_STRING_TYPE (arg3_type)
	    && arg3_type != PT_TYPE_MAYBE
	    && arg3_type != PT_TYPE_NULL && arg3_type != PT_TYPE_NA)
	  {
	    arg_type = PT_TYPE_NONE;
	    PT_ERRORmf5 (parser, node, MSGCAT_SET_PARSER_SEMANTIC,
			 MSGCAT_SEMANTIC_FUNC_NOT_DEFINED_ON_4,
			 pt_show_function (fcode),
			 pt_show_type_enum (arg1_type),
			 pt_show_type_enum (arg2_type),
			 pt_show_type_enum (arg3_type),
			 pt_show_type_enum (arg4_type));
	    break;
	  }

	/* check arg1 */
	if (!PT_IS_NUMERIC_TYPE (arg1_type)
	    && !PT_IS_STRING_TYPE (arg1_type)
	    && !PT_IS_DATE_TIME_TYPE (arg1_type)
	    && arg1_type != PT_TYPE_ENUMERATION
	    && arg1_type != PT_TYPE_MAYBE
	    && arg1_type != PT_TYPE_NULL && arg1_type != PT_TYPE_NA)
	  {
	    arg_type = PT_TYPE_NONE;
	    PT_ERRORmf5 (parser, node, MSGCAT_SET_PARSER_SEMANTIC,
			 MSGCAT_SEMANTIC_FUNC_NOT_DEFINED_ON_4,
			 pt_show_function (fcode),
			 pt_show_type_enum (arg1_type),
			 pt_show_type_enum (arg2_type),
			 pt_show_type_enum (arg3_type),
			 pt_show_type_enum (arg4_type));
	    break;
	  }
	/* check arg4 */
	if (!PT_IS_NUMERIC_TYPE (arg4_type)
	    && !PT_IS_STRING_TYPE (arg4_type)
	    && !PT_IS_DATE_TIME_TYPE (arg4_type)
	    && arg4_type != PT_TYPE_MAYBE
	    && arg4_type != PT_TYPE_NULL && arg4_type != PT_TYPE_NA)
	  {
	    arg_type = PT_TYPE_NONE;
	    PT_ERRORmf5 (parser, node, MSGCAT_SET_PARSER_SEMANTIC,
			 MSGCAT_SEMANTIC_FUNC_NOT_DEFINED_ON_4,
			 pt_show_function (fcode),
			 pt_show_type_enum (arg1_type),
			 pt_show_type_enum (arg2_type),
			 pt_show_type_enum (arg3_type),
			 pt_show_type_enum (arg4_type));
	    break;
	  }
      }
      break;

    default:
      check_agg_single_arg = false;
      break;
    }

  if (is_agg_function && check_agg_single_arg)
    {
      if (arg_list->next)
	{
	  PT_ERRORmf (parser, node, MSGCAT_SET_PARSER_SEMANTIC,
		      MSGCAT_SEMANTIC_AGG_FUN_WANT_1_ARG,
		      pt_short_print (parser, node));
	}
      else
	{
	  /* do special constant folding;
	   *   COUNT(1), COUNT(?), COUNT(:x), ... -> COUNT(*) */
	  /* TODO does this belong to type checking or constant folding? */
	  if (pt_is_const (arg_list))
	    {
	      PT_MISC_TYPE all_or_distinct;

	      all_or_distinct = node->info.function.all_or_distinct;
	      if (fcode == PT_COUNT && all_or_distinct != PT_DISTINCT)
		{
		  fcode = node->info.function.function_type = PT_COUNT_STAR;
		  parser_free_tree (parser, arg_list);
		  arg_list = node->info.function.arg_list = NULL;
		}
	    }
	}
    }

  if (node->type_enum == PT_TYPE_NONE || node->data_type == NULL)
    {
      /* determine function result type */
      switch (fcode)
	{
	case PT_COUNT:
	case PT_COUNT_STAR:
	case PT_ROW_NUMBER:
	case PT_RANK:
	case PT_DENSE_RANK:
	case PT_NTILE:
	  node->type_enum = PT_TYPE_INTEGER;
	  break;

	case PT_GROUPBY_NUM:
	  node->type_enum = PT_TYPE_BIGINT;
	  break;

	case PT_AGG_BIT_AND:
	case PT_AGG_BIT_OR:
	case PT_AGG_BIT_XOR:
	  node->type_enum = PT_TYPE_BIGINT;
	  break;

	case F_TABLE_SET:
	  node->type_enum = PT_TYPE_SET;
	  pt_add_type_to_set (parser,
			      pt_get_select_list (parser, arg_list),
			      &node->data_type);
	  break;

	case F_TABLE_MULTISET:
	  node->type_enum = PT_TYPE_MULTISET;
	  pt_add_type_to_set (parser,
			      pt_get_select_list (parser, arg_list),
			      &node->data_type);
	  break;

	case F_TABLE_SEQUENCE:
	  node->type_enum = PT_TYPE_SEQUENCE;
	  pt_add_type_to_set (parser,
			      pt_get_select_list (parser, arg_list),
			      &node->data_type);
	  break;

	case F_SET:
	  node->type_enum = PT_TYPE_SET;
	  pt_add_type_to_set (parser, arg_list, &node->data_type);
	  break;

	case F_MULTISET:
	  node->type_enum = PT_TYPE_MULTISET;
	  pt_add_type_to_set (parser, arg_list, &node->data_type);
	  break;

	case F_SEQUENCE:
	  node->type_enum = PT_TYPE_SEQUENCE;
	  pt_add_type_to_set (parser, arg_list, &node->data_type);
	  break;

	case PT_SUM:
	  node->type_enum = arg_type;
	  node->data_type = parser_copy_tree_list (parser,
						   arg_list->data_type);
	  if (arg_type == PT_TYPE_NUMERIC && node->data_type)
	    {
	      node->data_type->info.data_type.precision =
		DB_MAX_NUMERIC_PRECISION;
	    }
	  break;

	case PT_AVG:
	case PT_STDDEV:
	case PT_STDDEV_POP:
	case PT_STDDEV_SAMP:
	case PT_VARIANCE:
	case PT_VAR_POP:
	case PT_VAR_SAMP:
	  node->type_enum = arg_type;
	  node->data_type = NULL;

	  break;

	case PT_GROUP_CONCAT:
	  {
	    if (arg_type == PT_TYPE_NCHAR || arg_type == PT_TYPE_VARNCHAR)
	      {
		node->type_enum = PT_TYPE_VARNCHAR;
		node->data_type = pt_make_prim_data_type (parser,
							  PT_TYPE_VARNCHAR);
		if (node->data_type == NULL)
		  {
		    assert (false);
		  }
		node->data_type->info.data_type.precision =
		  TP_FLOATING_PRECISION_VALUE;
	      }
	    else if (arg_type == PT_TYPE_BIT || arg_type == PT_TYPE_VARBIT)
	      {
		node->type_enum = PT_TYPE_VARBIT;
		node->data_type = pt_make_prim_data_type (parser,
							  PT_TYPE_VARBIT);
		if (node->data_type == NULL)
		  {
		    node->type_enum = PT_TYPE_NONE;
		    assert (false);
		  }
		node->data_type->info.data_type.precision =
		  TP_FLOATING_PRECISION_VALUE;
	      }
	    else
	      {
		node->type_enum = PT_TYPE_VARCHAR;
		node->data_type = pt_make_prim_data_type (parser,
							  PT_TYPE_VARCHAR);
		if (node->data_type == NULL)
		  {
		    node->type_enum = PT_TYPE_NONE;
		    assert (false);
		  }
		node->data_type->info.data_type.precision =
		  TP_FLOATING_PRECISION_VALUE;
	      }
	  }
	  break;

	case F_INSERT_SUBSTRING:
	  {
	    PT_NODE *new_att = NULL;
	    PT_TYPE_ENUM arg1_type = PT_TYPE_NONE, arg2_type = PT_TYPE_NONE,
	      arg3_type = PT_TYPE_NONE, arg4_type = PT_TYPE_NONE;
	    PT_TYPE_ENUM arg1_orig_type, arg2_orig_type, arg3_orig_type,
	      arg4_orig_type;
	    PT_NODE *arg_array[NUM_F_INSERT_SUBSTRING_ARGS];
	    int num_args;

	    /* arg_list to array */
	    if (pt_node_list_to_array (parser, arg_list, arg_array,
				       NUM_F_INSERT_SUBSTRING_ARGS,
				       &num_args) != NO_ERROR)
	      {
		break;
	      }
	    if (num_args != NUM_F_INSERT_SUBSTRING_ARGS)
	      {
		assert (false);
		break;
	      }

	    arg1_orig_type = arg1_type = arg_array[0]->type_enum;
	    arg2_orig_type = arg2_type = arg_array[1]->type_enum;
	    arg3_orig_type = arg3_type = arg_array[2]->type_enum;
	    arg4_orig_type = arg4_type = arg_array[3]->type_enum;
	    arg_type = PT_TYPE_NONE;

	    /* validate and/or convert arguments */
	    /* arg1 should be VAR-str, but compatible with arg4
	     * (except when arg4 is BIT - no casting to bit on arg1) */
	    if (!(PT_IS_STRING_TYPE (arg1_type)))
	      {
		PT_TYPE_ENUM upgraded_type = PT_TYPE_NONE;
		if (arg4_type == PT_TYPE_NCHAR)
		  {
		    upgraded_type = PT_TYPE_VARNCHAR;
		  }
		else
		  {
		    upgraded_type = PT_TYPE_VARCHAR;
		  }

		new_att = pt_wrap_with_cast_op (parser, arg_array[0],
						upgraded_type,
						TP_FLOATING_PRECISION_VALUE,
						0, NULL);
		if (new_att == NULL)
		  {
		    break;
		  }
		node->info.function.arg_list = arg_array[0] = new_att;
		arg_type = arg1_type = upgraded_type;
	      }
	    else
	      {
		arg_type = arg1_type;
	      }


	    if (arg2_type != PT_TYPE_INTEGER)
	      {
		new_att = pt_wrap_with_cast_op (parser, arg_array[1],
						PT_TYPE_INTEGER,
						TP_FLOATING_PRECISION_VALUE,
						0, NULL);
		if (new_att == NULL)
		  {
		    break;
		  }
		arg_array[0]->next = arg_array[1] = new_att;
		arg2_type = PT_TYPE_INTEGER;

	      }

	    if (arg3_type != PT_TYPE_INTEGER)
	      {
		new_att = pt_wrap_with_cast_op (parser, arg_array[2],
						PT_TYPE_INTEGER,
						TP_FLOATING_PRECISION_VALUE,
						0, NULL);
		if (new_att == NULL)
		  {
		    break;
		  }
		arg_array[1]->next = arg_array[2] = new_att;
		arg3_type = PT_TYPE_INTEGER;
	      }

	    /* set result type and precision */
	    if (arg_type == PT_TYPE_NCHAR || arg_type == PT_TYPE_VARNCHAR)
	      {
		node->type_enum = PT_TYPE_VARNCHAR;
		node->data_type = pt_make_prim_data_type (parser,
							  PT_TYPE_VARNCHAR);
		node->data_type->info.data_type.precision =
		  TP_FLOATING_PRECISION_VALUE;
	      }
	    else if (arg_type == PT_TYPE_BIT || arg_type == PT_TYPE_VARBIT)
	      {
		node->type_enum = PT_TYPE_VARBIT;
		node->data_type = pt_make_prim_data_type (parser,
							  PT_TYPE_VARBIT);
		node->data_type->info.data_type.precision =
		  TP_FLOATING_PRECISION_VALUE;
	      }
	    else
	      {
		arg_type = node->type_enum = PT_TYPE_VARCHAR;
		node->data_type = pt_make_prim_data_type (parser,
							  PT_TYPE_VARCHAR);
		node->data_type->info.data_type.precision =
		  TP_FLOATING_PRECISION_VALUE;
	      }
	    /* validate and/or set arg4 */
	    if (!(PT_IS_STRING_TYPE (arg4_type)))
	      {
		new_att = pt_wrap_with_cast_op (parser, arg_array[3],
						arg_type,
						TP_FLOATING_PRECISION_VALUE,
						0, NULL);
		if (new_att == NULL)
		  {
		    break;
		  }
		arg_array[2]->next = arg_array[3] = new_att;
	      }
	    /* final check of arg and arg4 type matching */
	    if ((arg4_type == PT_TYPE_VARNCHAR || arg4_type == PT_TYPE_NCHAR)
		&& (arg_type == PT_TYPE_VARCHAR || arg_type == PT_TYPE_CHAR))
	      {
		arg_type = PT_TYPE_NONE;
		PT_ERRORmf5 (parser, node, MSGCAT_SET_PARSER_SEMANTIC,
			     MSGCAT_SEMANTIC_FUNC_NOT_DEFINED_ON_4,
			     pt_show_function (fcode),
			     pt_show_type_enum (arg1_orig_type),
			     pt_show_type_enum (arg2_orig_type),
			     pt_show_type_enum (arg3_orig_type),
			     pt_show_type_enum (arg4_orig_type));
	      }
	    else if ((arg_type == PT_TYPE_VARNCHAR
		      || arg_type == PT_TYPE_NCHAR)
		     && (arg4_type == PT_TYPE_VARCHAR
			 || arg4_type == PT_TYPE_CHAR))
	      {
		arg_type = PT_TYPE_NONE;
		PT_ERRORmf5 (parser, node, MSGCAT_SET_PARSER_SEMANTIC,
			     MSGCAT_SEMANTIC_FUNC_NOT_DEFINED_ON_4,
			     pt_show_function (fcode),
			     pt_show_type_enum (arg1_orig_type),
			     pt_show_type_enum (arg2_orig_type),
			     pt_show_type_enum (arg3_orig_type),
			     pt_show_type_enum (arg4_orig_type));
	      }
	    else if ((arg_type == PT_TYPE_VARBIT
		      || arg_type == PT_TYPE_BIT)
		     && (arg4_type != PT_TYPE_VARBIT
			 && arg4_type != PT_TYPE_BIT))
	      {
		arg_type = PT_TYPE_NONE;
		PT_ERRORmf5 (parser, node, MSGCAT_SET_PARSER_SEMANTIC,
			     MSGCAT_SEMANTIC_FUNC_NOT_DEFINED_ON_4,
			     pt_show_function (fcode),
			     pt_show_type_enum (arg1_orig_type),
			     pt_show_type_enum (arg2_orig_type),
			     pt_show_type_enum (arg3_orig_type),
			     pt_show_type_enum (arg4_orig_type));
	      }
	    else if ((arg4_type == PT_TYPE_VARBIT
		      || arg4_type == PT_TYPE_BIT)
		     && (arg_type != PT_TYPE_VARBIT
			 && arg_type != PT_TYPE_BIT))
	      {
		arg_type = PT_TYPE_NONE;
		PT_ERRORmf5 (parser, node, MSGCAT_SET_PARSER_SEMANTIC,
			     MSGCAT_SEMANTIC_FUNC_NOT_DEFINED_ON_4,
			     pt_show_function (fcode),
			     pt_show_type_enum (arg1_orig_type),
			     pt_show_type_enum (arg2_orig_type),
			     pt_show_type_enum (arg3_orig_type),
			     pt_show_type_enum (arg4_orig_type));
	      }
	  }
	  break;

	case F_ELT:
	  {
	    PT_NODE *new_att = NULL;
	    PT_NODE *arg = arg_list, *prev_arg = arg_list;
	    int max_precision = 0;

	    /* check and cast the type for the index argument. */
	    if (!PT_IS_DISCRETE_NUMBER_TYPE (arg->type_enum))
	      {
		new_att =
		  pt_wrap_with_cast_op (parser, arg, PT_TYPE_BIGINT,
					TP_FLOATING_PRECISION_VALUE, 0, NULL);

		if (new_att)
		  {
		    prev_arg = arg_list = arg = new_att;
		    node->info.function.arg_list = arg_list;
		  }
		else
		  {
		    break;
		  }
	      }

	    /*
	       Look for the first argument of character string type and obtain its
	       category (CHAR/NCHAR).
	       All other arguments should be converted to this type, which is also
	       the return type.
	     */

	    arg_type = PT_TYPE_NONE;
	    arg = arg->next;

	    while (arg && arg_type == PT_TYPE_NONE)
	      {
		if (PT_IS_CHAR_STRING_TYPE (arg->type_enum))
		  {
		    if (arg->type_enum == PT_TYPE_CHAR
			|| arg->type_enum == PT_TYPE_VARCHAR)
		      {
			arg_type = PT_TYPE_VARCHAR;
		      }
		    else
		      {
			arg_type = PT_TYPE_VARNCHAR;
		      }
		  }
		else
		  {
		    arg = arg->next;
		  }
	      }

	    if (arg_type == PT_TYPE_NONE)
	      {
		/* no [N]CHAR [VARYING] argument passed; convert them all to VARCHAR */
		arg_type = PT_TYPE_VARCHAR;
	      }

	    /* take the maximum precision among all value arguments */
	    arg = arg_list->next;

	    while (arg)
	      {
		int precision = TP_FLOATING_PRECISION_VALUE;
		PT_NODE *new_att = NULL;

		precision = pt_character_length_for_node (arg, arg_type);
		if (max_precision != TP_FLOATING_PRECISION_VALUE)
		  {
		    if (precision == TP_FLOATING_PRECISION_VALUE ||
			max_precision < precision)
		      {
			max_precision = precision;
		      }
		  }

		arg = arg->next;
	      }

	    /* cast all arguments to [N]CHAR VARYING(max_precision)  */
	    arg = arg_list->next;
	    while (arg)
	      {
		if (arg->type_enum != arg_type ||
		    arg->data_type->info.data_type.precision != max_precision)
		  {
		    PT_NODE *new_attr =
		      pt_wrap_with_cast_op (parser, arg, arg_type,
					    max_precision, 0, NULL);

		    if (new_attr)
		      {
			prev_arg->next = arg = new_attr;
		      }
		    else
		      {
			break;
		      }
		  }

		arg = arg->next;
		prev_arg = prev_arg->next;
	      }

	    /* Return the selected data type and precision */

	    node->data_type = pt_make_prim_data_type (parser, arg_type);

	    if (node->data_type)
	      {
		node->type_enum = arg_type;
		node->data_type->info.data_type.precision = max_precision;
		node->data_type->info.data_type.dec_precision = 0;
	      }
	  }
	  break;

	default:
	  /* otherwise, f(x) has same type as x */
	  node->type_enum = arg_type;
	  node->data_type = parser_copy_tree_list (parser,
						   arg_list->data_type);
	  break;
	}
    }

  /* collation checking */
  arg_list = node->info.function.arg_list;
  switch (fcode)
    {
    case PT_GROUP_CONCAT:
      {
	INTL_CODESET arg1_cs = LANG_SYS_CODESET;
	int arg1_coll = LANG_SYS_COLLATION;
	PT_COLL_COERC_LEV arg1_coerc_level = PT_COLLATION_NOT_COERC;
	PT_NODE *new_node;
	PT_TYPE_ENUM sep_type;

	(void) pt_get_collation_info (arg_list, &arg1_coll, &arg1_cs,
				      &arg1_coerc_level);

	sep_type = (arg_list->next) ? arg_list->next->type_enum :
	  PT_TYPE_NONE;
	if (PT_HAS_COLLATION (sep_type))
	  {
	    assert (arg_list->next != NULL);

	    new_node = pt_coerce_node_collation (parser, arg_list->next,
						 arg1_coll, arg1_cs, false,
						 false);

	    if (new_node == NULL)
	      {
		goto error_collation;
	      }

	    arg_list->next = new_node;
	  }

	new_node = pt_coerce_node_collation (parser, node, arg1_coll,
					     arg1_cs, true, false);

	if (new_node == NULL)
	  {
	    goto error_collation;
	  }

	node = new_node;
      }
      break;

    case F_INSERT_SUBSTRING:
      {
	PT_TYPE_ENUM arg1_type = PT_TYPE_NONE, arg4_type = PT_TYPE_NONE;
	PT_NODE *arg_array[NUM_F_INSERT_SUBSTRING_ARGS];
	int num_args = 0;
	INTL_CODESET arg1_cs = LANG_SYS_CODESET;
	INTL_CODESET arg4_cs = LANG_SYS_CODESET;
	INTL_CODESET common_cs = LANG_SYS_CODESET;
	int arg1_coll = LANG_SYS_COLLATION;
	int arg4_coll = LANG_SYS_COLLATION;
	int common_coll = LANG_SYS_COLLATION;
	PT_COLL_COERC_LEV arg1_coerc_level = PT_COLLATION_NOT_COERC;
	PT_COLL_COERC_LEV arg4_coerc_level = PT_COLLATION_NOT_COERC;
	PT_NODE *new_node;
	int args_w_coll = 0;

	if (pt_node_list_to_array (parser, arg_list, arg_array,
				   NUM_F_INSERT_SUBSTRING_ARGS,
				   &num_args) != NO_ERROR)
	  {
	    break;
	  }

	if (num_args != NUM_F_INSERT_SUBSTRING_ARGS)
	  {
	    assert (false);
	    break;
	  }

	arg1_type = arg_array[0]->type_enum;
	arg4_type = arg_array[3]->type_enum;

	if (PT_HAS_COLLATION (arg1_type) || arg1_type == PT_TYPE_MAYBE)
	  {
	    if (pt_get_collation_info (arg_array[0], &arg1_coll, &arg1_cs,
				       &arg1_coerc_level))
	      {
		args_w_coll++;
	      }

	    if (arg1_type != PT_TYPE_MAYBE)
	      {
		common_coll = arg1_coll;
		common_cs = arg1_cs;
	      }
	  }

	if (PT_HAS_COLLATION (arg4_type) || arg4_type == PT_TYPE_MAYBE)
	  {
	    if (pt_get_collation_info (arg_array[3], &arg4_coll, &arg4_cs,
				       &arg4_coerc_level))
	      {
		args_w_coll++;
	      }

	    if (arg1_type != PT_TYPE_MAYBE)
	      {
		common_coll = arg4_coll;
		common_cs = arg4_cs;
	      }
	  }

	if (arg1_coll == arg4_coll)
	  {
	    assert (arg1_cs == arg4_cs);
	    break;
	  }

	if (pt_common_collation (arg1_coll, arg1_cs, arg1_coerc_level,
				 arg4_coll, arg4_cs, arg4_coerc_level,
				 0, 0, 0,
				 args_w_coll, false, &common_coll,
				 &common_cs) != 0)
	  {
	    goto error_collation;
	  }

	/* coerce collation of arguments */
	if ((common_coll != arg1_coll || common_cs != arg1_cs)
	    && (PT_HAS_COLLATION (arg1_type) || arg1_type == PT_TYPE_MAYBE))
	  {
	    new_node = pt_coerce_node_collation (parser, arg_array[0],
						 common_coll, common_cs,
						 false, false);
	    if (new_node == NULL)
	      {
		goto error_collation;
	      }

	    node->info.function.arg_list = arg_array[0] = new_node;
	  }

	/* coerce collation of arguments */
	if ((common_coll != arg4_coll || common_cs != arg4_cs)
	    && (PT_HAS_COLLATION (arg4_type) || arg4_type == PT_TYPE_MAYBE))
	  {
	    new_node = pt_coerce_node_collation (parser, arg_array[3],
						 common_coll, common_cs,
						 false, false);
	    if (new_node == NULL)
	      {
		goto error_collation;
	      }

	    arg_array[2]->next = arg_array[3] = new_node;
	  }

	new_node = pt_coerce_node_collation (parser, node, common_coll,
					     common_cs, true, false);
	if (new_node == NULL)
	  {
	    goto error_collation;
	  }

	node = new_node;
      }
      break;

    default:
      {
	PT_TYPE_ENUM arg_type = PT_TYPE_NONE, func_res_type = PT_TYPE_NONE;
	INTL_CODESET arg_cs = LANG_SYS_CODESET;
	INTL_CODESET func_cs = LANG_SYS_CODESET;
	int arg_coll = LANG_SYS_COLLATION, func_coll = LANG_SYS_COLLATION;
	PT_COLL_COERC_LEV arg_coerc_level = PT_COLLATION_NOT_COERC;
	PT_COLL_COERC_LEV func_coerc_level = PT_COLLATION_NOT_COERC;
	PT_NODE *new_node;

	arg_type = (arg_list != NULL) ? arg_list->type_enum : PT_TYPE_NONE;
	func_res_type = node->type_enum;

	if (!PT_HAS_COLLATION (arg_type) && !PT_IS_COLLECTION_TYPE (arg_type))
	  {
	    break;
	  }

	if (!PT_HAS_COLLATION (func_res_type)
	    && !PT_IS_COLLECTION_TYPE (func_res_type))
	  {
	    break;
	  }

	if (!pt_get_collation_info (arg_list, &arg_coll, &arg_cs,
				    &arg_coerc_level))
	  {
	    break;
	  }

	if (!pt_get_collation_info (node, &func_coll, &func_cs,
				    &func_coerc_level))
	  {
	    break;
	  }

	if (arg_coll == func_coll)
	  {
	    assert (arg_cs == func_cs);
	    break;
	  }

	new_node =
	  pt_coerce_node_collation (parser, node, arg_coll, arg_cs, true,
				    false);
	if (new_node == NULL)
	  {
	    goto error_collation;
	  }

	node = new_node;
      }
    }

  return node;

error_collation:
  PT_ERRORmf (parser, node, MSGCAT_SET_PARSER_SEMANTIC,
	      MSGCAT_SEMANTIC_COLLATION_OP_ERROR, pt_show_function (fcode));

  return node;
}

/*
 * pt_eval_method_call_type () -
 *   return: returns a node of the same type.
 *   parser(in): parser global context info for reentrancy
 *   node(in): a parse tree node of type PT_METHOD_CALL.
 */

static PT_NODE *
pt_eval_method_call_type (PARSER_CONTEXT * parser, PT_NODE * node)
{
  PT_NODE *method_name;
  PT_NODE *on_call_target;
  DB_OBJECT *obj = (DB_OBJECT *) 0;
  const char *name = (const char *) 0;
  DB_METHOD *method = (DB_METHOD *) 0;
  DB_DOMAIN *domain;
  DB_TYPE type;

  method_name = node->info.method_call.method_name;
  on_call_target = node->info.method_call.on_call_target;
  if (on_call_target == NULL)
    {
      return node;
    }

  if (method_name->node_type == PT_NAME)
    {
      name = method_name->info.name.original;
    }

  switch (on_call_target->node_type)
    {
    case PT_VALUE:
      obj = on_call_target->info.value.data_value.op;
      if (obj && name)
	{
	  if ((method = (DB_METHOD *) db_get_method (obj, name)) == NULL)
	    {
	      if (er_errid () == ER_OBJ_INVALID_METHOD)
		{
		  er_clear ();
		}

	      method = (DB_METHOD *) db_get_class_method (obj, name);
	    }
	}
      break;

    case PT_NAME:
      if (on_call_target->data_type
	  && on_call_target->data_type->info.data_type.entity)
	{
	  obj =
	    on_call_target->data_type->info.data_type.entity->info.
	    name.db_object;
	}

      if (obj && name)
	{
	  method = (DB_METHOD *) db_get_method (obj, name);
	  if (method == NULL)
	    {
	      if (er_errid () == ER_OBJ_INVALID_METHOD)
		{
		  er_clear ();
		}
	      method = (DB_METHOD *) db_get_class_method (obj, name);
	    }
	}
      break;

    default:
      break;
    }

  if (method)
    {
      domain = db_method_return_domain (method);
      if (domain)
	{
	  type = TP_DOMAIN_TYPE (domain);
	  node->type_enum = (PT_TYPE_ENUM) pt_db_to_type_enum (type);
	}
    }

  return node;
}

/*
 * pt_evaluate_db_value_expr () - apply op to db_value opds & place it in result
 *   return: 1 if evaluation succeeded, 0 otherwise
 *   parser(in): handle to the parser context
 *   expr(in): the expression to be applied
 *   op(in): a PT_OP_TYPE (the desired operation)
 *   arg1(in): 1st db_value operand
 *   arg2(in): 2nd db_value operand
 *   arg3(in): 3rd db_value operand
 *   result(out): a newly set db_value if successful, untouched otherwise
 *   domain(in): domain of result (for arithmetic & set ops)
 *   o1(in): a PT_NODE containing the source line & column position of arg1
 *   o2(in): a PT_NODE containing the source line & column position of arg2
 *   o3(in): a PT_NODE containing the source line & column position of arg3
 *   qualifier(in): trim qualifier or datetime component specifier
 */

int
pt_evaluate_db_value_expr (PARSER_CONTEXT * parser,
			   PT_NODE * expr,
			   PT_OP_TYPE op,
			   DB_VALUE * arg1,
			   DB_VALUE * arg2,
			   DB_VALUE * arg3,
			   DB_VALUE * result,
			   TP_DOMAIN * domain,
			   PT_NODE * o1, PT_NODE * o2,
			   PT_NODE * o3, PT_MISC_TYPE qualifier)
{
  DB_TYPE typ;
  DB_TYPE typ1, typ2;
  PT_TYPE_ENUM rTyp;
  int cmp;
  DB_VALUE_COMPARE_RESULT cmp_result;
  DB_VALUE_COMPARE_RESULT cmp_result2;
  DB_VALUE tmp_val;
  int error, i;
  DB_DATA_STATUS truncation;
  TP_DOMAIN_STATUS status;
  DB_TYPE res_type;

  assert (parser != NULL);

  if (!arg1 || !result)
    {
      return 0;
    }

  typ = TP_DOMAIN_TYPE (domain);
  rTyp = (PT_TYPE_ENUM) pt_db_to_type_enum (typ);

  /* do not coerce arg1, arg2 for STRCAT */
  if (op == PT_PLUS && PT_IS_STRING_TYPE (rTyp))
    {
      if (prm_get_bool_value (PRM_ID_PLUS_AS_CONCAT))
	{
	  op = PT_STRCAT;
	}
      else
	{
	  /* parameters should already be coerced to other types */
	  assert (false);
	}
    }

  typ1 = (arg1) ? DB_VALUE_TYPE (arg1) : DB_TYPE_NULL;
  typ2 = (arg2) ? DB_VALUE_TYPE (arg2) : DB_TYPE_NULL;
  cmp = 0;
  db_make_null (result);

  switch (op)
    {
    case PT_NOT:
      if (typ1 == DB_TYPE_NULL)
	{
	  db_make_null (result);	/* not NULL = NULL */
	}
      else if (DB_GET_INTEGER (arg1))
	{
	  db_make_int (result, false);	/* not true = false */
	}
      else
	{
	  db_make_int (result, true);	/* not false = true */
	}
      break;

    case PT_PRIOR:
    case PT_QPRIOR:
    case PT_CONNECT_BY_ROOT:
      if (db_value_clone (arg1, result) != NO_ERROR)
	{
	  return 0;
	}
      break;

    case PT_SYS_CONNECT_BY_PATH:
      return 0;

    case PT_BIT_NOT:
      switch (typ1)
	{
	case DB_TYPE_NULL:
	  db_make_null (result);
	  break;

	case DB_TYPE_INTEGER:
	  db_make_bigint (result, ~((DB_BIGINT) DB_GET_INTEGER (arg1)));
	  break;

	case DB_TYPE_BIGINT:
	  db_make_bigint (result, ~DB_GET_BIGINT (arg1));
	  break;

	case DB_TYPE_SHORT:
	  db_make_bigint (result, ~((DB_BIGINT) DB_GET_SHORT (arg1)));
	  break;

	default:
	  return 0;
	}
      break;

    case PT_BIT_AND:
      {
	DB_BIGINT bi[2];
	DB_TYPE dbtype[2];
	DB_VALUE *dbval[2];
	int i;

	dbtype[0] = typ1;
	dbtype[1] = typ2;
	dbval[0] = arg1;
	dbval[1] = arg2;

	for (i = 0; i < 2; i++)
	  {
	    switch (dbtype[i])
	      {
	      case DB_TYPE_NULL:
		db_make_null (result);
		break;

	      case DB_TYPE_INTEGER:
		bi[i] = (DB_BIGINT) DB_GET_INTEGER (dbval[i]);
		break;

	      case DB_TYPE_BIGINT:
		bi[i] = DB_GET_BIGINT (dbval[i]);
		break;

	      case DB_TYPE_SHORT:
		bi[i] = (DB_BIGINT) DB_GET_SHORT (dbval[i]);
		break;

	      default:
		return 0;
	      }
	  }

	if (dbtype[0] != DB_TYPE_NULL && dbtype[1] != DB_TYPE_NULL)
	  {
	    db_make_bigint (result, bi[0] & bi[1]);
	  }
      }
      break;

    case PT_BIT_OR:
      {
	DB_BIGINT bi[2];
	DB_TYPE dbtype[2];
	DB_VALUE *dbval[2];
	int i;

	dbtype[0] = typ1;
	dbtype[1] = typ2;
	dbval[0] = arg1;
	dbval[1] = arg2;

	for (i = 0; i < 2; i++)
	  {
	    switch (dbtype[i])
	      {
	      case DB_TYPE_NULL:
		db_make_null (result);
		break;

	      case DB_TYPE_INTEGER:
		bi[i] = (DB_BIGINT) DB_GET_INTEGER (dbval[i]);
		break;

	      case DB_TYPE_BIGINT:
		bi[i] = DB_GET_BIGINT (dbval[i]);
		break;

	      case DB_TYPE_SHORT:
		bi[i] = (DB_BIGINT) DB_GET_SHORT (dbval[i]);
		break;

	      default:
		return 0;
	      }
	  }

	if (dbtype[0] != DB_TYPE_NULL && dbtype[1] != DB_TYPE_NULL)
	  {
	    db_make_bigint (result, bi[0] | bi[1]);
	  }
      }
      break;

    case PT_BIT_XOR:
      {
	DB_BIGINT bi[2];
	DB_TYPE dbtype[2];
	DB_VALUE *dbval[2];
	int i;

	dbtype[0] = typ1;
	dbtype[1] = typ2;
	dbval[0] = arg1;
	dbval[1] = arg2;

	for (i = 0; i < 2; i++)
	  {
	    switch (dbtype[i])
	      {
	      case DB_TYPE_NULL:
		db_make_null (result);
		break;

	      case DB_TYPE_INTEGER:
		bi[i] = (DB_BIGINT) DB_GET_INTEGER (dbval[i]);
		break;

	      case DB_TYPE_BIGINT:
		bi[i] = DB_GET_BIGINT (dbval[i]);
		break;

	      case DB_TYPE_SHORT:
		bi[i] = (DB_BIGINT) DB_GET_SHORT (dbval[i]);
		break;

	      default:
		return 0;
	      }
	  }

	if (dbtype[0] != DB_TYPE_NULL && dbtype[1] != DB_TYPE_NULL)
	  {
	    db_make_bigint (result, bi[0] ^ bi[1]);
	  }
      }
      break;

    case PT_BITSHIFT_LEFT:
    case PT_BITSHIFT_RIGHT:
      {
	DB_BIGINT bi[2];
	DB_TYPE dbtype[2];
	DB_VALUE *dbval[2];
	int i;

	dbtype[0] = typ1;
	dbtype[1] = typ2;
	dbval[0] = arg1;
	dbval[1] = arg2;

	for (i = 0; i < 2; i++)
	  {
	    switch (dbtype[i])
	      {
	      case DB_TYPE_NULL:
		db_make_null (result);
		break;

	      case DB_TYPE_INTEGER:
		bi[i] = (DB_BIGINT) DB_GET_INTEGER (dbval[i]);
		break;

	      case DB_TYPE_BIGINT:
		bi[i] = DB_GET_BIGINT (dbval[i]);
		break;

	      case DB_TYPE_SHORT:
		bi[i] = (DB_BIGINT) DB_GET_SHORT (dbval[i]);
		break;

	      default:
		return 0;
	      }
	  }

	if (dbtype[0] != DB_TYPE_NULL && dbtype[1] != DB_TYPE_NULL)
	  {
	    if (bi[1] < (sizeof (DB_BIGINT) * 8) && bi[1] >= 0)
	      {
		if (op == PT_BITSHIFT_LEFT)
		  {
		    db_make_bigint (result,
				    ((UINT64) bi[0]) << ((UINT64) bi[1]));
		  }
		else
		  {
		    db_make_bigint (result,
				    ((UINT64) bi[0]) >> ((UINT64) bi[1]));
		  }
	      }
	    else
	      {
		db_make_bigint (result, 0);
	      }
	  }
      }
      break;

    case PT_DIV:
    case PT_MOD:
      {
	DB_BIGINT bi[2];
	DB_TYPE dbtype[2];
	DB_VALUE *dbval[2];
	int i;

	dbtype[0] = typ1;
	dbtype[1] = typ2;
	dbval[0] = arg1;
	dbval[1] = arg2;

	for (i = 0; i < 2; i++)
	  {
	    switch (dbtype[i])
	      {
	      case DB_TYPE_NULL:
		db_make_null (result);
		break;

	      case DB_TYPE_INTEGER:
		bi[i] = (DB_BIGINT) DB_GET_INTEGER (dbval[i]);
		break;

	      case DB_TYPE_BIGINT:
		bi[i] = DB_GET_BIGINT (dbval[i]);
		break;

	      case DB_TYPE_SHORT:
		bi[i] = (DB_BIGINT) DB_GET_SHORT (dbval[i]);
		break;

	      default:
		return 0;
	      }
	  }

	if (dbtype[0] != DB_TYPE_NULL && dbtype[1] != DB_TYPE_NULL)
	  {
	    if (bi[1] != 0)
	      {
		if (op == PT_DIV)
		  {
		    if (typ1 == DB_TYPE_INTEGER)
		      {
			if (OR_CHECK_INT_DIV_OVERFLOW (bi[0], bi[1]))
			  {
			    goto overflow;
			  }
			db_make_int (result, (INT32) (bi[0] / bi[1]));
		      }
		    else if (typ1 == DB_TYPE_BIGINT)
		      {
			if (OR_CHECK_BIGINT_DIV_OVERFLOW (bi[0], bi[1]))
			  {
			    goto overflow;
			  }
			db_make_bigint (result, bi[0] / bi[1]);
		      }
		    else
		      {
			if (OR_CHECK_SHORT_DIV_OVERFLOW (bi[0], bi[1]))
			  {
			    goto overflow;
			  }
			db_make_short (result, (INT16) (bi[0] / bi[1]));
		      }
		  }
		else
		  {
		    if (typ1 == DB_TYPE_INTEGER)
		      {
			db_make_int (result, (INT32) (bi[0] % bi[1]));
		      }
		    else if (typ1 == DB_TYPE_BIGINT)
		      {
			db_make_bigint (result, bi[0] % bi[1]);
		      }
		    else
		      {
			db_make_short (result, (INT16) (bi[0] % bi[1]));
		      }
		  }
	      }
	    else
	      {
		PT_ERRORm (parser, o2, MSGCAT_SET_PARSER_SEMANTIC,
			   MSGCAT_SEMANTIC_ZERO_DIVIDE);
		return 0;
	      }
	  }
      }
      break;

    case PT_IF:
      {				/* Obs: when this case occurs both args are the same type */
	if (DB_IS_NULL (arg1))
	  {
	    if (db_value_clone (arg3, result) != NO_ERROR)
	      {
		return 0;
	      }
	  }
	else
	  {
	    if (DB_GET_INTEGER (arg1))
	      {
		if (db_value_clone (arg2, result) != NO_ERROR)
		  {
		    return 0;
		  }
	      }
	    else
	      {
		if (db_value_clone (arg3, result) != NO_ERROR)
		  {
		    return 0;
		  }
	      }
	  }
      }
      break;

    case PT_IFNULL:
    case PT_COALESCE:
    case PT_NVL:
      {
	DB_VALUE *src;
	TP_DOMAIN *target_domain = NULL;
	bool need_free = false;
	PT_NODE *target_node;

	if (typ == DB_TYPE_VARIABLE)
	  {
	    TP_DOMAIN *arg1_domain, *arg2_domain;
	    TP_DOMAIN tmp_arg1_domain, tmp_arg2_domain;

	    arg1_domain = tp_domain_resolve_value (arg1, &tmp_arg1_domain);
	    arg2_domain = tp_domain_resolve_value (arg2, &tmp_arg2_domain);

	    target_domain = tp_infer_common_domain (arg1_domain,
						    arg2_domain, &need_free);
	  }
	else
	  {
	    target_domain = domain;
	  }

	if (typ1 == DB_TYPE_NULL)
	  {
	    src = arg2;
	    target_node = o2;
	  }
	else
	  {
	    src = arg1;
	    target_node = o1;
	  }

	if (tp_value_cast (src, result, target_domain,
			   false) != DOMAIN_COMPATIBLE)
	  {
	    if (need_free)
	      {
		tp_domain_free (target_domain);
	      }

	    rTyp =
	      (PT_TYPE_ENUM) pt_db_to_type_enum (target_domain->type->id);
	    PT_ERRORmf2 (parser, target_node, MSGCAT_SET_PARSER_SEMANTIC,
			 MSGCAT_SEMANTIC_CANT_COERCE_TO,
			 pt_short_print (parser, target_node),
			 pt_show_type_enum (rTyp));

	    return 0;
	  }

	if (need_free)
	  {
	    tp_domain_free (target_domain);
	  }
      }
      return 1;

    case PT_NVL2:
      {
	DB_VALUE *src;
	PT_NODE *target_node;
	TP_DOMAIN *target_domain = NULL;
	TP_DOMAIN *tmp_domain = NULL;
	bool need_free = false, need_tmp_domain_free = false;

	if (typ == DB_TYPE_VARIABLE)
	  {
	    TP_DOMAIN *arg1_domain, *arg2_domain, *arg3_domain;
	    TP_DOMAIN tmp_arg1_domain, tmp_arg2_domain, tmp_arg3_domain;

	    arg1_domain = tp_domain_resolve_value (arg1, &tmp_arg1_domain);
	    arg2_domain = tp_domain_resolve_value (arg2, &tmp_arg2_domain);
	    arg3_domain = tp_domain_resolve_value (arg3, &tmp_arg3_domain);

	    tmp_domain = tp_infer_common_domain (arg1_domain,
						 arg2_domain,
						 &need_tmp_domain_free);
	    target_domain = tp_infer_common_domain (tmp_domain,
						    arg3_domain, &need_free);
	  }
	else
	  {
	    target_domain = domain;
	  }

	if (typ1 == DB_TYPE_NULL)
	  {
	    src = arg3;
	    target_node = o3;
	  }
	else
	  {
	    src = arg2;
	    target_node = o2;
	  }

	if (tp_value_cast (src, result, target_domain,
			   false) != DOMAIN_COMPATIBLE)
	  {
	    if (need_free)
	      {
		tp_domain_free (target_domain);
	      }
	    if (need_tmp_domain_free)
	      {
		tp_domain_free (tmp_domain);
	      }

	    rTyp =
	      (PT_TYPE_ENUM) pt_db_to_type_enum (target_domain->type->id);
	    PT_ERRORmf2 (parser, target_node, MSGCAT_SET_PARSER_SEMANTIC,
			 MSGCAT_SEMANTIC_CANT_COERCE_TO,
			 pt_short_print (parser, target_node),
			 pt_show_type_enum (rTyp));
	    return 0;
	  }

	if (need_free)
	  {
	    tp_domain_free (target_domain);
	  }
	if (need_tmp_domain_free)
	  {
	    tp_domain_free (tmp_domain);
	  }
      }
      return 1;

    case PT_ISNULL:
      if (DB_IS_NULL (arg1))
	{
	  db_make_int (result, true);
	}
      else
	{
	  db_make_int (result, false);
	}
      break;

    case PT_UNARY_MINUS:
      switch (typ1)
	{
	case DB_TYPE_NULL:
	  db_make_null (result);	/* -NA = NA, -NULL = NULL */
	  break;

	case DB_TYPE_INTEGER:
	  if (DB_GET_INTEGER (arg1) == DB_INT32_MIN)
	    {
	      goto overflow;
	    }
	  else
	    {
	      db_make_int (result, -DB_GET_INTEGER (arg1));
	    }
	  break;

	case DB_TYPE_BIGINT:
	  if (DB_GET_BIGINT (arg1) == DB_BIGINT_MIN)
	    {
	      goto overflow;
	    }
	  else
	    {
	      db_make_bigint (result, -DB_GET_BIGINT (arg1));
	    }
	  break;

	case DB_TYPE_SHORT:
	  if (DB_GET_SHORT (arg1) == DB_INT16_MIN)
	    {
	      goto overflow;
	    }
	  else
	    {
	      db_make_short (result, -DB_GET_SHORT (arg1));
	    }
	  break;

	case DB_TYPE_FLOAT:
	  db_make_float (result, -DB_GET_FLOAT (arg1));
	  break;

	case DB_TYPE_DOUBLE:
	  db_make_double (result, -DB_GET_DOUBLE (arg1));
	  break;

	case DB_TYPE_NUMERIC:
	  if (numeric_db_value_negate (arg1) != NO_ERROR)
	    {
	      PT_ERRORc (parser, o1, er_msg ());
	      return 0;
	    }

	  db_make_numeric (result, DB_GET_NUMERIC (arg1),
			   DB_VALUE_PRECISION (arg1), DB_VALUE_SCALE (arg1));
	  break;

	case DB_TYPE_MONETARY:
	  DB_MAKE_MONETARY (result, -DB_GET_MONETARY (arg1)->amount);
	  break;

	case DB_TYPE_CHAR:
	case DB_TYPE_VARCHAR:
	case DB_TYPE_NCHAR:
	case DB_TYPE_VARNCHAR:
	  domain = tp_domain_resolve_default (DB_TYPE_DOUBLE);
	  db_make_null (&tmp_val);
	  /*force explicit cast ; scenario :
	   * INSERT INTO t VALUE(-?) , USING '10', column is INTEGER*/
	  if (tp_value_cast (arg1, &tmp_val, domain, false) !=
	      DOMAIN_COMPATIBLE)
	    {
	      if (prm_get_bool_value (PRM_ID_RETURN_NULL_ON_FUNCTION_ERRORS)
		  == false)
		{
		  PT_ERRORmf2 (parser, o1, MSGCAT_SET_PARSER_SEMANTIC,
			       MSGCAT_SEMANTIC_CANT_COERCE_TO,
			       parser_print_tree (parser, o1),
			       pt_show_type_enum (PT_TYPE_DOUBLE));
		  return 0;
		}
	      else
		{
		  db_make_null (result);
		  return 1;
		}
	    }
	  else
	    {
	      db_value_clear (arg1);
	      *arg1 = tmp_val;
	      db_make_double (result, -DB_GET_DOUBLE (arg1));
	    }
	  break;

	default:
	  return 0;		/* an unhandled type is a failure */
	}
      break;

    case PT_IS_NULL:
      if (typ1 == DB_TYPE_NULL)
	{
	  db_make_int (result, true);
	}
      else
	{
	  db_make_int (result, false);
	}
      break;

    case PT_IS_NOT_NULL:
      if (typ1 == DB_TYPE_NULL)
	{
	  db_make_int (result, false);
	}
      else
	{
	  db_make_int (result, true);
	}
      break;

    case PT_IS:
    case PT_IS_NOT:
      {
	int _true, _false;

	_true = (op == PT_IS) ? 1 : 0;
	_false = 1 - _true;

	if ((o1 && o1->node_type != PT_VALUE)
	    || (o2 && o2->node_type != PT_VALUE))
	  {
	    return 0;
	  }
	if (DB_IS_NULL (arg1))
	  {
	    if (DB_IS_NULL (arg2))
	      {
		db_make_int (result, _true);
	      }
	    else
	      {
		db_make_int (result, _false);
	      }
	  }
	else
	  {
	    if (DB_IS_NULL (arg2))
	      {
		db_make_int (result, _false);
	      }
	    else
	      {
		if (DB_GET_INTEGER (arg1) == DB_GET_INTEGER (arg2))
		  {
		    db_make_int (result, _true);
		  }
		else
		  {
		    db_make_int (result, _false);
		  }
	      }
	  }
      }
      break;

    case PT_TYPEOF:
      if (db_typeof_dbval (result, arg1) != NO_ERROR)
	{
	  db_make_null (result);
	}
      break;

    case PT_CONCAT_WS:
      if (DB_VALUE_TYPE (arg3) == DB_TYPE_NULL)
	{
	  db_make_null (result);
	  break;
	}
      /* no break here */
    case PT_CONCAT:
      if (typ1 == DB_TYPE_NULL || (typ2 == DB_TYPE_NULL && o2))
	{
	  bool check_empty_string;
	  check_empty_string =
	    (prm_get_bool_value (PRM_ID_ORACLE_STYLE_EMPTY_STRING)) ? true :
	    false;

	  if (!check_empty_string || !PT_IS_STRING_TYPE (rTyp))
	    {
	      if (op != PT_CONCAT_WS)
		{
		  db_make_null (result);
		  break;
		}
	    }
	}

      /* screen out cases we don't evaluate */
      if (!PT_IS_STRING_TYPE (rTyp))
	{
	  return 0;
	}

      switch (typ)
	{
	case DB_TYPE_CHAR:
	case DB_TYPE_NCHAR:
	case DB_TYPE_VARCHAR:
	case DB_TYPE_VARNCHAR:
	case DB_TYPE_BIT:
	case DB_TYPE_VARBIT:
	  if (o2)
	    {
	      if (op == PT_CONCAT_WS)
		{
		  if (typ1 == DB_TYPE_NULL)
		    {
		      if (db_value_clone (arg2, result) != NO_ERROR)
			{
			  return 0;
			}
		    }
		  else if (typ2 == DB_TYPE_NULL)
		    {
		      if (db_value_clone (arg1, result) != NO_ERROR)
			{
			  return 0;
			}
		    }
		  else
		    {
		      if (db_string_concatenate (arg1, arg3, &tmp_val,
						 &truncation) < 0
			  || truncation != DATA_STATUS_OK)
			{
			  PT_ERRORc (parser, o1, er_msg ());
			  return 0;
			}
		      if (db_string_concatenate (&tmp_val, arg2, result,
						 &truncation) < 0
			  || truncation != DATA_STATUS_OK)
			{
			  PT_ERRORc (parser, o1, er_msg ());
			  return 0;
			}
		    }
		}
	      else
		{
		  if (db_string_concatenate (arg1, arg2, result, &truncation)
		      < 0 || truncation != DATA_STATUS_OK)
		    {
		      PT_ERRORc (parser, o1, er_msg ());
		      return 0;
		    }
		}
	    }
	  else
	    {
	      if (db_value_clone (arg1, result) != NO_ERROR)
		{
		  return 0;
		}
	    }
	  break;

	default:
	  return 0;
	}

      break;

    case PT_FIELD:
      if (o1->node_type != PT_VALUE
	  || (o2 && o2->node_type != PT_VALUE) || o3->node_type != PT_VALUE)
	{
	  return 0;
	}

      if (DB_IS_NULL (arg3))
	{
	  db_make_int (result, 0);
	  break;
	}

      if (o3 && o3->next && o3->next->info.value.data_value.i == 1)
	{
	  if (tp_value_compare (arg3, arg1, 1, 0) == DB_EQ)
	    {
	      db_make_int (result, 1);
	    }
	  else if (tp_value_compare (arg3, arg2, 1, 0) == DB_EQ)
	    {
	      db_make_int (result, 2);
	    }
	  else
	    {
	      db_make_int (result, 0);
	    }
	}
      else
	{
	  i = db_get_int (arg1);
	  if (i > 0)
	    {
	      db_make_int (result, i);
	    }
	  else
	    {
	      if (tp_value_compare (arg3, arg2, 1, 0) == DB_EQ)
		{
		  if (o3 && o3->next)
		    {
		      db_make_int (result, o3->next->info.value.data_value.i);
		    }
		}
	      else
		{
		  db_make_int (result, 0);
		}
	    }
	}
      break;

    case PT_LEFT:
      if (!DB_IS_NULL (arg1) && !DB_IS_NULL (arg2))
	{
	  DB_VALUE tmp_val2;
	  if (tp_value_coerce (arg2, &tmp_val2, &tp_Integer_domain) !=
	      DOMAIN_COMPATIBLE)
	    {
	      PT_ERRORmf2 (parser, o1, MSGCAT_SET_PARSER_SEMANTIC,
			   MSGCAT_SEMANTIC_CANT_COERCE_TO,
			   parser_print_tree (parser, o2),
			   pt_show_type_enum (PT_TYPE_INTEGER));
	      return 0;
	    }

	  db_make_int (&tmp_val, 0);
	  error = db_string_substring (SUBSTRING, arg1, &tmp_val, &tmp_val2,
				       result);
	  if (error < 0)
	    {
	      PT_ERRORc (parser, o1, er_msg ());
	      return 0;
	    }
	  else
	    {
	      return 1;
	    }
	}
      else
	{
	  db_make_null (result);
	  return 1;
	}

    case PT_RIGHT:
      if (!DB_IS_NULL (arg1) && !DB_IS_NULL (arg2))
	{
	  DB_VALUE tmp_val2;

	  if (QSTR_IS_BIT (typ1))
	    {
	      if (db_string_bit_length (arg1, &tmp_val) != NO_ERROR)
		{
		  PT_ERRORc (parser, o1, er_msg ());
		  return 0;
		}
	    }
	  else
	    {
	      if (db_string_char_length (arg1, &tmp_val) != NO_ERROR)
		{
		  PT_ERRORc (parser, o1, er_msg ());
		  return 0;
		}
	    }
	  if (DB_IS_NULL (&tmp_val))
	    {
	      PT_ERRORc (parser, o1, er_msg ());
	      return 0;
	    }

	  if (tp_value_coerce (arg2, &tmp_val2, &tp_Integer_domain) !=
	      DOMAIN_COMPATIBLE)
	    {
	      PT_ERRORmf2 (parser, o1, MSGCAT_SET_PARSER_SEMANTIC,
			   MSGCAT_SEMANTIC_CANT_COERCE_TO,
			   parser_print_tree (parser, o2),
			   pt_show_type_enum (PT_TYPE_INTEGER));
	      return 0;
	    }

	  /* If len, defined as second argument, is negative value,
	   * RIGHT function returns the entire string.
	   * It's same behavior with LEFT and SUBSTRING.
	   */
	  if (db_get_int (&tmp_val2) < 0)
	    {
	      db_make_int (&tmp_val, 0);
	    }
	  else
	    {
	      db_make_int (&tmp_val,
			   db_get_int (&tmp_val) - db_get_int (&tmp_val2) +
			   1);
	    }
	  error = db_string_substring (SUBSTRING, arg1, &tmp_val, &tmp_val2,
				       result);
	  if (error < 0)
	    {
	      PT_ERRORc (parser, o1, er_msg ());
	      return 0;
	    }
	  else
	    {
	      return 1;
	    }
	}
      else
	{
	  db_make_null (result);
	  return 1;
	}

    case PT_REPEAT:
      {
	if (!DB_IS_NULL (arg1) && !DB_IS_NULL (arg2))
	  {
	    error = db_string_repeat (arg1, arg2, result);
	    if (error < 0)
	      {
		PT_ERRORc (parser, o1, er_msg ());
		return 0;
	      }
	    else
	      {
		return 1;
	      }
	  }
	else
	  {
	    db_make_null (result);
	    return 1;
	  }
      }
      /* break is not needed because of return(s) */
    case PT_SPACE:
      {
	if (DB_IS_NULL (arg1))
	  {
	    db_make_null (result);
	    return 1;
	  }
	else
	  {
	    error = db_string_space (arg1, result);
	    if (error < 0)
	      {
		PT_ERRORc (parser, o1, er_msg ());
		return 0;
	      }
	    else
	      {
		return 1;
	      }
	  }
      }
      break;

    case PT_LOCATE:
      if (DB_IS_NULL (arg1) || DB_IS_NULL (arg2) || (o3 && DB_IS_NULL (arg3)))
	{
	  db_make_null (result);
	}
      else
	{
	  if (!o3)
	    {
	      if (db_string_position (arg1, arg2, result) != NO_ERROR)
		{
		  PT_ERRORc (parser, o1, er_msg ());
		  return 0;
		}
	    }
	  else
	    {
	      DB_VALUE tmp_len, tmp_arg3;
	      int tmp = db_get_int (arg3);
	      if (tmp < 1)
		{
		  db_make_int (&tmp_arg3, 1);
		}
	      else
		{
		  db_make_int (&tmp_arg3, tmp);
		}

	      if (db_string_char_length (arg2, &tmp_len) != NO_ERROR)
		{
		  PT_ERRORc (parser, o2, er_msg ());
		  return 0;
		}
	      if (DB_IS_NULL (&tmp_len))
		{
		  PT_ERRORc (parser, o2, er_msg ());
		  return 0;
		}

	      db_make_int (&tmp_len,
			   db_get_int (&tmp_len) - db_get_int (&tmp_arg3) +
			   1);

	      if (db_string_substring (SUBSTRING, arg2, &tmp_arg3,
				       &tmp_len, &tmp_val) != NO_ERROR)
		{
		  PT_ERRORc (parser, o2, er_msg ());
		  return 0;
		}

	      if (db_string_position (arg1, &tmp_val, result) != NO_ERROR)
		{
		  PT_ERRORc (parser, o1, er_msg ());
		  return 0;
		}
	      if (db_get_int (result) > 0)
		{
		  db_make_int (result,
			       db_get_int (result) + db_get_int (&tmp_arg3) -
			       1);
		}
	    }
	}
      break;

    case PT_MID:
      if (DB_IS_NULL (arg1) || DB_IS_NULL (arg2) || DB_IS_NULL (arg3))
	{
	  db_make_null (result);
	}
      else
	{
	  DB_VALUE tmp_len, tmp_arg2, tmp_arg3;
	  int pos, len;

	  pos = db_get_int (arg2);
	  len = db_get_int (arg3);

	  if (pos < 0)
	    {
	      if (QSTR_IS_BIT (typ1))
		{
		  if (db_string_bit_length (arg1, &tmp_len) != NO_ERROR)
		    {
		      PT_ERRORc (parser, o1, er_msg ());
		      return 0;
		    }
		}
	      else
		{
		  if (db_string_char_length (arg1, &tmp_len) != NO_ERROR)
		    {
		      PT_ERRORc (parser, o1, er_msg ());
		      return 0;
		    }
		}
	      if (DB_IS_NULL (&tmp_len))
		{
		  PT_ERRORc (parser, o1, er_msg ());
		  return 0;
		}
	      pos = pos + db_get_int (&tmp_len) + 1;
	    }

	  if (pos < 1)
	    {
	      db_make_int (&tmp_arg2, 1);
	    }
	  else
	    {
	      db_make_int (&tmp_arg2, pos);
	    }

	  if (len < 1)
	    {
	      db_make_int (&tmp_arg3, 0);
	    }
	  else
	    {
	      db_make_int (&tmp_arg3, len);
	    }

	  error = db_string_substring (SUBSTRING, arg1, &tmp_arg2, &tmp_arg3,
				       result);
	  if (error < 0)
	    {
	      PT_ERRORc (parser, o1, er_msg ());
	      return 0;
	    }
	  else
	    {
	      return 1;
	    }
	}
      break;

    case PT_STRCMP:
      if (DB_IS_NULL (arg1) || DB_IS_NULL (arg2))
	{
	  db_make_null (result);
	}
      else
	{
	  if (db_string_compare (arg1, arg2, result) != NO_ERROR)
	    {
	      PT_ERRORc (parser, o1, er_msg ());
	      return 0;
	    }

	  cmp = db_get_int (result);
	  if (cmp < 0)
	    {
	      cmp = -1;
	    }
	  else if (cmp > 0)
	    {
	      cmp = 1;
	    }
	  db_make_int (result, cmp);
	}
      break;

    case PT_REVERSE:
      if (DB_IS_NULL (arg1))
	{
	  db_make_null (result);
	}
      else
	{
	  if (db_string_reverse (arg1, result) != NO_ERROR)
	    {
	      PT_ERRORc (parser, o1, er_msg ());
	      return 0;
	    }
	}
      break;

    case PT_BIT_COUNT:
      if (db_bit_count_dbval (result, arg1) != NO_ERROR)
	{
	  PT_ERRORc (parser, o1, er_msg ());
	  return 0;
	}
      break;

    case PT_EXISTS:
      if (TP_IS_SET_TYPE (typ1))
	{
	  if (db_set_size (db_get_set (arg1)) > 0)
	    {
	      db_make_int (result, true);
	    }
	  else
	    {
	      db_make_int (result, false);
	    }
	}
      else
	{
	  db_make_int (result, true);
	}
      break;

    case PT_AND:
      if ((typ1 == DB_TYPE_NULL && typ2 == DB_TYPE_NULL)
	  || (typ1 == DB_TYPE_NULL && DB_GET_INTEGER (arg2))
	  || (typ2 == DB_TYPE_NULL && DB_GET_INTEGER (arg1)))
	{
	  db_make_null (result);
	}
      else if (typ1 != DB_TYPE_NULL && DB_GET_INTEGER (arg1)
	       && typ2 != DB_TYPE_NULL && DB_GET_INTEGER (arg2))
	{
	  db_make_int (result, true);
	}
      else
	{
	  db_make_int (result, false);
	}
      break;

    case PT_OR:
      if ((typ1 == DB_TYPE_NULL && typ2 == DB_TYPE_NULL)
	  || (typ1 == DB_TYPE_NULL && !DB_GET_INTEGER (arg2))
	  || (typ2 == DB_TYPE_NULL && !DB_GET_INTEGER (arg1)))
	{
	  db_make_null (result);
	}
      else if (typ1 != DB_TYPE_NULL && !DB_GET_INTEGER (arg1)
	       && typ2 != DB_TYPE_NULL && !DB_GET_INTEGER (arg2))
	{
	  db_make_int (result, false);
	}
      else
	{
	  db_make_int (result, true);
	}
      break;

    case PT_XOR:
      if (typ1 == DB_TYPE_NULL || typ2 == DB_TYPE_NULL)
	{
	  db_make_null (result);
	}
      else if ((!DB_GET_INTEGER (arg1) && !DB_GET_INTEGER (arg2))
	       || (DB_GET_INTEGER (arg1) && DB_GET_INTEGER (arg2)))
	{
	  db_make_int (result, false);
	}
      else
	{
	  db_make_int (result, true);
	}
      break;

    case PT_PLUS:
    case PT_MINUS:
    case PT_TIMES:
    case PT_DIVIDE:
      if (typ == DB_TYPE_VARIABLE && pt_is_op_hv_late_bind (op))
	{
	  rTyp = pt_common_type (pt_db_to_type_enum (typ1),
				 pt_db_to_type_enum (typ2));
	  /* TODO: override common type for plus, minus
	   * this should be done in a more generic code : but,
	   * pt_common_type_op is expected to return TYPE_NONE in this case*/
	  if (PT_IS_CHAR_STRING_TYPE (rTyp) &&
	      (op != PT_PLUS
	       || prm_get_bool_value (PRM_ID_PLUS_AS_CONCAT) == false))
	    {
	      rTyp = PT_TYPE_DOUBLE;
	    }
	  typ = pt_type_enum_to_db (rTyp);
	  domain = tp_domain_resolve_default (typ);
	}

      if (typ1 == DB_TYPE_NULL || typ2 == DB_TYPE_NULL)
	{
	  bool check_empty_string;

	  check_empty_string =
	    ((prm_get_bool_value (PRM_ID_ORACLE_STYLE_EMPTY_STRING))
	     ? true : false);
	  if (!check_empty_string || op != PT_PLUS
	      || !PT_IS_STRING_TYPE (rTyp))
	    {
	      db_make_null (result);	/* NULL arith_op any = NULL */
	      break;
	    }
	}

      /* screen out cases we don't evaluate */
      if (!PT_IS_NUMERIC_TYPE (rTyp)
	  && !PT_IS_STRING_TYPE (rTyp)
	  && rTyp != PT_TYPE_SET
	  && rTyp != PT_TYPE_MULTISET
	  && rTyp != PT_TYPE_SEQUENCE && !PT_IS_DATE_TIME_TYPE (rTyp))
	{
	  return 0;
	}

      /* don't coerce dates and times */
      if (typ != DB_TYPE_DATE
	  && typ != DB_TYPE_UTIME
	  && typ != DB_TYPE_TIME
	  && typ != DB_TYPE_DATETIME
	  && !((typ == DB_TYPE_INTEGER || typ == DB_TYPE_BIGINT)
	       && ((typ1 == DB_TYPE_DATE && typ2 == DB_TYPE_DATE)
		   || (typ1 == DB_TYPE_UTIME && typ2 == DB_TYPE_UTIME)
		   || (typ1 == DB_TYPE_DATETIME && typ2 == DB_TYPE_DATETIME)
		   || (typ1 == DB_TYPE_TIME && typ2 == DB_TYPE_TIME))))
	{
	  /* coerce operands to data type of result */
	  if (typ1 != typ)
	    {
	      db_make_null (&tmp_val);
	      /*force explicit cast ; scenario :
	       * INSERT INTO t VALUE(''1''+?) , USING 10, column is INTEGER*/
	      if (tp_value_cast (arg1, &tmp_val, domain, false) !=
		  DOMAIN_COMPATIBLE)
		{
		  PT_ERRORmf2 (parser, o1, MSGCAT_SET_PARSER_SEMANTIC,
			       MSGCAT_SEMANTIC_CANT_COERCE_TO,
			       parser_print_tree (parser, o1),
			       pt_show_type_enum (rTyp));

		  return 0;
		}
	      else
		{
		  db_value_clear (arg1);
		  *arg1 = tmp_val;
		}
	    }

	  if (typ2 != typ)
	    {
	      db_make_null (&tmp_val);
	      /* force explicit cast ; scenario:
	       * INSERT INTO t VALUE(? + ''1'') , USING 10, column is INTEGER*/
	      if (tp_value_cast (arg2, &tmp_val, domain, false) !=
		  DOMAIN_COMPATIBLE)
		{
		  PT_ERRORmf2 (parser, o2, MSGCAT_SET_PARSER_SEMANTIC,
			       MSGCAT_SEMANTIC_CANT_COERCE_TO,
			       parser_print_tree (parser, o2),
			       pt_show_type_enum (rTyp));

		  return 0;
		}
	      else
		{
		  db_value_clear (arg2);
		  *arg2 = tmp_val;
		}
	    }
	}

      switch (op)
	{
	case PT_PLUS:
	  switch (typ)
	    {
	    case DB_TYPE_SET:
	    case DB_TYPE_MULTISET:
	    case DB_TYPE_SEQUENCE:
	      if (!pt_union_sets (parser, domain, arg1, arg2, result, o2))
		{
		  return 0;	/* set union failed */
		}
	      break;

	    case DB_TYPE_CHAR:
	    case DB_TYPE_NCHAR:
	    case DB_TYPE_VARCHAR:
	    case DB_TYPE_VARNCHAR:
	    case DB_TYPE_BIT:
	    case DB_TYPE_VARBIT:
	      if (db_string_concatenate (arg1, arg2,
					 result, &truncation) < 0 ||
		  truncation != DATA_STATUS_OK)
		{
		  PT_ERRORc (parser, o1, er_msg ());
		  return 0;
		}
	      break;

	    case DB_TYPE_INTEGER:
	      {
		int i1, i2, itmp;

		i1 = DB_GET_INT (arg1);
		i2 = DB_GET_INT (arg2);
		itmp = i1 + i2;
		if (OR_CHECK_ADD_OVERFLOW (i1, i2, itmp))
		  goto overflow;
		else
		  db_make_int (result, itmp);
		break;
	      }

	    case DB_TYPE_BIGINT:
	      {
		DB_BIGINT bi1, bi2, bitmp;

		bi1 = DB_GET_BIGINT (arg1);
		bi2 = DB_GET_BIGINT (arg2);
		bitmp = bi1 + bi2;
		if (OR_CHECK_ADD_OVERFLOW (bi1, bi2, bitmp))
		  goto overflow;
		else
		  db_make_bigint (result, bitmp);
		break;
	      }

	    case DB_TYPE_SHORT:
	      {
		short s1, s2, stmp;

		s1 = DB_GET_SHORT (arg1);
		s2 = DB_GET_SHORT (arg2);
		stmp = s1 + s2;
		if (OR_CHECK_ADD_OVERFLOW (s1, s2, stmp))
		  goto overflow;
		else
		  db_make_short (result, stmp);
		break;
	      }

	    case DB_TYPE_FLOAT:
	      {
		float ftmp;

		ftmp = DB_GET_FLOAT (arg1) + DB_GET_FLOAT (arg2);
		if (OR_CHECK_FLOAT_OVERFLOW (ftmp))
		  goto overflow;
		else
		  db_make_float (result, ftmp);
		break;
	      }

	    case DB_TYPE_DOUBLE:
	      {
		double dtmp;

		dtmp = DB_GET_DOUBLE (arg1) + DB_GET_DOUBLE (arg2);
		if (OR_CHECK_DOUBLE_OVERFLOW (dtmp))
		  goto overflow;
		else
		  db_make_double (result, dtmp);
		break;
	      }

	    case DB_TYPE_NUMERIC:
	      if (numeric_db_value_add (arg1, arg2, result) != NO_ERROR)
		{
		  PT_ERRORc (parser, o1, er_msg ());
		  return 0;
		}

	      res_type = DB_VALUE_DOMAIN_TYPE (result);
	      if (tp_value_coerce (result, result,
				   domain) != DOMAIN_COMPATIBLE)
		{
		  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE,
			  ER_TP_CANT_COERCE, 2,
			  pr_type_name (res_type),
			  pr_type_name (TP_DOMAIN_TYPE (domain)));

		  return 0;
		}
	      break;

	    case DB_TYPE_MONETARY:
	      {
		double dtmp;

		dtmp = (DB_GET_MONETARY (arg1)->amount +
			DB_GET_MONETARY (arg2)->amount);
		if (OR_CHECK_DOUBLE_OVERFLOW (dtmp))
		  goto overflow;
		else
		  DB_MAKE_MONETARY (result, dtmp);
		break;
	      }

	    case DB_TYPE_TIME:
	      {
		DB_TIME *time, result_time;
		int hour, minute, second;
		DB_BIGINT itmp;
		DB_VALUE *other;

		if (DB_VALUE_TYPE (arg1) == DB_TYPE_TIME)
		  {
		    time = DB_GET_TIME (arg1);
		    other = arg2;
		  }
		else
		  {
		    time = DB_GET_TIME (arg2);
		    other = arg1;
		  }

		switch (DB_VALUE_TYPE (other))
		  {
		  case DB_TYPE_INTEGER:
		    itmp = DB_GET_INTEGER (other);	/* SECONDS_OF_ONE_DAY */
		    break;
		  case DB_TYPE_SMALLINT:
		    itmp = DB_GET_SHORT (other);	/* SECONDS_OF_ONE_DAY */
		    break;
		  case DB_TYPE_BIGINT:
		    itmp = DB_GET_BIGINT (other);	/* SECONDS_OF_ONE_DAY */
		    break;
		  default:
		    return 0;
		  }
		if (itmp < 0)
		  {
		    DB_TIME uother = (DB_TIME) ((-itmp) % 86400);
		    if (*time < uother)
		      {
			*time += 86400;
		      }
		    result_time = (*time) - uother;
		  }
		else
		  {
		    result_time = (itmp + *time) % 86400;
		  }
		db_time_decode (&result_time, &hour, &minute, &second);
		db_make_time (result, hour, minute, second);
	      }
	      break;

	    case DB_TYPE_UTIME:
	      {
		DB_UTIME *utime, result_utime;
		DB_VALUE *other;
		DB_BIGINT bi;

		if (DB_VALUE_TYPE (arg1) == DB_TYPE_UTIME)
		  {
		    utime = DB_GET_UTIME (arg1);
		    other = arg2;
		  }
		else
		  {
		    utime = DB_GET_UTIME (arg2);
		    other = arg1;
		  }

		if (*utime == 0)
		  {
		    /* operation with zero date returns null */
		    DB_MAKE_NULL (result);
		    break;
		  }

		switch (DB_VALUE_TYPE (other))
		  {
		  case DB_TYPE_INTEGER:
		    bi = DB_GET_INTEGER (other);
		    break;
		  case DB_TYPE_SMALLINT:
		    bi = DB_GET_SHORT (other);
		    break;
		  case DB_TYPE_BIGINT:
		    bi = DB_GET_BIGINT (other);
		    break;
		  default:
		    return 0;
		  }

		if (bi < 0)
		  {
		    if (bi == DB_BIGINT_MIN)
		      {
			if (*utime == 0)
			  {
			    goto overflow;
			  }
			else
			  {
			    bi++;
			    (*utime)--;
			  }
		      }
		    if (OR_CHECK_UNS_SUB_UNDERFLOW (*utime, -bi,
						    (*utime) + bi))
		      {
			goto overflow;
		      }
		    result_utime = (DB_UTIME) ((*utime) + bi);
		  }
		else
		  {
		    result_utime = (DB_UTIME) (*utime + bi);
		    if (OR_CHECK_UNS_ADD_OVERFLOW (*utime, bi, result_utime)
			|| INT_MAX < result_utime)
		      {
			goto overflow;
		      }
		  }

		db_make_timestamp (result, result_utime);
	      }
	      break;

	    case DB_TYPE_DATETIME:
	      {
		DB_DATETIME *datetime, result_datetime;
		DB_BIGINT bi1, bi2, result_bi, tmp_bi;
		DB_VALUE *other;

		if (DB_VALUE_TYPE (arg1) == DB_TYPE_DATETIME)
		  {
		    datetime = DB_GET_DATETIME (arg1);
		    other = arg2;
		  }
		else
		  {
		    datetime = DB_GET_DATETIME (arg2);
		    other = arg1;
		  }

		if (datetime->date == 0 && datetime->time == 0)
		  {
		    /* operation with zero date returns null */
		    DB_MAKE_NULL (result);
		    break;
		  }

		switch (DB_VALUE_TYPE (other))
		  {
		  case DB_TYPE_SMALLINT:
		    bi2 = (DB_BIGINT) DB_GET_SHORT (other);
		    break;
		  case DB_TYPE_INTEGER:
		    bi2 = (DB_BIGINT) DB_GET_INTEGER (other);
		    break;
		  default:
		    bi2 = (DB_BIGINT) DB_GET_BIGINT (other);
		    break;
		  }

		bi1 = ((DB_BIGINT) datetime->date) * MILLISECONDS_OF_ONE_DAY
		  + datetime->time;

		if (bi2 < 0)
		  {
		    if (bi2 == DB_BIGINT_MIN)
		      {
			goto overflow;
		      }
		    result_bi = bi1 + bi2;
		    if (OR_CHECK_SUB_UNDERFLOW (bi1, bi2, result_bi))
		      {
			goto overflow;
		      }
		  }
		else
		  {
		    result_bi = bi1 + bi2;
		    if (OR_CHECK_ADD_OVERFLOW (bi1, bi2, result_bi))
		      {
			goto overflow;
		      }
		  }

		tmp_bi = (DB_BIGINT) (result_bi / MILLISECONDS_OF_ONE_DAY);
		if (OR_CHECK_INT_OVERFLOW (tmp_bi) || tmp_bi > DB_DATE_MAX
		    || tmp_bi < DB_DATE_MIN)
		  {
		    goto overflow;
		  }
		result_datetime.date = (int) tmp_bi;
		result_datetime.time =
		  (int) (result_bi % MILLISECONDS_OF_ONE_DAY);

		db_make_datetime (result, &result_datetime);
	      }
	      break;

	    case DB_TYPE_DATE:
	      {
		DB_DATE *date, result_date;
		DB_VALUE *other;
		DB_BIGINT bi;

		if (DB_VALUE_TYPE (arg1) == DB_TYPE_DATE)
		  {
		    date = DB_GET_DATE (arg1);
		    other = arg2;
		  }
		else
		  {
		    date = DB_GET_DATE (arg2);
		    other = arg1;
		  }

		if (*date == 0)
		  {
		    /* operation with zero date returns null */
		    DB_MAKE_NULL (result);
		    break;
		  }

		switch (DB_VALUE_TYPE (other))
		  {
		  case DB_TYPE_INTEGER:
		    bi = DB_GET_INTEGER (other);
		    break;
		  case DB_TYPE_SMALLINT:
		    bi = DB_GET_SHORT (other);
		    break;
		  case DB_TYPE_BIGINT:
		    bi = DB_GET_BIGINT (other);
		    break;
		  default:
		    return 0;
		  }
		if (bi < 0)
		  {
		    if (bi == DB_BIGINT_MIN)
		      {
			if (*date == 0)
			  {
			    goto overflow;
			  }
			bi++;
			(*date)--;
		      }
		    if (OR_CHECK_UNS_SUB_UNDERFLOW (*date, -bi, (*date) + bi)
			|| (*date) + bi < DB_DATE_MIN)
		      {
			goto overflow;
		      }
		    result_date = (DB_DATE) ((*date) + bi);
		  }
		else
		  {
		    result_date = (DB_DATE) (*date + bi);
		    if (OR_CHECK_UNS_ADD_OVERFLOW (*date, bi, result_date)
			|| result_date > DB_DATE_MAX)
		      {
			goto overflow;
		      }
		  }

		db_value_put_encoded_date (result, &result_date);
	      }
	      break;
	    default:
	      return 0;
	    }
	  break;

	case PT_MINUS:
	  switch (typ)
	    {
	    case DB_TYPE_SET:
	    case DB_TYPE_MULTISET:
	      if (!pt_difference_sets (parser, domain, arg1,
				       arg2, result, o2))
		{
		  return 0;	/* set union failed */
		}
	      break;

	    case DB_TYPE_INTEGER:
	      {
		int i1, i2, itmp;

		i1 = DB_GET_INT (arg1);
		i2 = DB_GET_INT (arg2);
		itmp = i1 - i2;
		if (OR_CHECK_SUB_UNDERFLOW (i1, i2, itmp))
		  goto overflow;
		else
		  db_make_int (result, itmp);
		break;
	      }

	    case DB_TYPE_BIGINT:
	      {
		DB_BIGINT bi1, bi2, result_bi;
		DB_DATETIME *dt1, *dt2;

		if (typ1 == DB_TYPE_DATETIME && typ2 == DB_TYPE_DATETIME)
		  {
		    dt1 = DB_GET_DATETIME (arg1);
		    dt2 = DB_GET_DATETIME (arg2);

		    bi1 = ((DB_BIGINT) dt1->date) * MILLISECONDS_OF_ONE_DAY
		      + dt1->time;
		    bi2 = ((DB_BIGINT) dt2->date) * MILLISECONDS_OF_ONE_DAY
		      + dt2->time;
		  }
		else
		  {
		    bi1 = DB_GET_BIGINT (arg1);
		    bi2 = DB_GET_BIGINT (arg2);
		  }

		if ((TP_IS_DATE_TYPE (typ1) && bi1 == 0)
		    || (TP_IS_DATE_TYPE (typ2) && bi2 == 0))
		  {
		    /* operation with zero date returns null */
		    DB_MAKE_NULL (result);
		    break;
		  }

		result_bi = bi1 - bi2;
		if (OR_CHECK_SUB_UNDERFLOW (bi1, bi2, result_bi))
		  {
		    goto overflow;
		  }
		else
		  {
		    db_make_bigint (result, result_bi);
		  }
		break;
	      }

	    case DB_TYPE_SHORT:
	      {
		short s1, s2, stmp;

		s1 = DB_GET_SHORT (arg1);
		s2 = DB_GET_SHORT (arg2);
		stmp = s1 - s2;
		if (OR_CHECK_SUB_UNDERFLOW (s1, s2, stmp))
		  goto overflow;
		else
		  db_make_short (result, stmp);
		break;
	      }

	    case DB_TYPE_FLOAT:
	      {
		float ftmp;

		ftmp = DB_GET_FLOAT (arg1) - DB_GET_FLOAT (arg2);
		if (OR_CHECK_FLOAT_OVERFLOW (ftmp))
		  goto overflow;
		else
		  db_make_float (result, ftmp);
		break;
	      }

	    case DB_TYPE_DOUBLE:
	      {
		double dtmp;

		dtmp = DB_GET_DOUBLE (arg1) - DB_GET_DOUBLE (arg2);
		if (OR_CHECK_DOUBLE_OVERFLOW (dtmp))
		  goto overflow;
		else
		  db_make_double (result, dtmp);
		break;
	      }

	    case DB_TYPE_NUMERIC:
	      if (numeric_db_value_sub (arg1, arg2, result) != NO_ERROR)
		{
		  PT_ERRORc (parser, o1, er_msg ());
		  return 0;
		}
	      res_type = DB_VALUE_DOMAIN_TYPE (result);
	      if (tp_value_coerce (result, result,
				   domain) != DOMAIN_COMPATIBLE)
		{
		  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE,
			  ER_TP_CANT_COERCE, 2,
			  pr_type_name (res_type),
			  pr_type_name (TP_DOMAIN_TYPE (domain)));

		  return 0;
		}
	      break;

	    case DB_TYPE_MONETARY:
	      {
		double dtmp;

		dtmp = DB_GET_MONETARY (arg1)->amount -
		  DB_GET_MONETARY (arg2)->amount;
		if (OR_CHECK_DOUBLE_OVERFLOW (dtmp))
		  goto overflow;
		else
		  DB_MAKE_MONETARY (result, dtmp);
		break;
	      }


	    case DB_TYPE_TIME:
	      {
		DB_TIME *time, result_time;
		int hour, minute, second;
		DB_BIGINT bi = 0, ubi = 0;

		switch (DB_VALUE_TYPE (arg2))
		  {
		  case DB_TYPE_SHORT:
		    bi = DB_GET_SHORT (arg2);
		    break;
		  case DB_TYPE_INTEGER:
		    bi = DB_GET_INT (arg2);
		    break;
		  case DB_TYPE_BIGINT:
		    bi = DB_GET_BIGINT (arg2);
		    break;
		  default:
		    assert (false);
		    break;
		  }
		ubi = (bi < 0) ? -bi : bi;
		time = DB_GET_TIME (arg1);
		if (*time < (DB_TIME) (ubi % 86400))
		  {
		    *time += 86400;
		  }
		result_time = *time - (bi % 86400);
		db_time_decode (&result_time, &hour, &minute, &second);
		db_make_time (result, hour, minute, second);
	      }
	      break;

	    case DB_TYPE_UTIME:
	      {
		DB_UTIME *utime, result_utime;
		DB_BIGINT bi = 0;

		switch (DB_VALUE_TYPE (arg2))
		  {
		  case DB_TYPE_SHORT:
		    bi = DB_GET_SHORT (arg2);
		    break;
		  case DB_TYPE_INTEGER:
		    bi = DB_GET_INT (arg2);
		    break;
		  case DB_TYPE_BIGINT:
		    bi = DB_GET_BIGINT (arg2);
		    break;
		  default:
		    assert (false);
		    break;
		  }
		utime = DB_GET_UTIME (arg1);

		if (*utime == 0)
		  {
		    /* operation with zero date returns null */
		    DB_MAKE_NULL (result);
		    break;
		  }

		if (bi < 0)
		  {
		    /* we're adding */
		    result_utime = (DB_UTIME) (*utime - bi);
		    if (OR_CHECK_UNS_ADD_OVERFLOW (*utime, -bi, result_utime)
			|| INT_MAX < result_utime)
		      {
			goto overflow;
		      }
		  }
		else
		  {
		    result_utime = (DB_UTIME) (*utime - bi);
		    if (OR_CHECK_UNS_SUB_UNDERFLOW (*utime, bi, result_utime))
		      {
			PT_ERRORm (parser, o2, MSGCAT_SET_PARSER_SEMANTIC,
				   MSGCAT_SEMANTIC_TIME_UNDERFLOW);
			return 0;
		      }
		  }
		db_make_timestamp (result, result_utime);
	      }
	      break;

	    case DB_TYPE_DATETIME:
	      {
		DB_DATETIME result_datetime;
		DB_DATETIME *datetime;
		DB_BIGINT bi = 0;

		switch (DB_VALUE_TYPE (arg2))
		  {
		  case DB_TYPE_SHORT:
		    bi = DB_GET_SHORT (arg2);
		    break;
		  case DB_TYPE_INTEGER:
		    bi = DB_GET_INT (arg2);
		    break;
		  case DB_TYPE_BIGINT:
		    bi = DB_GET_BIGINT (arg2);
		    break;
		  default:
		    assert (false);
		    break;
		  }
		datetime = DB_GET_DATETIME (arg1);

		if (datetime->date == 0 && datetime->time == 0)
		  {
		    /* operation with zero date returns null */
		    DB_MAKE_NULL (result);
		    break;
		  }

		error = db_subtract_int_from_datetime (datetime, bi,
						       &result_datetime);
		if (error != NO_ERROR)
		  {
		    PT_ERRORm (parser, o2, MSGCAT_SET_PARSER_SEMANTIC,
			       MSGCAT_SEMANTIC_DATE_UNDERFLOW);
		    return 0;
		  }
		db_make_datetime (result, &result_datetime);
	      }
	      break;

	    case DB_TYPE_DATE:
	      {
		DB_DATE *date, result_date;
		int month, day, year;
		DB_BIGINT bi = 0;

		switch (DB_VALUE_TYPE (arg2))
		  {
		  case DB_TYPE_SHORT:
		    bi = DB_GET_SHORT (arg2);
		    break;
		  case DB_TYPE_INTEGER:
		    bi = DB_GET_INT (arg2);
		    break;
		  case DB_TYPE_BIGINT:
		    bi = DB_GET_BIGINT (arg2);
		    break;
		  default:
		    assert (false);
		    break;
		  }
		date = DB_GET_DATE (arg1);

		if (*date == 0)
		  {
		    /* operation with zero date returns null */
		    DB_MAKE_NULL (result);
		    break;
		  }

		if (bi < 0)
		  {
		    /* we're adding */
		    result_date = (DB_DATE) (*date - bi);
		    if (OR_CHECK_UNS_ADD_OVERFLOW (*date, -bi, result_date)
			|| result_date > DB_DATE_MAX)
		      {
			goto overflow;
		      }
		  }
		else
		  {
		    result_date = *date - (DB_DATE) bi;
		    if (OR_CHECK_UNS_SUB_UNDERFLOW (*date, bi, result_date)
			|| result_date < DB_DATE_MIN)
		      {
			PT_ERRORm (parser, o2, MSGCAT_SET_PARSER_SEMANTIC,
				   MSGCAT_SEMANTIC_DATE_UNDERFLOW);
			return 0;
		      }
		  }
		db_date_decode (&result_date, &month, &day, &year);
		db_make_date (result, month, day, year);
	      }
	      break;

	    default:
	      return 0;
	    }
	  break;

	case PT_TIMES:
	  switch (typ)
	    {
	    case DB_TYPE_SET:
	    case DB_TYPE_MULTISET:
	      if (!pt_product_sets (parser, domain, arg1, arg2, result, o2))
		{
		  return 0;	/* set union failed */
		}
	      break;

	    case DB_TYPE_INTEGER:
	      {
		int i1, i2, itmp;

		i1 = DB_GET_INT (arg1);
		i2 = DB_GET_INT (arg2);
		itmp = i1 * i2;
		if (OR_CHECK_MULT_OVERFLOW (i1, i2, itmp))
		  goto overflow;
		else
		  db_make_int (result, itmp);
		break;
	      }

	    case DB_TYPE_BIGINT:
	      {
		DB_BIGINT bi1, bi2, bitmp;
		bi1 = DB_GET_BIGINT (arg1);
		bi2 = DB_GET_BIGINT (arg2);
		bitmp = bi1 * bi2;
		if (OR_CHECK_MULT_OVERFLOW (bi1, bi2, bitmp))
		  goto overflow;
		else
		  db_make_bigint (result, bitmp);
		break;
	      }

	    case DB_TYPE_SHORT:
	      {
		short s1, s2, stmp;

		s1 = DB_GET_SHORT (arg1);
		s2 = DB_GET_SHORT (arg2);
		stmp = s1 * s2;
		if (OR_CHECK_MULT_OVERFLOW (s1, s2, stmp))
		  goto overflow;
		else
		  db_make_short (result, stmp);
		break;
	      }

	    case DB_TYPE_FLOAT:
	      {
		float ftmp;

		ftmp = DB_GET_FLOAT (arg1) * DB_GET_FLOAT (arg2);
		if (OR_CHECK_FLOAT_OVERFLOW (ftmp))
		  goto overflow;
		else
		  db_make_float (result, ftmp);
		break;
	      }

	    case DB_TYPE_DOUBLE:
	      {
		double dtmp;

		dtmp = DB_GET_DOUBLE (arg1) * DB_GET_DOUBLE (arg2);
		if (OR_CHECK_DOUBLE_OVERFLOW (dtmp))
		  goto overflow;
		else
		  db_make_double (result, dtmp);
		break;
	      }

	    case DB_TYPE_NUMERIC:
	      error = numeric_db_value_mul (arg1, arg2, result);
	      if (error == ER_NUM_OVERFLOW)
		{
		  goto overflow;
		}
	      else if (error != NO_ERROR)
		{
		  PT_ERRORc (parser, o1, er_msg ());
		  return 0;
		}
	      res_type = DB_VALUE_DOMAIN_TYPE (result);
	      if (tp_value_coerce (result, result,
				   domain) != DOMAIN_COMPATIBLE)
		{
		  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE,
			  ER_TP_CANT_COERCE, 2,
			  pr_type_name (res_type),
			  pr_type_name (TP_DOMAIN_TYPE (domain)));

		  return 0;
		}
	      break;

	    case DB_TYPE_MONETARY:
	      {
		double dtmp;

		dtmp = DB_GET_MONETARY (arg1)->amount *
		  DB_GET_MONETARY (arg2)->amount;
		if (OR_CHECK_DOUBLE_OVERFLOW (dtmp))
		  goto overflow;
		else
		  DB_MAKE_MONETARY (result, dtmp);
		break;
	      }
	      break;

	    default:
	      return 0;
	    }
	  break;

	case PT_DIVIDE:
	  switch (typ)
	    {
	    case DB_TYPE_SHORT:
	      if (DB_GET_SHORT (arg2) != 0)
		{
		  db_make_short (result,
				 DB_GET_SHORT (arg1) / DB_GET_SHORT (arg2));
		  return 1;
		}
	      break;

	    case DB_TYPE_INTEGER:
	      if (DB_GET_INTEGER (arg2) != 0)
		{
		  db_make_int (result, (DB_GET_INTEGER (arg1)
					/ DB_GET_INTEGER (arg2)));
		  return 1;
		}
	      break;
	    case DB_TYPE_BIGINT:
	      if (DB_GET_BIGINT (arg2) != 0)
		{
		  db_make_bigint (result, (DB_GET_BIGINT (arg1)
					   / DB_GET_BIGINT (arg2)));
		  return 1;
		}
	      break;
	    case DB_TYPE_FLOAT:
	      if (fabs (DB_GET_FLOAT (arg2)) > FLT_EPSILON)
		{
		  float ftmp;

		  ftmp = DB_GET_FLOAT (arg1) / DB_GET_FLOAT (arg2);
		  if (OR_CHECK_FLOAT_OVERFLOW (ftmp))
		    {
		      goto overflow;
		    }
		  else
		    {
		      db_make_float (result, ftmp);
		      return 1;
		    }
		}
	      break;

	    case DB_TYPE_DOUBLE:
	      if (fabs (DB_GET_DOUBLE (arg2)) > DBL_EPSILON)
		{
		  double dtmp;

		  dtmp = DB_GET_DOUBLE (arg1) / DB_GET_DOUBLE (arg2);
		  if (OR_CHECK_DOUBLE_OVERFLOW (dtmp))
		    {
		      goto overflow;
		    }
		  else
		    {
		      db_make_double (result, dtmp);
		      return 1;	/* success */
		    }
		}
	      break;

	    case DB_TYPE_NUMERIC:
	      if (!numeric_db_value_is_zero (arg2))
		{
		  error = numeric_db_value_div (arg1, arg2, result);
		  if (error == ER_NUM_OVERFLOW)
		    {
		      goto overflow;
		    }
		  else if (error != NO_ERROR)
		    {
		      PT_ERRORc (parser, o1, er_msg ());
		      return 0;
		    }

		  res_type = DB_VALUE_DOMAIN_TYPE (result);
		  if (tp_value_coerce (result, result,
				       domain) != DOMAIN_COMPATIBLE)
		    {
		      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE,
			      ER_TP_CANT_COERCE, 2,
			      pr_type_name (res_type),
			      pr_type_name (TP_DOMAIN_TYPE (domain)));
		      return 0;
		    }

		  return 1;
		}
	      break;

	    case DB_TYPE_MONETARY:
	      if (fabs (DB_GET_MONETARY (arg2)->amount) > DBL_EPSILON)
		{
		  double dtmp;

		  dtmp = DB_GET_MONETARY (arg1)->amount /
		    DB_GET_MONETARY (arg2)->amount;
		  if (OR_CHECK_DOUBLE_OVERFLOW (dtmp))
		    {
		      goto overflow;
		    }
		  else
		    {
		      DB_MAKE_MONETARY (result, dtmp);
		      return 1;	/* success */
		    }
		}
	      break;

	    default:
	      return 0;
	    }

	  PT_ERRORm (parser, o2, MSGCAT_SET_PARSER_SEMANTIC,
		     MSGCAT_SEMANTIC_ZERO_DIVIDE);
	  return 0;

	default:
	  return 0;
	}
      break;

    case PT_STRCAT:
      if (typ1 == DB_TYPE_NULL || typ2 == DB_TYPE_NULL)
	{
	  bool check_empty_string;

	  check_empty_string =
	    ((prm_get_bool_value (PRM_ID_ORACLE_STYLE_EMPTY_STRING))
	     ? true : false);

	  if (!check_empty_string || !PT_IS_STRING_TYPE (rTyp))
	    {
	      db_make_null (result);	/* NULL arith_op any = NULL */
	      break;
	    }
	}

      /* screen out cases we don't evaluate */
      if (!PT_IS_STRING_TYPE (rTyp))
	{
	  return 0;
	}

      switch (typ)
	{
	case DB_TYPE_CHAR:
	case DB_TYPE_NCHAR:
	case DB_TYPE_VARCHAR:
	case DB_TYPE_VARNCHAR:
	case DB_TYPE_BIT:
	case DB_TYPE_VARBIT:
	  if (db_string_concatenate (arg1, arg2, result, &truncation) < 0
	      || truncation != DATA_STATUS_OK)
	    {
	      PT_ERRORc (parser, o1, er_msg ());
	      return 0;
	    }
	  break;

	default:
	  return 0;
	}
      break;

    case PT_MODULUS:
      error = db_mod_dbval (result, arg1, arg2);
      if (error != NO_ERROR)
	{
	  PT_ERRORc (parser, o1, er_msg ());
	  return 0;
	}
      break;

    case PT_PI:
      db_make_double (result, 3.14159265358979323846264338);
      break;

    case PT_RAND:
      /* rand() and drand() should always generate the same value during a statement.
       * To support it, we add lrand and drand member to PARSER_CONTEXT.
       */
      if (DB_IS_NULL (arg1))
	{
	  db_make_int (result, parser->lrand);
	}
      else
	{
	  srand48 (DB_GET_INT (arg1));
	  db_make_int (result, lrand48 ());
	}
      break;

    case PT_DRAND:
      if (DB_IS_NULL (arg1))
	{
	  db_make_double (result, parser->drand);
	}
      else
	{
	  srand48 (DB_GET_INT (arg1));
	  db_make_double (result, drand48 ());
	}
      break;

    case PT_RANDOM:
      /* Generate seed internally if there is no seed given as argument.
       * rand() on select list gets a random value by fetch_peek_arith().
       * But, if rand() is specified on VALUES clause of insert statement,
       * it gets a random value by the following codes.
       * In this case, DB_VALUE(arg1) of NULL type is passed.
       */
      if (DB_IS_NULL (arg1))
	{
	  struct timeval t;
	  gettimeofday (&t, NULL);
	  srand48 ((long) (t.tv_usec + lrand48 ()));
	}
      else
	{
	  srand48 (DB_GET_INT (arg1));
	}
      db_make_int (result, lrand48 ());
      break;

    case PT_DRANDOM:
      if (DB_IS_NULL (arg1))
	{
	  struct timeval t;
	  gettimeofday (&t, NULL);
	  srand48 ((long) (t.tv_usec + lrand48 ()));
	}
      else
	{
	  srand48 (DB_GET_INT (arg1));
	}
      db_make_double (result, drand48 ());
      break;

    case PT_FLOOR:
      error = db_floor_dbval (result, arg1);
      if (error != NO_ERROR)
	{
	  PT_ERRORc (parser, o1, er_msg ());
	  return 0;
	}
      break;

    case PT_CEIL:
      error = db_ceil_dbval (result, arg1);
      if (error != NO_ERROR)
	{
	  PT_ERRORc (parser, o1, er_msg ());
	  return 0;
	}
      break;

    case PT_SIGN:
      error = db_sign_dbval (result, arg1);
      if (error != NO_ERROR)
	{
	  PT_ERRORc (parser, o1, er_msg ());
	  return 0;
	}
      break;

    case PT_ABS:
      error = db_abs_dbval (result, arg1);
      if (error != NO_ERROR)
	{
	  PT_ERRORc (parser, o1, er_msg ());
	  return 0;
	}
      break;

    case PT_POWER:
      error = db_power_dbval (result, arg1, arg2);
      if (error != NO_ERROR)
	{
	  PT_ERRORc (parser, o1, er_msg ());
	  return 0;
	}
      break;

    case PT_ROUND:
      error = db_round_dbval (result, arg1, arg2);
      if (error != NO_ERROR)
	{
	  PT_ERRORc (parser, o1, er_msg ());
	  return 0;
	}
      break;

    case PT_LOG:
      error = db_log_dbval (result, arg1, arg2);
      if (error != NO_ERROR)
	{
	  PT_ERRORc (parser, o1, er_msg ());
	  return 0;
	}
      break;

    case PT_EXP:
      error = db_exp_dbval (result, arg1);
      if (error != NO_ERROR)
	{
	  PT_ERRORc (parser, o1, er_msg ());
	  return 0;
	}
      break;

    case PT_SQRT:
      error = db_sqrt_dbval (result, arg1);
      if (error != NO_ERROR)
	{
	  PT_ERRORc (parser, o1, er_msg ());
	  return 0;
	}
      break;

    case PT_SIN:
      error = db_sin_dbval (result, arg1);
      if (error != NO_ERROR)
	{
	  PT_ERRORc (parser, o1, er_msg ());
	  return 0;
	}
      break;

    case PT_COS:
      error = db_cos_dbval (result, arg1);
      if (error != NO_ERROR)
	{
	  PT_ERRORc (parser, o1, er_msg ());
	  return 0;
	}
      break;

    case PT_TAN:
      error = db_tan_dbval (result, arg1);
      if (error != NO_ERROR)
	{
	  PT_ERRORc (parser, o1, er_msg ());
	  return 0;
	}
      break;

    case PT_COT:
      error = db_cot_dbval (result, arg1);
      if (error != NO_ERROR)
	{
	  PT_ERRORc (parser, o1, er_msg ());
	  return 0;
	}
      break;

    case PT_ACOS:
      error = db_acos_dbval (result, arg1);
      if (error != NO_ERROR)
	{
	  PT_ERRORc (parser, o1, er_msg ());
	  return 0;
	}
      break;

    case PT_ASIN:
      error = db_asin_dbval (result, arg1);
      if (error != NO_ERROR)
	{
	  PT_ERRORc (parser, o1, er_msg ());
	  return 0;
	}
      break;

    case PT_ATAN:
      error = db_atan_dbval (result, arg1);
      if (error != NO_ERROR)
	{
	  PT_ERRORc (parser, o1, er_msg ());
	  return 0;
	}
      break;

    case PT_ATAN2:
      error = db_atan2_dbval (result, arg1, arg2);
      if (error != NO_ERROR)
	{
	  PT_ERRORc (parser, o1, er_msg ());
	  return 0;
	}
      break;

    case PT_DEGREES:
      error = db_degrees_dbval (result, arg1);
      if (error != NO_ERROR)
	{
	  PT_ERRORc (parser, o1, er_msg ());
	  return 0;
	}
      break;

    case PT_DATEF:
      error = db_date_dbval (result, arg1, NULL);
      if (error != NO_ERROR)
	{
	  PT_ERRORc (parser, o1, er_msg ());
	  return 0;
	}
      break;

    case PT_TIMEF:
      error = db_time_dbval (result, arg1, NULL);
      if (error != NO_ERROR)
	{
	  PT_ERRORc (parser, o1, er_msg ());
	  return 0;
	}
      break;

    case PT_RADIANS:
      error = db_radians_dbval (result, arg1);
      if (error != NO_ERROR)
	{
	  PT_ERRORc (parser, o1, er_msg ());
	  return 0;
	}
      break;

    case PT_LN:
      error = db_log_generic_dbval (result, arg1, -1 /* e convention */ );
      if (error != NO_ERROR)
	{
	  PT_ERRORc (parser, o1, er_msg ());
	  return 0;
	}
      break;

    case PT_LOG2:
      error = db_log_generic_dbval (result, arg1, 2);
      if (error != NO_ERROR)
	{
	  PT_ERRORc (parser, o1, er_msg ());
	  return 0;
	}
      break;

    case PT_LOG10:
      error = db_log_generic_dbval (result, arg1, 10);
      if (error != NO_ERROR)
	{
	  PT_ERRORc (parser, o1, er_msg ());
	  return 0;
	}
      break;

    case PT_TRUNC:
      error = db_trunc_dbval (result, arg1, arg2);
      if (error != NO_ERROR)
	{
	  PT_ERRORc (parser, o1, er_msg ());
	  return 0;
	}
      break;

    case PT_CHR:
      error = db_string_chr (result, arg1, arg2);
      if (error != NO_ERROR)
	{
	  PT_ERRORc (parser, o1, er_msg ());
	  return 0;
	}
      break;

    case PT_INSTR:
      error = db_string_instr (arg1, arg2, arg3, result);
      if (error < 0)
	{
	  PT_ERRORc (parser, o1, er_msg ());
	  return 0;
	}
      else
	{
	  return 1;
	}

    case PT_LEAST:
      cmp_result =
	(DB_VALUE_COMPARE_RESULT) tp_value_compare (arg1, arg2, 1, 0);
      if (cmp_result == DB_EQ || cmp_result == DB_LT)
	{
	  pr_clone_value ((DB_VALUE *) arg1, result);
	}
      else if (cmp_result == DB_GT)
	{
	  pr_clone_value ((DB_VALUE *) arg2, result);
	}
      else
	{
	  assert_release (DB_IS_NULL (arg1) || DB_IS_NULL (arg2));
	  db_make_null (result);
	  return 1;
	}

      if (tp_value_cast (result, result, domain, true) != DOMAIN_COMPATIBLE)
	{
	  PT_ERRORmf2 (parser, o2, MSGCAT_SET_PARSER_SEMANTIC,
		       MSGCAT_SEMANTIC_CANT_COERCE_TO,
		       pt_short_print (parser, o2), pt_show_type_enum (rTyp));
	  return 0;
	}

      return 1;

    case PT_GREATEST:
      cmp_result =
	(DB_VALUE_COMPARE_RESULT) tp_value_compare (arg1, arg2, 1, 0);
      if (cmp_result == DB_EQ || cmp_result == DB_GT)
	{
	  pr_clone_value ((DB_VALUE *) arg1, result);
	}
      else if (cmp_result == DB_LT)
	{
	  pr_clone_value ((DB_VALUE *) arg2, result);
	}
      else
	{
	  assert_release (DB_IS_NULL (arg1) || DB_IS_NULL (arg2));
	  db_make_null (result);
	  return 1;
	}

      if (tp_value_cast (result, result, domain, true) != DOMAIN_COMPATIBLE)
	{
	  PT_ERRORmf2 (parser, o2, MSGCAT_SET_PARSER_SEMANTIC,
		       MSGCAT_SEMANTIC_CANT_COERCE_TO,
		       pt_short_print (parser, o2), pt_show_type_enum (rTyp));
	  return 0;
	}

      return 1;

    case PT_POSITION:
      error = db_string_position (arg1, arg2, result);
      if (error < 0)
	{
	  PT_ERRORc (parser, o1, er_msg ());
	  return 0;
	}
      else
	{
	  return 1;
	}

    case PT_FINDINSET:
      error = db_find_string_in_in_set (arg1, arg2, result);
      if (error < 0)
	{
	  PT_ERRORc (parser, o1, er_msg ());
	  return 0;
	}
      else
	{
	  return 1;
	}

    case PT_SUBSTRING:
      if (DB_IS_NULL (arg1) || DB_IS_NULL (arg2) || (o3 && DB_IS_NULL (arg3)))
	{
	  DB_MAKE_NULL (result);
	  return 1;
	}

      if (prm_get_integer_value (PRM_ID_COMPAT_MODE) == COMPAT_MYSQL)
	{
	  DB_VALUE tmp_len, tmp_arg2, tmp_arg3;
	  int pos, len;

	  pos = db_get_int (arg2);
	  if (pos < 0)
	    {
	      if (QSTR_IS_BIT (typ1))
		{
		  if (db_string_bit_length (arg1, &tmp_len) != NO_ERROR)
		    {
		      PT_ERRORc (parser, o1, er_msg ());
		      return 0;
		    }
		}
	      else
		{
		  if (db_string_char_length (arg1, &tmp_len) != NO_ERROR)
		    {
		      PT_ERRORc (parser, o1, er_msg ());
		      return 0;
		    }
		}
	      if (DB_IS_NULL (&tmp_len))
		{
		  PT_ERRORc (parser, o1, er_msg ());
		  return 0;
		}
	      pos = pos + db_get_int (&tmp_len) + 1;
	    }

	  if (pos < 1)
	    {
	      db_make_int (&tmp_arg2, 1);
	    }
	  else
	    {
	      db_make_int (&tmp_arg2, pos);
	    }

	  if (o3)
	    {
	      len = db_get_int (arg3);
	      if (len < 1)
		{
		  db_make_int (&tmp_arg3, 0);
		}
	      else
		{
		  db_make_int (&tmp_arg3, len);
		}
	    }
	  else
	    {
	      db_make_null (&tmp_arg3);
	    }

	  error =
	    db_string_substring (pt_misc_to_qp_misc_operand
				 (qualifier), arg1, &tmp_arg2,
				 &tmp_arg3, result);
	  if (error < 0)
	    {
	      PT_ERRORc (parser, o1, er_msg ());
	      return 0;
	    }
	  else
	    {
	      return 1;
	    }
	}
      else
	{
	  error =
	    db_string_substring (pt_misc_to_qp_misc_operand
				 (qualifier), arg1, arg2, arg3, result);
	  if (error < 0)
	    {
	      PT_ERRORc (parser, o1, er_msg ());
	      return 0;
	    }
	  else
	    {
	      return 1;
	    }
	}

    case PT_OCTET_LENGTH:
      if (o1->type_enum == PT_TYPE_NA || o1->type_enum == PT_TYPE_NULL)
	{
	  db_make_null (result);
	  return 1;
	}

      if (!PT_IS_STRING_TYPE (o1->type_enum))
	{
	  return 0;
	}

      db_make_int (result, db_get_string_size (arg1));
      return 1;

    case PT_BIT_LENGTH:
      if (o1->type_enum == PT_TYPE_NA || o1->type_enum == PT_TYPE_NULL)
	{
	  db_make_null (result);
	  return 1;
	}

      if (!PT_IS_STRING_TYPE (o1->type_enum))
	{
	  return 0;
	}

      if (PT_IS_CHAR_STRING_TYPE (o1->type_enum))
	{
	  db_make_int (result, 8 * DB_GET_STRING_SIZE (arg1));
	}
      else
	{
	  int len = 0;

	  /* must be a bit gadget */
	  (void) db_get_bit (arg1, &len);
	  db_make_int (result, len);
	}
      return 1;

    case PT_CHAR_LENGTH:
      if (o1->type_enum == PT_TYPE_NA || o1->type_enum == PT_TYPE_NULL)
	{
	  db_make_null (result);
	  return 1;
	}
      else if (!PT_IS_CHAR_STRING_TYPE (o1->type_enum))
	{
	  return 0;
	}
      db_make_int (result, DB_GET_STRING_LENGTH (arg1));
      return 1;

    case PT_LOWER:
      error = db_string_lower (arg1, result);
      if (error < 0)
	{
	  PT_ERRORc (parser, o1, er_msg ());
	  return 0;
	}
      else
	{
	  return 1;
	}

    case PT_UPPER:
      error = db_string_upper (arg1, result);
      if (error < 0)
	{
	  PT_ERRORc (parser, o1, er_msg ());
	  return 0;
	}
      else
	{
	  return 1;
	}

    case PT_HEX:
      error = db_hex (arg1, result);
      if (error < 0)
	{
	  PT_ERRORc (parser, o1, er_msg ());
	  return 0;
	}
      else
	{
	  return 1;
	}

    case PT_ASCII:
      error = db_ascii (arg1, result);
      if (error < 0)
	{
	  PT_ERRORc (parser, o1, er_msg ());
	  return 0;
	}
      else
	{
	  return 1;
	}

    case PT_CONV:
      error = db_conv (arg1, arg2, arg3, result);
      if (error < 0)
	{
	  PT_ERRORc (parser, o1, er_msg ());
	  return 0;
	}
      else
	{
	  return 1;
	}

    case PT_BIN:
      error = db_bigint_to_binary_string (arg1, result);
      if (error != NO_ERROR)
	{
	  PT_ERRORc (parser, o1, er_msg ());
	  return 0;
	}
      else
	{
	  return 1;
	}

    case PT_TRIM:
      error =
	db_string_trim (pt_misc_to_qp_misc_operand (qualifier),
			arg2, arg1, result);
      if (error < 0)
	{
	  PT_ERRORc (parser, o1, er_msg ());
	  return 0;
	}
      else
	{
	  return 1;
	}

    case PT_LIKE_LOWER_BOUND:
    case PT_LIKE_UPPER_BOUND:
      error = db_like_bound (arg1, arg2, result, (op == PT_LIKE_LOWER_BOUND));
      if (error < 0)
	{
	  PT_ERRORc (parser, o1, er_msg ());
	  return 0;
	}
      else
	{
	  return 1;
	}

    case PT_LTRIM:
      error = db_string_trim (LEADING, arg2, arg1, result);
      if (error < 0)
	{
	  PT_ERRORc (parser, o1, er_msg ());
	  return 0;
	}
      else
	{
	  return 1;
	}

    case PT_RTRIM:
      error = db_string_trim (TRAILING, arg2, arg1, result);
      if (error < 0)
	{
	  PT_ERRORc (parser, o1, er_msg ());
	  return 0;
	}
      else
	{
	  return 1;
	}

    case PT_FROM_UNIXTIME:
      error = db_from_unixtime (arg1, arg2, result);
      if (error < 0)
	{
	  PT_ERRORc (parser, o1, er_msg ());
	  return 0;
	}
      else
	{
	  return 1;
	}

    case PT_SUBSTRING_INDEX:
      error = db_string_substring_index (arg1, arg2, arg3, result);
      if (error < 0)
	{
	  PT_ERRORc (parser, o1, er_msg ());
	  return 0;
	}
      break;

    case PT_MD5:
      error = db_string_md5 (arg1, result);
      if (error < 0)
	{
	  PT_ERRORc (parser, o1, er_msg ());
	  return 0;
	}
      else
	{
	  return 1;
	}

    case PT_LPAD:
      error = db_string_pad (LEADING, arg1, arg2, arg3, result);
      if (error < 0)
	{
	  PT_ERRORc (parser, o1, er_msg ());
	  return 0;
	}
      else
	{
	  return 1;
	}

    case PT_RPAD:
      error = db_string_pad (TRAILING, arg1, arg2, arg3, result);
      if (error < 0)
	{
	  PT_ERRORc (parser, o1, er_msg ());
	  return 0;
	}
      else
	{
	  return 1;
	}

    case PT_REPLACE:
      error = db_string_replace (arg1, arg2, arg3, result);
      if (error < 0)
	{
	  PT_ERRORc (parser, o1, er_msg ());
	  return 0;
	}
      else
	{
	  return 1;
	}

    case PT_TRANSLATE:
      error = db_string_translate (arg1, arg2, arg3, result);
      if (error < 0)
	{
	  PT_ERRORc (parser, o1, er_msg ());
	  return 0;
	}
      else
	{
	  return 1;
	}

    case PT_ADD_MONTHS:
      error = db_add_months (arg1, arg2, result);
      if (error < 0)
	{
	  PT_ERRORc (parser, o1, er_msg ());
	  return 0;
	}
      else
	{
	  return 1;
	}

    case PT_LAST_DAY:
      error = db_last_day (arg1, result);
      if (error < 0)
	{
	  PT_ERRORc (parser, expr, er_msg ());
	  return 0;
	}
      else
	{
	  return 1;
	}

    case PT_UNIX_TIMESTAMP:
      error = db_unix_timestamp (arg1, result);
      if (error < 0)
	{
	  PT_ERRORc (parser, o1, er_msg ());
	  return 0;
	}
      else
	{
	  return 1;
	}

    case PT_STR_TO_DATE:
      error = db_str_to_date (arg1, arg2, arg3, result, NULL);
      if (error < 0)
	{
	  PT_ERRORc (parser, o1, er_msg ());
	  return 0;
	}
      else
	{
	  return 1;
	}

    case PT_TIME_FORMAT:
      error = db_time_format (arg1, arg2, arg3, result, domain);
      if (error < 0)
	{
	  PT_ERRORc (parser, o1, er_msg ());
	  return 0;
	}
      else
	{
	  return 1;
	}

    case PT_TIMESTAMP:
      if (typ1 == DB_TYPE_NULL)
	{
	  db_make_null (result);
	  return 1;
	}

      error = db_timestamp (arg1, arg2, result);
      if (error < 0)
	{
	  PT_ERRORc (parser, o1, er_msg ());
	  return 0;
	}
      else
	{
	  return 1;
	}

    case PT_YEARF:
    case PT_MONTHF:
    case PT_DAYF:
      error = db_get_date_item (arg1, op, result);
      if (error < 0)
	{
	  PT_ERRORc (parser, o1, er_msg ());
	  return 0;
	}
      else
	{
	  return 1;
	}

    case PT_DAYOFMONTH:
      /* day of month is handled like PT_DAYF */
      error = db_get_date_item (arg1, PT_DAYF, result);
      if (error < 0)
	{
	  PT_ERRORc (parser, o1, er_msg ());
	  return 0;
	}
      else
	{
	  return 1;
	}

    case PT_HOURF:
    case PT_MINUTEF:
    case PT_SECONDF:
      error = db_get_time_item (arg1, op, result);
      if (error < 0)
	{
	  PT_ERRORc (parser, o1, er_msg ());
	  return 0;
	}
      else
	{
	  return 1;
	}

    case PT_QUARTERF:
      {
	error = db_get_date_quarter (arg1, result);
	if (error < 0)
	  {
	    PT_ERRORc (parser, o1, er_msg ());
	    return 0;
	  }
	else
	  {
	    return 1;
	  }
      }

    case PT_WEEKDAY:
    case PT_DAYOFWEEK:
      {
	error = db_get_date_weekday (arg1, op, result);
	if (error < 0)
	  {
	    PT_ERRORc (parser, o1, er_msg ());
	    return 0;
	  }
	else
	  {
	    return 1;
	  }
      }

    case PT_DAYOFYEAR:
      {
	error = db_get_date_dayofyear (arg1, result);
	if (error < 0)
	  {
	    PT_ERRORc (parser, o1, er_msg ());
	    return 0;
	  }
	else
	  {
	    return 1;
	  }
      }

    case PT_TODAYS:
      {
	error = db_get_date_totaldays (arg1, result);
	if (error < 0)
	  {
	    PT_ERRORc (parser, o1, er_msg ());
	    return 0;
	  }
	else
	  {
	    return 1;
	  }
      }

    case PT_FROMDAYS:
      {
	error = db_get_date_from_days (arg1, result);
	if (error < 0)
	  {
	    PT_ERRORc (parser, o1, er_msg ());
	    return 0;
	  }
	else
	  {
	    return 1;
	  }
      }

    case PT_TIMETOSEC:
      {
	error = db_convert_time_to_sec (arg1, result);
	if (error < 0)
	  {
	    PT_ERRORc (parser, o1, er_msg ());
	    return 0;
	  }
	else
	  {
	    return 1;
	  }
      }

    case PT_SECTOTIME:
      {
	error = db_convert_sec_to_time (arg1, result);
	if (error < 0)
	  {
	    PT_ERRORc (parser, o1, er_msg ());
	    return 0;
	  }
	else
	  {
	    return 1;
	  }
      }

    case PT_MAKEDATE:
      {
	error = db_add_days_to_year (arg1, arg2, result);
	if (error < 0)
	  {
	    PT_ERRORc (parser, o1, er_msg ());
	    return 0;
	  }
	else
	  {
	    return 1;
	  }
      }

    case PT_MAKETIME:
      {
	error = db_convert_to_time (arg1, arg2, arg3, result);
	if (error < 0)
	  {
	    PT_ERRORc (parser, o1, er_msg ());
	    return 0;
	  }
	else
	  {
	    return 1;
	  }
      }

    case PT_ADDTIME:
      {
	error = db_add_time (arg1, arg2, result, NULL);
	if (error < 0)
	  {
	    PT_ERRORc (parser, o1, er_msg ());
	    return 0;
	  }
	else
	  {
	    return 1;
	  }
      }

    case PT_WEEKF:
      {
	error = db_get_date_week (arg1, arg2, result);
	if (error < 0)
	  {
	    PT_ERRORc (parser, o1, er_msg ());
	    return 0;
	  }
	else
	  {
	    return 1;
	  }
      }

    case PT_SCHEMA:
    case PT_DATABASE:
      db_make_null (result);
      error = db_make_string (&tmp_val, db_get_database_name ());
      if (error < 0)
	{
	  PT_ERRORc (parser, o1, er_msg ());
	  return 0;
	}

      error = db_value_clone (&tmp_val, result);
      if (error < 0)
	{
	  PT_ERRORc (parser, o1, er_msg ());
	  return 0;
	}
      else
	{
	  return 1;
	}

    case PT_VERSION:
      {
	db_make_null (result);
	error = db_make_string (&tmp_val, db_get_database_version ());
	if (error < 0)
	  {
	    PT_ERRORc (parser, o1, er_msg ());
	    return 0;
	  }

	error = db_value_clone (&tmp_val, result);
	if (error < 0)
	  {
	    PT_ERRORc (parser, o1, er_msg ());
	    return 0;
	  }
	else
	  {
	    return 1;
	  }
      }

    case PT_MONTHS_BETWEEN:
      error = db_months_between (arg1, arg2, result);
      if (error < 0)
	{
	  PT_ERRORc (parser, o1, er_msg ());
	  return 0;
	}
      else
	{
	  return 1;
	}

    case PT_FORMAT:
      error = db_format (arg1, arg2, arg3, result, NULL);
      if (error < 0)
	{
	  PT_ERRORc (parser, o1, er_msg ());
	  return 0;
	}
      else
	{
	  return 1;
	}

    case PT_DATE_FORMAT:
      error = db_date_format (arg1, arg2, arg3, result, domain);
      if (error < 0)
	{
	  PT_ERRORc (parser, o1, er_msg ());
	  return 0;
	}
      else
	{
	  return 1;
	}

    case PT_ADDDATE:
      error = db_date_add_interval_days (result, arg1, arg2);
      if (error < 0)
	{
	  PT_ERRORc (parser, o1, er_msg ());
	  return 0;
	}
      else
	{
	  return 1;
	}

    case PT_DATEDIFF:
      error = db_date_diff (arg1, arg2, result);
      if (error < 0)
	{
	  PT_ERRORc (parser, o1, er_msg ());
	  return 0;
	}
      else
	{
	  return 1;
	}
      break;

    case PT_TIMEDIFF:
      error = db_time_diff (arg1, arg2, result);
      if (error < 0)
	{
	  PT_ERRORc (parser, expr, er_msg ());
	  return 0;
	}
      else
	{
	  return 1;
	}
      break;

    case PT_SUBDATE:
      error = db_date_sub_interval_days (result, arg1, arg2);
      if (error < 0)
	{
	  PT_ERRORc (parser, o1, er_msg ());
	  return 0;
	}
      else
	{
	  return 1;
	}

    case PT_DATE_ADD:
      error = db_date_add_interval_expr (result, arg1, arg2,
					 o3->info.expr.qualifier);
      if (error < 0)
	{
	  PT_ERRORc (parser, o1, er_msg ());
	  return 0;
	}
      else
	{
	  return 1;
	}

    case PT_DATE_SUB:
      error = db_date_sub_interval_expr (result, arg1, arg2,
					 o3->info.expr.qualifier);
      if (error < 0)
	{
	  PT_ERRORc (parser, o1, er_msg ());
	  return 0;
	}
      else
	{
	  return 1;
	}

    case PT_SYS_DATE:
      {
	DB_DATETIME *tmp_datetime;

	db_value_domain_init (result, DB_TYPE_DATE, 0, 0);
	tmp_datetime = db_get_datetime (&parser->sys_datetime);
	db_value_put_encoded_date (result, &tmp_datetime->date);

	return 1;
      }
    case PT_UTC_DATE:
      {
	DB_VALUE timezone;
	DB_BIGINT timezone_milis;
	DB_DATETIME db_datetime;
	DB_DATETIME *tmp_datetime;

	DB_DATE db_date;

	/* extract the timezone part */
	db_sys_timezone (&timezone);
	timezone_milis = DB_GET_INT (&timezone) * 60000;
	tmp_datetime = db_get_datetime (&parser->sys_datetime);
	db_add_int_to_datetime (tmp_datetime, timezone_milis, &db_datetime);
	db_date = db_datetime.date;
	DB_MAKE_ENCODED_DATE (result, &db_date);

	return 1;
      }

    case PT_UTC_TIME:
      {
	DB_TIME db_time;
	DB_VALUE timezone;
	int timezone_val;
	DB_DATETIME *tmp_datetime;

	tmp_datetime = db_get_datetime (&parser->sys_datetime);
	db_time = tmp_datetime->time / 1000;
	/* extract the timezone part */
	db_sys_timezone (&timezone);
	timezone_val = DB_GET_INT (&timezone);
	db_time = db_time + timezone_val * 60 + SECONDS_OF_ONE_DAY;
	db_time = db_time % SECONDS_OF_ONE_DAY;

	DB_MAKE_ENCODED_TIME (result, &db_time);
	return 1;
      }

    case PT_SYS_TIME:
      {
	DB_DATETIME *tmp_datetime;
	DB_TIME tmp_time;

	db_value_domain_init (result, DB_TYPE_TIME, 0, 0);
	tmp_datetime = db_get_datetime (&parser->sys_datetime);
	tmp_time = tmp_datetime->time / 1000;
	db_value_put_encoded_time (result, &tmp_time);

	return 1;
      }

    case PT_SYS_TIMESTAMP:
      {
	DB_DATETIME *tmp_datetime;
	DB_DATE tmp_date;
	DB_TIME tmp_time;
	DB_TIMESTAMP tmp_timestamp;

	db_value_domain_init (result, DB_TYPE_TIMESTAMP, 0, 0);
	tmp_datetime = db_get_datetime (&parser->sys_datetime);
	tmp_date = tmp_datetime->date;
	tmp_time = tmp_datetime->time / 1000;
	db_timestamp_encode (&tmp_timestamp, &tmp_date, &tmp_time);
	db_make_timestamp (result, tmp_timestamp);
	return 1;
      }

    case PT_SYS_DATETIME:
      {
	DB_DATETIME *tmp_datetime;

	db_value_domain_init (result, DB_TYPE_DATETIME, 0, 0);
	tmp_datetime = db_get_datetime (&parser->sys_datetime);

	db_make_datetime (result, tmp_datetime);
	return 1;
      }

    case PT_CURRENT_USER:
      {
	const char *username = au_user_name ();

	error = db_make_string (result, username);
	if (error < 0)
	  {
	    db_string_free ((char *) username);
	    PT_ERRORc (parser, o1, er_msg ());
	    return 0;
	  }
	else
	  {
	    result->need_clear = true;
	    return 1;
	  }
      }

    case PT_USER:
      {
	char *user = NULL;

	user = db_get_user_and_host_name ();
	db_make_null (result);

	error = db_make_string (&tmp_val, user);
	if (error < 0)
	  {
	    PT_ERRORc (parser, o1, er_msg ());
	    db_private_free (NULL, user);
	    return 0;
	  }
	tmp_val.need_clear = true;

	error = pr_clone_value (&tmp_val, result);
	if (error < 0)
	  {
	    PT_ERRORc (parser, o1, er_msg ());
	    return 0;
	  }
	else
	  {
	    return 1;
	  }
      }

    case PT_ROW_COUNT:
      {
	int row_count = db_get_row_count_cache ();
	if (row_count == DB_ROW_COUNT_NOT_SET)
	  {
	    /* We do not have a cached value of row count. In this case we
	     * read it from the server and update the cached value
	     */
	    db_get_row_count (&row_count);
	    db_update_row_count_cache (row_count);
	  }
	db_make_int (result, row_count);
	return 1;
      }

    case PT_LAST_INSERT_ID:
      if (csession_get_last_insert_id (&tmp_val,
				       !obt_Last_insert_id_generated)
	  != NO_ERROR)
	{
	  return 0;
	}
      else
	{
	  db_value_clone (&tmp_val, result);
	  return 1;
	}

    case PT_LOCAL_TRANSACTION_ID:
      db_value_clone (&parser->local_transaction_id, result);
      return 1;

    case PT_TO_CHAR:
      error = db_to_char (arg1, arg2, arg3, result, domain);
      if (error < 0)
	{
	  PT_ERRORc (parser, o1, er_msg ());
	  return 0;
	}
      else
	{
	  return 1;
	}

    case PT_BIT_TO_BLOB:
      error = db_bit_to_blob (arg1, result);
      if (error < 0)
	{
	  PT_ERRORc (parser, o1, er_msg ());
	  return 0;
	}
      else
	{
	  return 1;
	}

    case PT_CHAR_TO_BLOB:
      error = db_char_to_blob (arg1, result);
      if (error < 0)
	{
	  PT_ERRORc (parser, o1, er_msg ());
	  return 0;
	}
      else
	{
	  return 1;
	}

    case PT_BLOB_FROM_FILE:
      error = db_blob_from_file (arg1, result);
      if (error < 0)
	{
	  PT_ERRORc (parser, o1, er_msg ());
	  return 0;
	}
      else
	{
	  return 1;
	}

    case PT_BLOB_TO_BIT:
      error = db_blob_to_bit (arg1, arg2, result);
      if (error < 0)
	{
	  PT_ERRORc (parser, o1, er_msg ());
	  return 0;
	}
      else
	{
	  return 1;
	}

    case PT_BLOB_LENGTH:
      error = db_blob_length (arg1, result);
      if (error < 0)
	{
	  PT_ERRORc (parser, o1, er_msg ());
	  return 0;
	}
      else
	{
	  return 1;
	}

    case PT_CHAR_TO_CLOB:
      error = db_char_to_clob (arg1, result);
      if (error < 0)
	{
	  PT_ERRORc (parser, o1, er_msg ());
	  return 0;
	}
      else
	{
	  return 1;
	}

    case PT_CLOB_FROM_FILE:
      error = db_clob_from_file (arg1, result);
      if (error < 0)
	{
	  PT_ERRORc (parser, o1, er_msg ());
	  return 0;
	}
      else
	{
	  return 1;
	}

    case PT_CLOB_TO_CHAR:
      error = db_clob_to_char (arg1, arg2, result);
      if (error < 0)
	{
	  PT_ERRORc (parser, o1, er_msg ());
	  return 0;
	}
      else
	{
	  return 1;
	}

    case PT_CLOB_LENGTH:
      error = db_clob_length (arg1, result);
      if (error < 0)
	{
	  PT_ERRORc (parser, o1, er_msg ());
	  return 0;
	}
      else
	{
	  return 1;
	}

    case PT_TO_DATE:
      error = db_to_date (arg1, arg2, arg3, result);
      if (error < 0)
	{
	  PT_ERRORc (parser, o1, er_msg ());
	  return 0;
	}
      else
	{
	  return 1;
	}

    case PT_TO_TIME:
      error = db_to_time (arg1, arg2, arg3, result);
      if (error < 0)
	{
	  PT_ERRORc (parser, o1, er_msg ());
	  return 0;
	}
      else
	{
	  return 1;
	}

    case PT_TO_TIMESTAMP:
      error = db_to_timestamp (arg1, arg2, arg3, result);
      if (error < 0)
	{
	  PT_ERRORc (parser, o1, er_msg ());
	  return 0;
	}
      else
	{
	  return 1;
	}

    case PT_TO_DATETIME:
      error = db_to_datetime (arg1, arg2, arg3, result);
      if (error < 0)
	{
	  PT_ERRORc (parser, o1, er_msg ());
	  return 0;
	}
      else
	{
	  return 1;
	}

    case PT_TO_NUMBER:
      error = db_to_number (arg1, arg2, arg3, result);
      if (error < 0)
	{
	  PT_ERRORc (parser, o1, er_msg ());
	  return 0;
	}
      else
	{
	  return 1;
	}

    case PT_EVALUATE_VARIABLE:
      {
	int err = 0;
	assert (DB_VALUE_DOMAIN_TYPE (arg1) == DB_TYPE_CHAR);
	err = db_get_variable (arg1, result);
	if (err != NO_ERROR)
	  {
	    PT_ERRORc (parser, o1, er_msg ());
	    return 0;
	  }
	return 1;
      }

    case PT_CAST:
      if (TP_DOMAIN_TYPE (domain) == DB_TYPE_VARIABLE)
	{
	  return 0;
	}
      status = tp_value_strict_cast (arg1, result, domain);
      if (status != DOMAIN_COMPATIBLE)
	{
	  if (PT_EXPR_INFO_IS_FLAGED (expr, PT_EXPR_INFO_CAST_NOFAIL))
	    {
	      db_make_null (result);
	      return 1;
	    }
	  PT_ERRORmf2 (parser, o1, MSGCAT_SET_PARSER_SEMANTIC,
		       MSGCAT_SEMANTIC_CANT_COERCE_TO,
		       pt_short_print (parser, o1), pt_show_type_enum (rTyp));
	  return 0;
	}
      else
	{
	  return 1;
	}

    case PT_CASE:
    case PT_DECODE:
      /* If arg3 = NULL, then arg2 = NULL and arg1 != NULL.  For this case,
       * we've already finished checking case_search_condition. */
      if (arg3 && (DB_VALUE_TYPE (arg3) == DB_TYPE_INTEGER
		   && DB_GET_INT (arg3) != 0))
	{
	  if (tp_value_coerce (arg1, result, domain) != DOMAIN_COMPATIBLE)
	    {
	      PT_ERRORmf2 (parser, o1, MSGCAT_SET_PARSER_SEMANTIC,
			   MSGCAT_SEMANTIC_CANT_COERCE_TO,
			   pt_short_print (parser, o1),
			   pt_show_type_enum (rTyp));
	      return 0;
	    }
	}
      else
	{
	  if (tp_value_coerce (arg2, result, domain) != DOMAIN_COMPATIBLE)
	    {
	      PT_ERRORmf2 (parser, o2, MSGCAT_SET_PARSER_SEMANTIC,
			   MSGCAT_SEMANTIC_CANT_COERCE_TO,
			   pt_short_print (parser, o2),
			   pt_show_type_enum (rTyp));
	      return 0;
	    }
	}
      break;

    case PT_NULLIF:
      if (tp_value_compare (arg1, arg2, 1, 0) == DB_EQ)
	{
	  db_make_null (result);
	}
      else
	{
	  pr_clone_value ((DB_VALUE *) arg1, result);
	}
      return 1;

    case PT_EXTRACT:
      if (typ1 == DB_TYPE_NULL)
	{
	  db_make_null (result);
	}
      else
	{
	  DB_DATE date;
	  DB_TIME time;
	  DB_UTIME *utime;
	  DB_DATETIME *datetime;
	  int year, month, day, hour, minute, second, millisecond;

	  switch (typ1)
	    {
	    case DB_TYPE_TIME:
	      time = *DB_GET_TIME (arg1);
	      db_time_decode (&time, &hour, &minute, &second);
	      break;

	    case DB_TYPE_DATE:
	      date = *DB_GET_DATE (arg1);
	      db_date_decode (&date, &month, &day, &year);
	      break;

	    case DB_TYPE_UTIME:
	      utime = DB_GET_UTIME (arg1);
	      db_timestamp_decode (utime, &date, &time);
	      if (qualifier == PT_YEAR
		  || qualifier == PT_MONTH || qualifier == PT_DAY)
		db_date_decode (&date, &month, &day, &year);
	      else
		db_time_decode (&time, &hour, &minute, &second);
	      break;

	    case DB_TYPE_DATETIME:
	      datetime = DB_GET_DATETIME (arg1);
	      db_datetime_decode (datetime, &month, &day, &year,
				  &hour, &minute, &second, &millisecond);
	      break;

	    default:
	      return 0;
	    }

	  switch (qualifier)
	    {
	    case PT_YEAR:
	      db_make_int (result, year);
	      break;

	    case PT_MONTH:
	      db_make_int (result, month);
	      break;

	    case PT_DAY:
	      db_make_int (result, day);
	      break;

	    case PT_HOUR:
	      db_make_int (result, hour);
	      break;

	    case PT_MINUTE:
	      db_make_int (result, minute);
	      break;

	    case PT_SECOND:
	      db_make_int (result, second);
	      break;

	    case PT_MILLISECOND:
	      db_make_int (result, millisecond);
	      break;

	    default:
	      return 0;
	    }
	}			/* else */
      break;

    case PT_EQ:
    case PT_NE:
    case PT_GE:
    case PT_GT:
    case PT_LT:
    case PT_LE:

    case PT_SETEQ:
    case PT_SETNEQ:
    case PT_SUPERSETEQ:
    case PT_SUPERSET:
    case PT_SUBSETEQ:
    case PT_SUBSET:

    case PT_NULLSAFE_EQ:

    case PT_IS_IN:
    case PT_IS_NOT_IN:
    case PT_EQ_SOME:
    case PT_NE_SOME:
    case PT_GT_SOME:
    case PT_GE_SOME:
    case PT_LT_SOME:
    case PT_LE_SOME:
    case PT_EQ_ALL:
    case PT_NE_ALL:
    case PT_GT_ALL:
    case PT_GE_ALL:
    case PT_LT_ALL:
    case PT_LE_ALL:
    case PT_LIKE:
    case PT_NOT_LIKE:
    case PT_RLIKE:
    case PT_NOT_RLIKE:
    case PT_RLIKE_BINARY:
    case PT_NOT_RLIKE_BINARY:
    case PT_BETWEEN:
    case PT_NOT_BETWEEN:
    case PT_RANGE:

      if (op != PT_BETWEEN && op != PT_NOT_BETWEEN
	  && op != PT_RANGE && (op != PT_EQ || qualifier != PT_EQ_TORDER))
	{
	  if ((typ1 == DB_TYPE_NULL || typ2 == DB_TYPE_NULL)
	      && op != PT_NULLSAFE_EQ)
	    {
	      db_make_null (result);	/* NULL comp_op any = NULL */
	      break;
	    }
	}

      switch (op)
	{
	case PT_EQ:
	  if (qualifier == PT_EQ_TORDER)
	    {
	      cmp_result =
		(DB_VALUE_COMPARE_RESULT) tp_value_compare (arg1, arg2, 1, 1);
	      cmp =
		(cmp_result == DB_UNK) ? -1 : (cmp_result == DB_EQ) ? 1 : 0;
	      break;
	    }

	  /* fall through */
	case PT_SETEQ:
	  cmp_result =
	    (DB_VALUE_COMPARE_RESULT) db_value_compare (arg1, arg2);
	  cmp = (cmp_result == DB_UNK) ? -1 : (cmp_result == DB_EQ) ? 1 : 0;
	  break;

	case PT_NE:
	case PT_SETNEQ:
	  cmp_result =
	    (DB_VALUE_COMPARE_RESULT) db_value_compare (arg1, arg2);
	  cmp = (cmp_result == DB_UNK) ? -1 : (cmp_result != DB_EQ) ? 1 : 0;
	  break;

	case PT_NULLSAFE_EQ:
	  if ((o1 && o1->node_type != PT_VALUE)
	      || (o2 && o2->node_type != PT_VALUE))
	    {
	      return 0;
	    }
	  if (arg1 == NULL || arg1->domain.general_info.is_null)
	    {
	      if (arg2 == NULL || arg2->domain.general_info.is_null)
		{
		  cmp_result = DB_EQ;
		}
	      else
		{
		  cmp_result = DB_NE;
		}
	    }
	  else
	    {
	      if (arg2 == NULL || arg2->domain.general_info.is_null)
		{
		  cmp_result = DB_NE;
		}
	      else
		{
		  cmp_result = (DB_VALUE_COMPARE_RESULT)
		    db_value_compare (arg1, arg2);
		}
	    }
	  cmp = (cmp_result == DB_EQ) ? 1 : 0;
	  break;

	case PT_SUPERSETEQ:
	  cmp_result = (DB_VALUE_COMPARE_RESULT) db_set_compare (arg1, arg2);
	  cmp = (cmp_result == DB_UNK) ? -1 : (cmp_result == DB_EQ
					       || cmp_result ==
					       DB_SUPERSET) ? 1 : 0;
	  break;

	case PT_SUPERSET:
	  cmp_result = (DB_VALUE_COMPARE_RESULT) db_set_compare (arg1, arg2);
	  cmp =
	    (cmp_result == DB_UNK) ? -1 : (cmp_result == DB_SUPERSET) ? 1 : 0;
	  break;

	case PT_SUBSET:
	  cmp_result = (DB_VALUE_COMPARE_RESULT) db_set_compare (arg1, arg2);
	  cmp =
	    (cmp_result == DB_UNK) ? -1 : (cmp_result == DB_SUBSET) ? 1 : 0;
	  break;

	case PT_SUBSETEQ:
	  cmp_result = (DB_VALUE_COMPARE_RESULT) db_set_compare (arg1, arg2);
	  cmp = (cmp_result == DB_UNK) ? -1 : (cmp_result == DB_EQ
					       || cmp_result ==
					       DB_SUBSET) ? 1 : 0;
	  break;

	case PT_GE:
	  cmp_result =
	    (DB_VALUE_COMPARE_RESULT) db_value_compare (arg1, arg2);
	  cmp = (cmp_result == DB_UNK) ? -1 : (cmp_result == DB_EQ
					       || cmp_result ==
					       DB_GT) ? 1 : 0;
	  break;

	case PT_GT:
	  cmp_result =
	    (DB_VALUE_COMPARE_RESULT) db_value_compare (arg1, arg2);
	  cmp = (cmp_result == DB_UNK) ? -1 : (cmp_result == DB_GT) ? 1 : 0;
	  break;

	case PT_LE:
	  cmp_result =
	    (DB_VALUE_COMPARE_RESULT) db_value_compare (arg1, arg2);
	  cmp = (cmp_result == DB_UNK) ? -1 : (cmp_result == DB_EQ
					       || cmp_result ==
					       DB_LT) ? 1 : 0;
	  break;

	case PT_LT:
	  cmp_result =
	    (DB_VALUE_COMPARE_RESULT) db_value_compare (arg1, arg2);
	  cmp = (cmp_result == DB_UNK) ? -1 : (cmp_result == DB_LT) ? 1 : 0;
	  break;

	case PT_EQ_SOME:
	case PT_IS_IN:
	  cmp = set_issome (arg1, db_get_set (arg2), PT_EQ_SOME, 1);
	  break;

	case PT_NE_SOME:
	case PT_GE_SOME:
	case PT_GT_SOME:
	case PT_LT_SOME:
	case PT_LE_SOME:
	  cmp = set_issome (arg1, db_get_set (arg2), op, 1);
	  break;

	case PT_EQ_ALL:
	  cmp = set_issome (arg1, db_get_set (arg2), PT_NE_SOME, 1);
	  if (cmp == 1)
	    cmp = 0;
	  else if (cmp == 0)
	    cmp = 1;
	  break;

	case PT_NE_ALL:
	case PT_IS_NOT_IN:
	  cmp = set_issome (arg1, db_get_set (arg2), PT_EQ_SOME, 1);
	  if (cmp == 1)
	    cmp = 0;
	  else if (cmp == 0)
	    cmp = 1;
	  break;

	case PT_GE_ALL:
	  cmp = set_issome (arg1, db_get_set (arg2), PT_LT_SOME, 1);
	  if (cmp == 1)
	    cmp = 0;
	  else if (cmp == 0)
	    cmp = 1;
	  break;

	case PT_GT_ALL:
	  cmp = set_issome (arg1, db_get_set (arg2), PT_LE_SOME, 1);
	  if (cmp == 1)
	    cmp = 0;
	  else if (cmp == 0)
	    cmp = 1;
	  break;

	case PT_LT_ALL:
	  cmp = set_issome (arg1, db_get_set (arg2), PT_GE_SOME, 1);
	  if (cmp == 1)
	    cmp = 0;
	  else if (cmp == 0)
	    cmp = 1;
	  break;

	case PT_LE_ALL:
	  cmp = set_issome (arg1, db_get_set (arg2), PT_GT_SOME, 1);
	  if (cmp == 1)
	    cmp = 0;
	  else if (cmp == 0)
	    cmp = 1;
	  break;

	case PT_LIKE:
	case PT_NOT_LIKE:
	  {
	    DB_VALUE *esc_char = arg3;
	    DB_VALUE slash_char;
	    char const *slash_str = "\\";

	    if (prm_get_bool_value (PRM_ID_NO_BACKSLASH_ESCAPES) == false
		&& DB_IS_NULL (esc_char))
	      {
		INTL_CODESET arg1_cs = DB_IS_NULL (arg1) ? LANG_SYS_CODESET :
		  DB_GET_STRING_CODESET (arg1);
		int arg1_coll = DB_IS_NULL (arg1) ? LANG_SYS_COLLATION :
		  DB_GET_STRING_COLLATION (arg1);
		/* when compat_mode=mysql, the slash '\\' is an escape character for
		   LIKE pattern, unless user explicitly specifies otherwise. */
		esc_char = &slash_char;
		if (arg1->domain.general_info.type == DB_TYPE_NCHAR
		    || arg1->domain.general_info.type == DB_TYPE_VARNCHAR)
		  {
		    DB_MAKE_NCHAR (esc_char, 1, slash_str, 1, arg1_cs,
				   arg1_coll);
		  }
		else
		  {
		    DB_MAKE_CHAR (esc_char, 1, slash_str, 1, arg1_cs,
				  arg1_coll);
		  }

		esc_char->need_clear = false;
	      }

	    if (db_string_like (arg1, arg2, esc_char, &cmp))
	      {
		/* db_string_like() also checks argument types */
		return 0;
	      }
	    cmp = ((op == PT_LIKE && cmp == V_TRUE)
		   || (op == PT_NOT_LIKE && cmp == V_FALSE)) ? 1 : 0;
	  }
	  break;

	case PT_RLIKE:
	case PT_NOT_RLIKE:
	case PT_RLIKE_BINARY:
	case PT_NOT_RLIKE_BINARY:
	  {
	    int err = db_string_rlike (arg1, arg2, arg3, NULL, NULL, &cmp);

	    switch (err)
	      {
	      case NO_ERROR:
		break;
	      case ER_REGEX_COMPILE_ERROR:	/* fall through */
	      case ER_REGEX_EXEC_ERROR:
		PT_ERRORc (parser, o1, er_msg ());
	      default:
		return 0;
	      }

	    /* negate result if using NOT syntax of operator */
	    if (op == PT_NOT_RLIKE || op == PT_NOT_RLIKE_BINARY)
	      {
		switch (cmp)
		  {
		  case V_TRUE:
		    cmp = V_FALSE;
		    break;

		  case V_FALSE:
		    cmp = V_TRUE;
		    break;

		  default:
		    break;
		  }
	      }
	  }
	  break;

	case PT_BETWEEN:
	  /* special handling for PT_BETWEEN and PT_NOT_BETWEEN */
	  cmp_result =
	    (DB_VALUE_COMPARE_RESULT) db_value_compare (arg2, arg1);
	  cmp_result2 =
	    (DB_VALUE_COMPARE_RESULT) db_value_compare (arg1, arg3);
	  if (((cmp_result == DB_UNK) && (cmp_result2 == DB_UNK))
	      || ((cmp_result == DB_UNK)
		  && ((cmp_result2 == DB_LT)
		      || (cmp_result2 == DB_EQ)))
	      || ((cmp_result2 == DB_UNK)
		  && ((cmp_result == DB_LT) || (cmp_result == DB_EQ))))
	    {
	      cmp = -1;
	    }
	  else if (((cmp_result != DB_UNK)
		    &&
		    (!((cmp_result == DB_LT) || (cmp_result == DB_EQ))))
		   || ((cmp_result2 != DB_UNK)
		       &&
		       (!((cmp_result2 == DB_LT) || (cmp_result2 == DB_EQ)))))
	    {
	      cmp = 0;
	    }
	  else
	    {
	      cmp = 1;
	    }
	  break;

	case PT_NOT_BETWEEN:
	  /* special handling for PT_BETWEEN and PT_NOT_BETWEEN */
	  cmp_result =
	    (DB_VALUE_COMPARE_RESULT) db_value_compare (arg2, arg1);
	  cmp_result2 =
	    (DB_VALUE_COMPARE_RESULT) db_value_compare (arg1, arg3);
	  if (((cmp_result == DB_UNK) && (cmp_result2 == DB_UNK))
	      || ((cmp_result == DB_UNK)
		  && ((cmp_result2 == DB_LT)
		      || (cmp_result2 == DB_EQ)))
	      || ((cmp_result2 == DB_UNK)
		  && ((cmp_result == DB_LT) || (cmp_result == DB_EQ))))
	    {
	      cmp = -1;
	    }
	  else if (((cmp_result != DB_UNK)
		    &&
		    (!((cmp_result == DB_LT) || (cmp_result == DB_EQ))))
		   || ((cmp_result2 != DB_UNK)
		       &&
		       (!((cmp_result2 == DB_LT) || (cmp_result2 == DB_EQ)))))
	    {
	      cmp = 1;
	    }
	  else
	    {
	      cmp = 0;
	    }
	  break;

	case PT_RANGE:
	  break;

	default:
	  return 0;
	}

      if (cmp == 1)
	db_make_int (result, 1);
      else if (cmp == 0)
	db_make_int (result, 0);
      else
	db_make_null (result);
      break;

    case PT_INDEX_CARDINALITY:
      /* constant folding for this expression is never performed :
       * is always resolved on server*/
      return 0;
    case PT_LIST_DBS:
    case PT_ASSIGN:
    case PT_LIKE_ESCAPE:
    case PT_BETWEEN_AND:
    case PT_BETWEEN_GE_LE:
    case PT_BETWEEN_GE_LT:
    case PT_BETWEEN_GT_LE:
    case PT_BETWEEN_GT_LT:
    case PT_BETWEEN_EQ_NA:
    case PT_BETWEEN_INF_LE:
    case PT_BETWEEN_INF_LT:
    case PT_BETWEEN_GE_INF:
    case PT_BETWEEN_GT_INF:
      /* these don't need to be handled */
      return 0;

    case PT_TO_ENUMERATION_VALUE:
      {
	TP_DOMAIN *enum_domain = NULL;

	assert (expr->info.expr.arg1 != NULL);
	assert (expr->data_type != NULL);

	enum_domain = pt_data_type_to_db_domain (parser, expr->data_type,
						 NULL);
	if (enum_domain == NULL)
	  {
	    return 0;
	  }

	enum_domain = tp_domain_cache (enum_domain);

	error = db_value_to_enumeration_value (arg1, result, enum_domain);
	if (error < 0)
	  {
	    PT_ERRORc (parser, o1, er_msg ());
	    return 0;
	  }
	else
	  {
	    if (DB_GET_ENUM_SHORT (result) == 0)
	      {
		/* To avoid coercing result to enumeration type later on, we
		 * consider that this expression cannot be folded
		 */
		return 0;
	      }
	    else
	      {
		return 1;
	      }
	  }
      }
      break;

    case PT_INET_ATON:
      error = db_inet_aton (result, arg1);
      if (error != NO_ERROR)
	{
	  PT_ERRORc (parser, o1, er_msg ());
	  return 0;
	}
      break;

    case PT_INET_NTOA:
      error = db_inet_ntoa (result, arg1);
      if (error != NO_ERROR)
	{
	  PT_ERRORc (parser, o1, er_msg ());
	  return 0;
	}
      break;

    case PT_COERCIBILITY:
      /* this expression should always be folded to constant */
      assert (false);
      break;

    case PT_CHARSET:
    case PT_COLLATION:
      error = db_get_cs_coll_info (result, arg1, (op == PT_CHARSET) ? 0 : 1);
      if (error != NO_ERROR)
	{
	  PT_ERRORc (parser, o1, er_msg ());
	  return 0;
	}
      break;

    default:
      break;
    }

  return 1;

overflow:
  PT_ERRORmf (parser, o1, MSGCAT_SET_PARSER_SEMANTIC,
	      MSGCAT_SEMANTIC_DATA_OVERFLOW_ON, pt_show_type_enum (rTyp));
  return 0;
}

/*
 * pt_fold_const_expr () - evaluate constant expression
 *   return: the evaluated expression, if successful,
 *           unchanged expr, if not successful.
 *   parser(in): parser global context info for reentrancy
 *   expr(in): a parse tree representation of a constant expression
 */

static PT_NODE *
pt_fold_const_expr (PARSER_CONTEXT * parser, PT_NODE * expr, void *arg)
{
  PT_TYPE_ENUM type1, type2 = PT_TYPE_NONE, type3, result_type;
  PT_NODE *opd1 = NULL, *opd2 = NULL, *opd3 = NULL, *result = NULL;
  DB_VALUE dummy, dbval_res, *arg1, *arg2, *arg3;
  PT_OP_TYPE op;
  PT_NODE *expr_next;
  int line, column;
  short location;
  const char *alias_print;
  unsigned is_hidden_column;

  if (expr == NULL)
    {
      return expr;
    }

  if (expr->node_type != PT_EXPR)
    {
      return expr;
    }

  if (expr->do_not_fold)
    {
      return expr;
    }

  location = expr->info.expr.location;

  db_make_null (&dbval_res);

  line = expr->line_number;
  column = expr->column_number;
  alias_print = expr->alias_print;
  expr_next = expr->next;
  expr->next = NULL;
  result_type = expr->type_enum;
  result = expr;
  is_hidden_column = expr->is_hidden_column;

  op = expr->info.expr.op;

  if (op == PT_FUNCTION_HOLDER)
    {
      assert (expr->info.expr.arg1 != NULL);

      if (expr->info.expr.arg1->node_type == PT_VALUE)
	{
	  /* const folding OK , replace current node with arg1 VALUE */
	  result = parser_copy_tree (parser, expr->info.expr.arg1);
	  result->info.value.location = location;
	  result->is_hidden_column = is_hidden_column;
	  if (result->info.value.text == NULL)
	    {
	      result->info.value.text =
		pt_append_string (parser, NULL, result->alias_print);
	    }
	  parser_free_tree (parser, expr);
	}
      else if (expr->info.expr.arg1->node_type != PT_FUNCTION)
	{
	  assert (false);
	}
      goto end;
    }
  /* special handling for only one range - convert to comp op */
  if (op == PT_RANGE)
    {
      PT_NODE *between_and;
      PT_OP_TYPE between_op;

      between_and = expr->info.expr.arg2;
      between_op = between_and->info.expr.op;
      if (between_and->or_next == NULL)
	{			/* has only one range */
	  opd1 = expr->info.expr.arg1;
	  opd2 = between_and->info.expr.arg1;
	  opd3 = between_and->info.expr.arg2;
	  if (opd1 && opd1->node_type == PT_VALUE
	      && opd2 && opd2->node_type == PT_VALUE)
	    {			/* both const */
	      if (between_op == PT_BETWEEN_EQ_NA
		  || between_op == PT_BETWEEN_GT_INF
		  || between_op == PT_BETWEEN_GE_INF
		  || between_op == PT_BETWEEN_INF_LT
		  || between_op == PT_BETWEEN_INF_LE)
		{
		  /* convert to comp op */
		  between_and->info.expr.arg1 = NULL;
		  parser_free_tree (parser, between_and);
		  expr->info.expr.arg2 = opd2;
		  op = expr->info.expr.op =
		    (between_op == PT_BETWEEN_EQ_NA) ? PT_EQ :
		    (between_op == PT_BETWEEN_GT_INF) ? PT_GT :
		    (between_op == PT_BETWEEN_GE_INF) ? PT_GE :
		    (between_op == PT_BETWEEN_INF_LT) ? PT_LT : PT_LE;
		}
	      else if (between_op == PT_BETWEEN_GE_LE)
		{
		  if (opd3 && opd3->node_type == PT_VALUE)
		    {
		      /* convert to between op */
		      between_and->info.expr.op = PT_BETWEEN_AND;
		      op = expr->info.expr.op = PT_BETWEEN;
		    }
		}
	    }
	}
    }
  else if (op == PT_NEXT_VALUE || op == PT_CURRENT_VALUE
	   || op == PT_BIT_TO_BLOB || op == PT_CHAR_TO_BLOB
	   || op == PT_BLOB_TO_BIT || op == PT_BLOB_LENGTH
	   || op == PT_CHAR_TO_CLOB || op == PT_CLOB_TO_CHAR
	   || op == PT_CLOB_LENGTH || op == PT_EXEC_STATS)
    {
      goto end;
    }
  else if (op == PT_ROW_COUNT)
    {
      int row_count = db_get_row_count_cache ();
      if (row_count == DB_ROW_COUNT_NOT_SET)
	{
	  /* Read the value from the server and cache it */
	  db_get_row_count (&row_count);
	  db_update_row_count_cache (row_count);
	}
      DB_MAKE_INT (&dbval_res, row_count);
      result = pt_dbval_to_value (parser, &dbval_res);
      goto end;
    }
  else if (op == PT_COERCIBILITY)
    {
      int coll_id;
      INTL_CODESET cs;
      PT_COLL_COERC_LEV coerc;

      if (pt_get_collation_info (expr->info.expr.arg1, &coll_id, &cs, &coerc))
	{
	  DB_MAKE_INT (&dbval_res, (int) coerc);
	}
      else
	{
	  if (prm_get_bool_value (PRM_ID_RETURN_NULL_ON_FUNCTION_ERRORS))
	    {
	      DB_MAKE_NULL (&dbval_res);
	    }
	  else
	    {
	      er_set (ER_WARNING_SEVERITY, ARG_FILE_LINE,
		      ER_OBJ_INVALID_ARGUMENTS, 0);
	      PT_ERRORc (parser, expr, er_msg ());
	      goto end;
	    }
	}

      result = pt_dbval_to_value (parser, &dbval_res);
      goto end;
    }

  opd1 = expr->info.expr.arg1;

  if (opd1 && op == PT_DEFAULTF)
    {
      result = parser_copy_tree (parser, opd1->info.name.default_value);
      if (result == NULL)
	{
	  PT_ERRORc (parser, expr, er_msg ());
	  return expr;
	}
      goto end;
    }

  if (op == PT_OID_OF_DUPLICATE_KEY)
    {
      OID null_oid;
      PT_NODE *tmp_value = parser_new_node (parser, PT_VALUE);

      if (tmp_value == NULL)
	{
	  PT_ERRORc (parser, expr, er_msg ());
	  return expr;
	}

      /* a NULL OID is returned; the resulting PT_VALUE node will be
         replaced with a PT_HOST_VAR by the auto parameterization step
         because of the special force_auto_parameterize flag.
         Also see and pt_dup_key_update_stmt () and
         qo_optimize_queries () */
      tmp_value->type_enum = PT_TYPE_OBJECT;
      OID_SET_NULL (&null_oid);
      DB_MAKE_OID (&tmp_value->info.value.db_value, &null_oid);
      tmp_value->info.value.db_value_is_initialized = true;
      tmp_value->data_type = parser_copy_tree (parser, expr->data_type);
      if (tmp_value->data_type == NULL)
	{
	  PT_ERRORc (parser, expr, er_msg ());
	  parser_free_tree (parser, tmp_value);
	  tmp_value = NULL;
	  return expr;
	}
      result = tmp_value;
      goto end;
    }

  if (opd1 && opd1->node_type == PT_VALUE)
    {
      arg1 = pt_value_to_db (parser, opd1);
      type1 = opd1->type_enum;
    }
  else
    {
      if (op == PT_EQ && expr->info.expr.qualifier == PT_EQ_TORDER)
	{
	  return result;
	}
      db_make_null (&dummy);
      arg1 = &dummy;
      type1 = PT_TYPE_NULL;
    }

  /* special handling for PT_BETWEEN and PT_NOT_BETWEEN */
  opd2 = (op == PT_BETWEEN || op == PT_NOT_BETWEEN)
    ? expr->info.expr.arg2->info.expr.arg1 : expr->info.expr.arg2;


  if (opd2 && opd2->node_type == PT_VALUE)
    {
      arg2 = pt_value_to_db (parser, opd2);
      type2 = opd2->type_enum;
    }
  else
    {
      switch (op)
	{
	case PT_TRIM:
	case PT_LTRIM:
	case PT_RTRIM:
	  {
	    INTL_CODESET arg1_cs = DB_IS_NULL (arg1) ? LANG_SYS_CODESET :
	      DB_GET_STRING_CODESET (arg1);
	    int arg1_coll = DB_IS_NULL (arg1) ? LANG_SYS_COLLATION :
	      DB_GET_STRING_COLLATION (arg1);

	    if (type1 == PT_TYPE_NCHAR || type1 == PT_TYPE_VARNCHAR)
	      {
		db_make_varnchar (&dummy, 1, (char *) " ", 1, arg1_cs,
				  arg1_coll);
		type2 = PT_TYPE_VARNCHAR;
	      }
	    else
	      {
		db_make_varchar (&dummy, 1, (char *) " ", 0, arg1_cs,
				 arg1_coll);
		type2 = PT_TYPE_VARCHAR;
	      }
	    arg2 = &dummy;
	  }
	  break;

	case PT_FROM_UNIXTIME:
	  arg2 = NULL;
	  break;

	case PT_LIKE_LOWER_BOUND:
	case PT_LIKE_UPPER_BOUND:
	case PT_TIMESTAMP:
	  /* If an operand2 exists and it's not a value, do not fold. */
	  if (opd2 != NULL)
	    {
	      goto end;
	    }
	  arg2 = NULL;
	  break;

	case PT_INCR:
	case PT_DECR:
	  {
	    PT_NODE *entity = NULL, *top, *spec;
	    PT_NODE *dtype;
	    const char *attr_name;
	    SEMANTIC_CHK_INFO *sc_info = (SEMANTIC_CHK_INFO *) arg;
	    int attrid, shared;
	    DB_DOMAIN *dom;

	    /* add an argument, oid of instance to do increment */
	    if (opd1 != NULL && opd1->node_type == PT_NAME)
	      {
		opd2 = pt_name (parser, "");
		if (opd2 == NULL)
		  {
		    PT_ERRORc (parser, expr, er_msg ());
		    return expr;
		  }

		dtype = parser_new_node (parser, PT_DATA_TYPE);
		if (dtype == NULL)
		  {
		    PT_ERRORc (parser, expr, er_msg ());
		    return expr;
		  }

		if (sc_info && (top = sc_info->top_node)
		    && (top->node_type == PT_SELECT))
		  {
		    /* if given top node, find domain class,
		     * and check if it is a derived class */
		    spec = pt_find_entity (parser,
					   top->info.query.q.select.from,
					   opd1->info.name.spec_id);
		    if (spec)
		      {
			entity = spec->info.spec.entity_name;
		      }
		  }
		else
		  {
		    entity = pt_name (parser, opd1->info.name.resolved);
		    if (entity == NULL)
		      {
			PT_ERRORc (parser, expr, er_msg ());
			return expr;
		      }
		    entity->info.name.db_object =
		      db_find_class (entity->info.name.original);
		  }

		if (entity == NULL || entity->info.name.db_object == NULL)
		  {
		    PT_ERRORf (parser, expr,
			       "Attribute of derived class "
			       "is not permitted in %s()",
			       (op == PT_INCR ? "INCR" : "DECR"));
		    return expr;
		  }
		dtype->type_enum = PT_TYPE_OBJECT;
		dtype->info.data_type.entity = entity;
		dtype->info.data_type.virt_type_enum = PT_TYPE_OBJECT;

		opd2->data_type = dtype;
		opd2->type_enum = PT_TYPE_OBJECT;
		opd2->info.name.meta_class = PT_OID_ATTR;
		opd2->info.name.spec_id = opd1->info.name.spec_id;
		opd2->info.name.resolved =
		  pt_append_string (parser, NULL, opd1->info.name.resolved);
		if (opd2->info.name.resolved == NULL)
		  {
		    PT_ERRORc (parser, expr, er_msg ());
		    return expr;
		  }

		attr_name = opd1->info.name.original;
		expr->info.expr.arg2 = opd2;
	      }
	    else if (opd1 != NULL && opd1->node_type == PT_DOT_)
	      {
		PT_NODE *arg2, *arg1 = opd1->info.dot.arg1;

		opd2 = parser_copy_tree_list (parser, arg1);
		if (opd2 == NULL)
		  {
		    PT_ERRORc (parser, expr, er_msg ());
		    return expr;
		  }

		if (opd2->node_type == PT_DOT_)
		  {
		    arg2 = opd2->info.dot.arg2;
		    entity = arg2->data_type->info.data_type.entity;
		  }
		else
		  {
		    entity = opd2->data_type->info.data_type.entity;
		  }

		attr_name = opd1->info.dot.arg2->info.name.original;
		expr->info.expr.arg2 = opd2;
	      }
	    else
	      {
		PT_ERRORf (parser, expr, "Invalid argument in %s()",
			   (op == PT_INCR ? "INCR" : "DECR"));
		return expr;
	      }

	    /* add an argument, id of attribute to do increment */
	    opd3 = parser_new_node (parser, PT_VALUE);
	    if (opd3 == NULL)
	      {
		PT_ERRORc (parser, expr, er_msg ());
		return expr;
	      }

	    if (sm_att_info (entity->info.name.db_object, attr_name,
			     &attrid, &dom, &shared, 0) < 0)
	      {
		PT_ERRORc (parser, expr, er_msg ());
		return expr;
	      }

	    opd3->type_enum = PT_TYPE_INTEGER;
	    opd3->info.value.data_value.i = attrid;
	    expr->info.expr.arg3 = opd3;
	  }

	  /* fall through */
	default:
	  db_make_null (&dummy);
	  arg2 = &dummy;
	  type2 = PT_TYPE_NULL;
	}
    }

  if (prm_get_bool_value (PRM_ID_ORACLE_STYLE_EMPTY_STRING)
      && (op == PT_STRCAT || op == PT_PLUS || op == PT_CONCAT
	  || op == PT_CONCAT_WS))
    {
      TP_DOMAIN *domain;

      /* use the caching variant of this function ! */
      domain = pt_xasl_node_to_domain (parser, expr);

      if (domain && QSTR_IS_ANY_CHAR_OR_BIT (TP_DOMAIN_TYPE (domain)))
	{
	  if (opd1 && opd1->node_type == PT_VALUE
	      && type1 == PT_TYPE_NULL && PT_IS_STRING_TYPE (type2))
	    {
	      /* fold 'null || char_opnd' expr to 'char_opnd' */
	      result = parser_copy_tree (parser, opd2);
	      if (result == NULL)
		{
		  PT_ERRORc (parser, expr, er_msg ());
		  return expr;
		}
	    }
	  else if (opd2 && opd2->node_type == PT_VALUE
		   && type2 == PT_TYPE_NULL && PT_IS_STRING_TYPE (type1))
	    {
	      /* fold 'char_opnd || null' expr to 'char_opnd' */
	      result = parser_copy_tree (parser, opd1);
	      if (result == NULL)
		{
		  PT_ERRORc (parser, expr, er_msg ());
		  return expr;
		}
	    }

	  goto end;		/* finish folding */
	}
    }

  /* special handling for PT_BETWEEN and PT_NOT_BETWEEN */
  opd3 = ((op == PT_BETWEEN || op == PT_NOT_BETWEEN)
	  ? expr->info.expr.arg2->info.expr.arg2 : expr->info.expr.arg3);
  if (opd3 && opd3->node_type == PT_VALUE)
    {
      arg3 = pt_value_to_db (parser, opd3);
      type3 = opd3->type_enum;
    }
  else
    {
      switch (op)
	{
	case PT_REPLACE:
	case PT_TRANSLATE:
	  {
	    INTL_CODESET arg1_cs = DB_IS_NULL (arg1) ? LANG_SYS_CODESET :
	      DB_GET_STRING_CODESET (arg1);
	    int arg1_coll = DB_IS_NULL (arg1) ? LANG_SYS_COLLATION :
	      DB_GET_STRING_COLLATION (arg1);
	    if (PT_IS_NATIONAL_CHAR_STRING_TYPE (type1))
	      {
		db_make_varnchar (&dummy, 1, (char *) "", 0, arg1_cs,
				  arg1_coll);
		type3 = PT_TYPE_VARNCHAR;
	      }
	    else
	      {
		db_make_varchar (&dummy, 1, (char *) "", 0, arg1_cs,
				 arg1_coll);
		type3 = PT_TYPE_VARCHAR;
	      }
	    arg3 = &dummy;
	  }
	  break;

	case PT_LPAD:
	case PT_RPAD:
	  {
	    char pad_str[3] = { '\0' };
	    int pad_size;
	    INTL_CODESET arg1_cs = DB_IS_NULL (arg1) ? LANG_SYS_CODESET :
	      DB_GET_STRING_CODESET (arg1);
	    int arg1_coll = DB_IS_NULL (arg1) ? LANG_SYS_COLLATION :
	      DB_GET_STRING_COLLATION (arg1);

	    intl_pad_char (arg1_cs, (unsigned char *) pad_str, &pad_size);

	    if (PT_IS_NATIONAL_CHAR_STRING_TYPE (type1))
	      {
		db_make_varnchar (&dummy, 1, pad_str, pad_size, arg1_cs,
				  arg1_coll);
		type3 = PT_TYPE_VARNCHAR;
	      }
	    else
	      {
		db_make_varchar (&dummy, 1, pad_str, pad_size, arg1_cs,
				 arg1_coll);
		type3 = PT_TYPE_VARCHAR;
	      }
	    arg3 = &dummy;
	  }
	  break;
	case PT_LIKE:
	  arg3 = NULL;
	  type3 = PT_TYPE_NONE;
	  break;

	default:
	  db_make_null (&dummy);
	  arg3 = &dummy;
	  type3 = PT_TYPE_NULL;
	  break;
	}
    }

  /* If the search condition for the CASE op is already determined,
   * optimize the tree and screen out the arguments for a possible
   * call of pt_evaluate_db_val_expr.
   */
  if ((op == PT_CASE || op == PT_DECODE) && opd3
      && opd3->node_type == PT_VALUE)
    {
      if (arg3 && (DB_VALUE_TYPE (arg3) == DB_TYPE_INTEGER &&
		   DB_GET_INT (arg3)))
	{
	  opd2 = NULL;
	}
      else
	{
	  opd1 = opd2;
	  arg1 = arg2;
	  opd2 = NULL;
	}

      if (opd1 && opd1->node_type != PT_VALUE)
	{
	  if (expr->info.expr.arg2 == opd1
	      && (opd1->info.expr.op == PT_CASE
		  || opd1->info.expr.op == PT_DECODE))
	    {
	      opd1->info.expr.continued_case = 0;
	    }

	  if (pt_check_same_datatype (parser, expr, opd1))
	    {
	      result = parser_copy_tree_list (parser, opd1);
	      if (result == NULL)
		{
		  PT_ERRORc (parser, expr, er_msg ());
		  return expr;
		}
	    }
	  else
	    {
	      PT_NODE *res;

	      res = parser_new_node (parser, PT_EXPR);
	      if (res == NULL)
		{
		  PT_ERRORc (parser, expr, er_msg ());
		  return expr;
		}

	      res->line_number = opd1->line_number;
	      res->column_number = opd1->column_number;
	      res->info.expr.op = PT_CAST;
	      res->info.expr.arg1 = parser_copy_tree_list (parser, opd1);
	      res->type_enum = expr->type_enum;
	      res->info.expr.location = expr->info.expr.location;
	      res->is_hidden_column = is_hidden_column;
	      PT_EXPR_INFO_SET_FLAG (res, PT_EXPR_INFO_CAST_SHOULD_FOLD);

	      if (pt_is_set_type (expr))
		{
		  PT_NODE *sdt;

		  sdt = parser_new_node (parser, PT_DATA_TYPE);
		  if (sdt == NULL)
		    {
		      parser_free_tree (parser, res);
		      PT_ERRORc (parser, expr, er_msg ());
		      return expr;
		    }
		  res->data_type = parser_copy_tree_list (parser,
							  expr->data_type);
		  sdt->type_enum = expr->type_enum;
		  sdt->data_type = parser_copy_tree_list (parser,
							  expr->data_type);
		  res->info.expr.cast_type = sdt;
		}
	      else if (PT_IS_PARAMETERIZED_TYPE (expr->type_enum))
		{
		  res->data_type = parser_copy_tree_list (parser,
							  expr->data_type);
		  res->info.expr.cast_type =
		    parser_copy_tree_list (parser, expr->data_type);
		  res->info.expr.cast_type->type_enum = expr->type_enum;
		}
	      else
		{
		  PT_NODE *dt;

		  dt = parser_new_node (parser, PT_DATA_TYPE);
		  if (dt == NULL)
		    {
		      parser_free_tree (parser, res);
		      PT_ERRORc (parser, expr, er_msg ());
		      return expr;
		    }
		  dt->type_enum = expr->type_enum;
		  res->info.expr.cast_type = dt;
		}

	      result = res;
	    }
	}

      opd3 = NULL;
    }

  /* If the op is AND or OR and one argument is a true/false/NULL
     and the other is a logical expression, optimize the tree so
     that one of the arguments replaces the node.                 */
  if (opd1 && opd2
      && ((opd1->type_enum == PT_TYPE_LOGICAL
	   || opd1->type_enum == PT_TYPE_NULL
	   || opd1->type_enum == PT_TYPE_MAYBE)
	  && (opd2->type_enum == PT_TYPE_LOGICAL
	      || opd2->type_enum == PT_TYPE_NULL
	      || opd2->type_enum == PT_TYPE_MAYBE))
      && ((opd1->node_type == PT_VALUE
	   && opd2->node_type != PT_VALUE)
	  || (opd2->node_type == PT_VALUE
	      && opd1->node_type != PT_VALUE))
      && (op == PT_AND || op == PT_OR))
    {
      PT_NODE *val;
      PT_NODE *other;
      DB_VALUE *db_value;

      if (opd1->node_type == PT_VALUE)
	{
	  val = opd1;
	  other = opd2;
	}
      else
	{
	  val = opd2;
	  other = opd1;
	}

      db_value = pt_value_to_db (parser, val);
      if (op == PT_AND)
	{
	  if (db_value && DB_VALUE_TYPE (db_value) == DB_TYPE_NULL)
	    {
	      result = val;
	    }
	  else if (db_value && DB_GET_INTEGER (db_value) == 1)
	    {
	      result = other;
	    }
	  else
	    {
	      result = val;
	    }
	}
      else
	{			/* op == PT_OR */
	  if (db_value && DB_VALUE_TYPE (db_value) == DB_TYPE_NULL)
	    {
	      result = other;
	    }
	  else if (db_value && DB_GET_INTEGER (db_value) == 1)
	    {
	      result = val;
	    }
	  else
	    {
	      result = other;
	    }
	}

      result = parser_copy_tree_list (parser, result);
      if (result == NULL)
	{
	  PT_ERRORc (parser, expr, er_msg ());
	  return expr;
	}
    }
  else if (opd1
	   && ((opd1->node_type == PT_VALUE
		&& (op != PT_CAST ||
		    PT_EXPR_INFO_IS_FLAGED (expr,
					    PT_EXPR_INFO_CAST_SHOULD_FOLD))
		&& (!opd2 || opd2->node_type == PT_VALUE)
		&& (!opd3 || opd3->node_type == PT_VALUE))
	       || ((opd1->type_enum == PT_TYPE_NA
		    || opd1->type_enum == PT_TYPE_NULL)
		   && op != PT_CASE && op != PT_TO_CHAR
		   && op != PT_TO_NUMBER && op != PT_TO_DATE
		   && op != PT_TO_TIME && op != PT_TO_TIMESTAMP
		   && op != PT_TO_DATETIME
		   && op != PT_NULLIF && op != PT_COALESCE
		   && op != PT_NVL && op != PT_NVL2 && op != PT_DECODE
		   && op != PT_IFNULL)
	       || (opd2 && (opd2->type_enum == PT_TYPE_NA
			    || opd2->type_enum == PT_TYPE_NULL)
		   && op != PT_CASE && op != PT_TO_CHAR
		   && op != PT_TO_NUMBER && op != PT_TO_DATE
		   && op != PT_TO_TIME && op != PT_TO_TIMESTAMP
		   && op != PT_TO_DATETIME && op != PT_BETWEEN
		   && op != PT_NOT_BETWEEN && op != PT_SYS_CONNECT_BY_PATH
		   && op != PT_NULLIF && op != PT_COALESCE
		   && op != PT_NVL && op != PT_NVL2 && op != PT_DECODE
		   && op != PT_IFNULL && op != PT_IF
		   && (op != PT_RANGE || !opd2->or_next))
	       || (opd3 && (opd3->type_enum == PT_TYPE_NA
			    || opd3->type_enum == PT_TYPE_NULL)
		   && op != PT_BETWEEN && op != PT_NOT_BETWEEN
		   && op != PT_NVL2 && op != PT_IF)
	       || (opd2 && opd3 && op == PT_IF
		   && (opd2->type_enum == PT_TYPE_NA
		       || opd2->type_enum == PT_TYPE_NULL)
		   && (opd3->type_enum == PT_TYPE_NA
		       || opd3->type_enum == PT_TYPE_NULL))))
    {
      PT_MISC_TYPE qualifier = (PT_MISC_TYPE) 0;
      TP_DOMAIN *domain;

      if (op == PT_TRIM || op == PT_EXTRACT ||
	  op == PT_SUBSTRING || op == PT_EQ)
	{
	  qualifier = expr->info.expr.qualifier;
	}

      /* use the caching variant of this function ! */
      domain = pt_xasl_node_to_domain (parser, expr);

      /* check to see if we received an error getting the domain */
      if (pt_has_error (parser))
	{
	  if (result)
	    {
	      if (result != expr)
		{
		  parser_free_tree (parser, result);
		}
	    }
	  return expr;
	}

      if (!pt_check_const_fold_op_w_args (op, arg1, arg2, arg3))
	{
	  goto end;
	}

      if (pt_evaluate_db_value_expr (parser, expr, op, arg1, arg2, arg3,
				     &dbval_res, domain, opd1,
				     opd2, opd3, qualifier))
	{
	  result = pt_dbval_to_value (parser, &dbval_res);
	  if (result && expr->or_next && result != expr)
	    {
	      PT_NODE *other;
	      DB_VALUE *db_value;

	      /* i.e., op == PT_OR */
	      db_value = pt_value_to_db (parser, result);	/* opd1 */
	      other = expr->or_next;	/* opd2 */
	      if (db_value && DB_VALUE_TYPE (db_value) == DB_TYPE_NULL)
		{
		  result = other;
		}
	      else
		if (db_value
		    && DB_VALUE_TYPE (db_value) == DB_TYPE_INTEGER
		    && DB_GET_INTEGER (db_value) == 1)
		{
		  parser_free_tree (parser, result->or_next);
		  result->or_next = NULL;
		}
	      else
		{
		  result = other;
		}

	      result = parser_copy_tree_list (parser, result);
	      if (result == NULL)
		{
		  pr_clear_value (&dbval_res);
		  PT_ERRORc (parser, expr, er_msg ());
		  return expr;
		}
	    }
	}
    }
  else if (result_type == PT_TYPE_LOGICAL)
    {
      /* We'll check to see if the expression is always true or false
       * due to type boundary overflows */
      if (opd1 && opd1->node_type == PT_VALUE
	  && opd2 && opd2->node_type == PT_VALUE)
	{
	  result = pt_compare_bounds_to_value (parser, expr, op,
					       opd1->type_enum, arg2, type2);
	}

      if (result && expr->or_next && result != expr)
	{
	  PT_NODE *other;
	  DB_VALUE *db_value;

	  /* i.e., op == PT_OR */
	  db_value = pt_value_to_db (parser, result);	/* opd1 */
	  other = result->or_next;	/* opd2 */
	  if (db_value && DB_VALUE_TYPE (db_value) == DB_TYPE_NULL)
	    {
	      result = other;
	    }
	  else if (db_value && DB_VALUE_TYPE (db_value) == DB_TYPE_INTEGER
		   && DB_GET_INTEGER (db_value) == 1)
	    {
	      parser_free_tree (parser, result->or_next);
	      result->or_next = NULL;
	    }
	  else
	    {
	      result = other;
	    }

	  result = parser_copy_tree_list (parser, result);
	}
      if (result == NULL)
	{
	  PT_ERRORc (parser, expr, er_msg ());
	  return expr;
	}
    }

end:
  pr_clear_value (&dbval_res);

  if (result)
    {
      result->line_number = line;
      result->column_number = column;
      result->alias_print = alias_print;
      result->is_hidden_column = is_hidden_column;
      result->is_value_query = expr->is_value_query;

      if (result != expr)
	{
	  if (alias_print == NULL
	      || (PT_IS_VALUE_NODE (result) && !result->info.value.text))
	    {
	      /* print expr to alias_print */
	      expr->alias_print = NULL;
	      PT_NODE_PRINT_TO_ALIAS (parser, expr, PT_CONVERT_RANGE);
	    }
	  if (alias_print == NULL)
	    {
	      alias_print = expr->alias_print;
	    }
	  if (result->alias_print == NULL)
	    {
	      result->alias_print =
		pt_append_string (parser, NULL, alias_print);
	    }
	  if (PT_IS_VALUE_NODE (result) && !result->info.value.text)
	    {
	      result->info.value.text =
		pt_append_string (parser, NULL, expr->alias_print);
	    }
	  parser_free_tree (parser, expr);
	}

      result->next = expr_next;

      if (result->type_enum != PT_TYPE_NA
	  && result->type_enum != PT_TYPE_NULL)
	{
	  result->type_enum = result_type;
	}

      if (result->node_type == PT_EXPR)
	{
	  result->info.expr.location = location;
	}
      else if (result->node_type == PT_VALUE)
	{
	  result->info.value.location = location;
	}
    }

  return result;
}

/*
 * pt_evaluate_function_w_args () - evaluate the function to a DB_VALUE
 *   return: 1, if successful,
 *           0, if not successful.
 *   parser(in): parser global context info for reentrancy
 *   fcode(in): function code
 *   args(in): array of arguments' values
 *   num_args(in): number of arguments
 *   result(out): result value of function (if evaluated)
 */
int
pt_evaluate_function_w_args (PARSER_CONTEXT * parser, FUNC_TYPE fcode,
			     DB_VALUE * args[], const int num_args,
			     DB_VALUE * result)
{
  int error = NO_ERROR, i;

  assert (parser != NULL);
  assert (args != NULL);
  assert (result != NULL);

  if (!args || !result)
    {
      return 0;
    }

  /* init array vars */
  for (i = 0; i < num_args; i++)
    {
      assert (args[i] != NULL);
    }

  switch (fcode)
    {
    case F_INSERT_SUBSTRING:
      {
	error = db_string_insert_substring (args[0], args[1], args[2],
					    args[3], result);
	if (error != NO_ERROR)
	  {
	    return 0;
	  }
      }
      break;
    case F_ELT:
      error = db_string_elt (result, args, num_args);
      if (error != NO_ERROR)
	{
	  return 0;
	}
      break;
    default:
      /* a supported function doesn't have const folding code */
      assert (false);
      break;
    }
  return 1;
}

/*
 * pt_fold_const_function () - evaluate constant function
 *   return: the evaluated expression, if successful,
 *           unchanged function, if not successful.
 *   parser(in): parser global context info for reentrancy
 *   func(in): a parse tree representation of a possibly constant function
 */
static PT_NODE *
pt_fold_const_function (PARSER_CONTEXT * parser, PT_NODE * func)
{
  PT_TYPE_ENUM result_type = PT_TYPE_NONE;
  PT_NODE *result = NULL;
  DB_VALUE dbval_res;
  PT_NODE *func_next;
  int line = 0, column = 0, num_args = 0;
  short location;
  const char *alias_print = NULL;
  int error = NO_ERROR;

  if (func == NULL)
    {
      return func;
    }

  if (func->node_type != PT_FUNCTION)
    {
      return func;
    }

  if (func->do_not_fold)
    {
      return func;
    }

  /* only functions wrapped with expressions are supported */
  if (!pt_is_expr_wrapped_function (parser, func))
    {
      return func;
    }


  /* PT_FUNCTION doesn't have location attribute as PT_EXPR does
   * temporary set location to 0 ( WHERE clause) */
  location = 0;

  db_make_null (&dbval_res);

  line = func->line_number;
  column = func->column_number;
  alias_print = func->alias_print;
  func_next = func->next;
  func->next = NULL;
  result_type = func->type_enum;
  result = func;

  if (pt_evaluate_function (parser, func, &dbval_res) == NO_ERROR)
    {
      result = pt_dbval_to_value (parser, &dbval_res);
    }

  pr_clear_value (&dbval_res);

  if (result)
    {
      if (result != func)
	{
	  if (alias_print == NULL
	      || (PT_IS_VALUE_NODE (result) && !result->info.value.text))
	    {
	      func->alias_print = NULL;
	      PT_NODE_PRINT_TO_ALIAS (parser, func, PT_CONVERT_RANGE);
	    }
	  if (PT_IS_VALUE_NODE (result) && !result->info.value.text)
	    {
	      result->info.value.text = func->alias_print;
	    }
	  if (alias_print == NULL)
	    {
	      alias_print = func->alias_print;
	    }
	  parser_free_tree (parser, func);
	}

      /* restore saved func attributes */
      result->next = func_next;

      if (result->type_enum != PT_TYPE_NA
	  && result->type_enum != PT_TYPE_NULL)
	{
	  result->type_enum = result_type;
	}

      result->line_number = line;
      result->column_number = column;
      result->alias_print = alias_print;
      if (result->node_type == PT_FUNCTION)
	{
	  assert (result->info.function.arg_list != NULL);
	}
      else if (result->node_type == PT_VALUE)
	{
	  /* temporary set location to a 0
	   * the location will be updated after const folding at the upper
	   * level : the parent node is a PT_EXPR node with a
	   * PT_FUNCTION_HOLDER operator type */
	  result->info.value.location = location;
	}
    }

  return result;
}				/* pt_fold_const_function() */

/*
 * pt_evaluate_function () - evaluate constant function
 *   return: NO_ERROR, if evaluation successfull,
 *	     an error code, if unsuccessful
 *   parser(in): parser global context info for reentrancy
 *   func(in): a parse tree representation of a possibly constant function
 *   dbval_res(in/out): the result DB_VALUE of evaluation
 */
int
pt_evaluate_function (PARSER_CONTEXT * parser, PT_NODE * func,
		      DB_VALUE * dbval_res)
{
  PT_NODE *operand;
  DB_VALUE dummy, **arg_array;
  FUNC_TYPE fcode;
  int error = NO_ERROR, i;
  int num_args = 0;
  bool all_args_const = false;

  /* init array variables */
  arg_array = NULL;

  if (func == NULL)
    {
      return ER_FAILED;
    }

  if (func->node_type != PT_FUNCTION)
    {
      return ER_FAILED;
    }

  fcode = func->info.function.function_type;
  /* only functions wrapped with expressions are supported */
  if (!pt_is_expr_wrapped_function (parser, func))
    {
      return ER_FAILED;
    }

  db_make_null (dbval_res);

  /* count function's arguments */
  operand = func->info.function.arg_list;
  num_args = 0;
  while (operand)
    {
      ++num_args;
      operand = operand->next;
    }
  assert (num_args > 0);

  arg_array = (DB_VALUE **) calloc (num_args, sizeof (DB_VALUE *));
  if (arg_array == NULL)
    {
      goto end;
    }

  /* convert all operands to DB_VALUE arguments */
  /* for some functions this may not be necessary :
   * you need to break from this loop and solve them at next steps */
  all_args_const = true;
  operand = func->info.function.arg_list;
  for (i = 0; i < num_args; i++)
    {
      if (operand != NULL && operand->node_type == PT_VALUE)
	{
	  DB_VALUE *arg = NULL;

	  arg = pt_value_to_db (parser, operand);
	  if (arg == NULL)
	    {
	      all_args_const = false;
	      break;
	    }
	  else
	    {
	      arg_array[i] = arg;
	    }
	}
      else
	{
	  db_make_null (&dummy);
	  arg_array[i] = &dummy;
	  all_args_const = false;
	  break;
	}
      operand = operand->next;
    }

  if (all_args_const && i == num_args)
    {
      TP_DOMAIN *domain;

      /* use the caching variant of this function ! */
      domain = pt_xasl_node_to_domain (parser, func);

      /* check if we received an error getting the domain */
      if (pt_has_error (parser))
	{
	  pr_clear_value (dbval_res);
	  error = ER_FAILED;
	  goto end;
	}

      if (pt_evaluate_function_w_args (parser, fcode, arg_array, num_args,
				       dbval_res) != 1)
	{
	  error = ER_FAILED;
	  goto end;
	}
    }
  else
    {
      error = ER_FAILED;
    }
end:
  if (arg_array != NULL)
    {
      free_and_init (arg_array);
    }

  return error;
}

/*
 * pt_semantic_type () - sets data types for all expressions in a parse tree
 * 			 and evaluates constant sub expressions
 *   return:
 *   parser(in):
 *   tree(in/out):
 *   sc_info_ptr(in):
 */

PT_NODE *
pt_semantic_type (PARSER_CONTEXT * parser,
		  PT_NODE * tree, SEMANTIC_CHK_INFO * sc_info_ptr)
{
  SEMANTIC_CHK_INFO sc_info = { NULL, NULL, 0, 0, 0, false, false };

  if (pt_has_error (parser))
    {
      return NULL;
    }

  if (sc_info_ptr)
    {
      /* do type checking */
      tree =
	parser_walk_tree (parser, tree, pt_eval_type_pre,
			  sc_info_ptr, pt_eval_type, sc_info_ptr);

      /* do constant folding */
      tree =
	parser_walk_tree (parser, tree, NULL, NULL,
			  pt_fold_constants, sc_info_ptr);
    }
  else
    {
      sc_info.top_node = tree;
      sc_info.donot_fold = false;
      /* do type checking */
      tree =
	parser_walk_tree (parser, tree, pt_eval_type_pre,
			  &sc_info, pt_eval_type, &sc_info);

      /* do constant folding */
      tree =
	parser_walk_tree (parser, tree, NULL, NULL,
			  pt_fold_constants, &sc_info);
    }

  if (pt_has_error (parser))
    {
      tree = NULL;
    }

  return tree;
}


/*
 * pt_class_name () - return the class name of a data_type node
 *   return:
 *   type(in): a data_type node
 */
static const char *
pt_class_name (const PT_NODE * type)
{
  if (!type
      || type->node_type != PT_DATA_TYPE
      || !type->info.data_type.entity
      || type->info.data_type.entity->node_type != PT_NAME)
    {
      return NULL;
    }
  else
    {
      return type->info.data_type.entity->info.name.original;
    }
}

/*
 * pt_set_default_data_type () -
 *   return:
 *   parser(in):
 *   type(in):
 *   dtp(in):
 */
static int
pt_set_default_data_type (PARSER_CONTEXT * parser,
			  PT_TYPE_ENUM type, PT_NODE ** dtp)
{
  PT_NODE *dt;
  int error = NO_ERROR;

  dt = parser_new_node (parser, PT_DATA_TYPE);
  if (dt == NULL)
    {
      return ER_GENERIC_ERROR;
    }

  dt->type_enum = type;
  switch (type)
    {
    case PT_TYPE_CHAR:
    case PT_TYPE_VARCHAR:
      dt->info.data_type.precision = TP_FLOATING_PRECISION_VALUE;
      dt->info.data_type.units = (int) LANG_SYS_CODESET;
      dt->info.data_type.collation_id = LANG_SYS_COLLATION;
      break;

    case PT_TYPE_NCHAR:
    case PT_TYPE_VARNCHAR:
      dt->info.data_type.precision = TP_FLOATING_PRECISION_VALUE;
      dt->info.data_type.units = (int) LANG_SYS_CODESET;
      dt->info.data_type.collation_id = LANG_SYS_COLLATION;
      break;

    case PT_TYPE_BIT:
    case PT_TYPE_VARBIT:
      dt->info.data_type.precision = TP_FLOATING_PRECISION_VALUE;
      dt->info.data_type.units = INTL_CODESET_RAW_BITS;
      break;

    case PT_TYPE_NUMERIC:
      dt->info.data_type.precision = TP_FLOATING_PRECISION_VALUE;
      /*
       * FIX ME!! Is it the case that this will always happen in
       * zero-scale context?  That's certainly the case when we're
       * coercing from integers, but what about floats and doubles?
       */
      dt->info.data_type.dec_precision = 0;
      break;

    default:
      PT_INTERNAL_ERROR (parser, "type check");
      error = ER_GENERIC_ERROR;
      break;
    }

  *dtp = dt;
  return error;
}


/*
 * pt_coerce_value () - coerce a PT_VALUE into another PT_VALUE of
 * 			compatible type
 *   return: NO_ERROR on success, non-zero for ERROR
 *   parser(in):
 *   src(in): a pointer to the original PT_VALUE
 *   dest(out): a pointer to the coerced PT_VALUE
 *   desired_type(in): the desired type of the coerced result
 *   data_type(in): the data type list of a (desired) set type or
 *                  the data type of an object or NULL
 */
int
pt_coerce_value (PARSER_CONTEXT * parser, PT_NODE * src, PT_NODE * dest,
		 PT_TYPE_ENUM desired_type, PT_NODE * data_type)
{
  PT_TYPE_ENUM original_type;
  PT_NODE *dest_next;
  int err = NO_ERROR;
  PT_NODE *temp = NULL;
  bool is_collation_change = false;

  assert (src != NULL && dest != NULL);

  dest_next = dest->next;

  original_type = src->type_enum;

  if (PT_HAS_COLLATION (original_type) && PT_HAS_COLLATION (desired_type)
      && src->data_type != NULL && data_type != NULL
      && src->data_type->info.data_type.collation_id
      != data_type->info.data_type.collation_id)
    {
      is_collation_change = true;
    }

  if ((original_type == (PT_TYPE_ENUM) desired_type
       && original_type != PT_TYPE_NUMERIC
       && desired_type != PT_TYPE_OBJECT
       && !is_collation_change)
      || original_type == PT_TYPE_NA || original_type == PT_TYPE_NULL)
    {
      if (src != dest)
	{
	  *dest = *src;
	  dest->next = dest_next;
	}
      return NO_ERROR;
    }

  if (data_type == NULL
      && PT_IS_PARAMETERIZED_TYPE (desired_type)
      && (err = pt_set_default_data_type (parser,
					  (PT_TYPE_ENUM) desired_type,
					  &data_type) < 0))
    {
      return err;
    }

  if (original_type == PT_TYPE_NUMERIC
      && desired_type == PT_TYPE_NUMERIC
      && src->data_type != NULL
      && (src->data_type->info.data_type.precision ==
	  data_type->info.data_type.precision)
      && (src->data_type->info.data_type.dec_precision ==
	  data_type->info.data_type.dec_precision) && !is_collation_change)
    {				/* exact match */

      if (src != dest)
	{
	  *dest = *src;
	  dest->next = dest_next;
	}
      return NO_ERROR;
    }

  if (original_type == PT_TYPE_NONE && src->node_type != PT_HOST_VAR)
    {
      if (src != dest)
	{
	  *dest = *src;
	  dest->next = dest_next;
	}
      dest->type_enum = (PT_TYPE_ENUM) desired_type;
      dest->data_type = parser_copy_tree_list (parser, data_type);
      /* don't return, in case further coercion is needed
         set original type to match desired type to avoid confusing
         type check below */
    }

  switch (src->node_type)
    {
    case PT_HOST_VAR:
      /* binding of host variables may be delayed in the case of an esql
       * PREPARE statement until an OPEN cursor or an EXECUTE statement.
       * in this case we seem to have no choice but to assume each host
       * variable is typeless and can be coerced into any desired type.
       */
      if (parser->set_host_var == 0)
	{
	  dest->type_enum = (PT_TYPE_ENUM) desired_type;
	  dest->data_type = parser_copy_tree_list (parser, data_type);
	  return NO_ERROR;
	}

      /* otherwise fall through to the PT_VALUE case */

    case PT_VALUE:
      {
	DB_VALUE *db_src = NULL;
	DB_VALUE db_dest;
	TP_DOMAIN *desired_domain;

	db_src = pt_value_to_db (parser, src);
	if (!db_src)
	  {
	    err = ER_GENERIC_ERROR;
	    break;
	  }

	db_make_null (&db_dest);

	/* be sure to use the domain caching versions */
	if (data_type)
	  {
	    desired_domain = pt_node_data_type_to_db_domain
	      (parser, (PT_NODE *) data_type, (PT_TYPE_ENUM) desired_type);
	    /* need a caching version of this function ? */
	    if (desired_domain != NULL)
	      {
		desired_domain = tp_domain_cache (desired_domain);
	      }
	  }
	else
	  {
	    desired_domain =
	      pt_xasl_type_enum_to_domain ((PT_TYPE_ENUM) desired_type);
	  }

	err = tp_value_coerce (db_src, &db_dest, desired_domain);

	switch (err)
	  {
	  case DOMAIN_INCOMPATIBLE:
	    err = ER_IT_INCOMPATIBLE_DATATYPE;
	    break;
	  case DOMAIN_OVERFLOW:
	    err = ER_IT_DATA_OVERFLOW;
	    break;
	  case DOMAIN_ERROR:
	    err = er_errid ();
	    break;
	  default:
	    break;
	  }

	if (err == DOMAIN_COMPATIBLE
	    && src->node_type == PT_HOST_VAR
	    && prm_get_bool_value (PRM_ID_HOSTVAR_LATE_BINDING) == false)
	  {
	    /* when the type of the host variable is compatible to coerce,
	     * it is enough. NEVER change the node type to PT_VALUE. */
	    db_value_clear (&db_dest);
	    return NO_ERROR;
	  }

	if (src->info.value.db_value_is_in_workspace)
	  {
	    (void) db_value_clear (db_src);
	  }

	if (err >= 0)
	  {
	    temp = pt_dbval_to_value (parser, &db_dest);
	    (void) db_value_clear (&db_dest);
	    if (!temp)
	      {
		err = ER_GENERIC_ERROR;
	      }
	    else
	      {
		temp->line_number = dest->line_number;
		temp->column_number = dest->column_number;
		temp->alias_print = dest->alias_print;
		temp->info.value.print_charset =
		  dest->info.value.print_charset;
		temp->info.value.print_collation =
		  dest->info.value.print_collation;
		*dest = *temp;
		dest->next = dest_next;
		temp->info.value.db_value_is_in_workspace = 0;
		parser_free_node (parser, temp);
	      }
	  }
      }
      break;

    case PT_FUNCTION:
      if (src == dest)
	{
	  switch (src->info.function.function_type)
	    {
	    case F_MULTISET:
	    case F_SEQUENCE:
	      switch (desired_type)
		{
		case PT_TYPE_SET:
		  dest->info.function.function_type = F_SET;
		  dest->type_enum = PT_TYPE_SET;
		  break;
		case PT_TYPE_SEQUENCE:
		  dest->info.function.function_type = F_SEQUENCE;
		  dest->type_enum = PT_TYPE_SEQUENCE;
		  break;
		case PT_TYPE_MULTISET:
		  dest->info.function.function_type = F_MULTISET;
		  dest->type_enum = PT_TYPE_MULTISET;
		  break;
		default:
		  break;
		}
	      break;

	    case F_TABLE_MULTISET:
	    case F_TABLE_SEQUENCE:
	      switch (desired_type)
		{
		case PT_TYPE_SET:
		  dest->info.function.function_type = F_TABLE_SET;
		  dest->type_enum = PT_TYPE_SET;
		  break;
		case PT_TYPE_SEQUENCE:
		  dest->info.function.function_type = F_TABLE_SEQUENCE;
		  dest->type_enum = PT_TYPE_SEQUENCE;
		  break;
		case PT_TYPE_MULTISET:
		  dest->info.function.function_type = F_TABLE_MULTISET;
		  dest->type_enum = PT_TYPE_MULTISET;
		  break;
		default:
		  break;
		}
	      break;

	    default:
	      break;
	    }
	}
      break;

    default:
      err = ((pt_common_type ((PT_TYPE_ENUM) desired_type,
			      src->type_enum) == PT_TYPE_NONE)
	     ? ER_IT_INCOMPATIBLE_DATATYPE : NO_ERROR);
      break;
    }

  if (err == ER_IT_DATA_OVERFLOW)
    {
      PT_ERRORmf2 (parser, src, MSGCAT_SET_PARSER_SEMANTIC,
		   MSGCAT_SEMANTIC_OVERFLOW_COERCING_TO,
		   pt_short_print (parser, src),
		   pt_show_type_enum ((PT_TYPE_ENUM) desired_type));
    }
  else if (err < 0)
    {
      PT_ERRORmf2 (parser, src, MSGCAT_SET_PARSER_SEMANTIC,
		   MSGCAT_SEMANTIC_CANT_COERCE_TO,
		   pt_short_print (parser, src),
		   (desired_type == PT_TYPE_OBJECT
		    ? pt_class_name (data_type)
		    : pt_show_type_enum ((PT_TYPE_ENUM) desired_type)));
    }

  return err;
}

#if defined(ENABLE_UNUSED_FUNCTION)
/*
 * generic_func_casecmp () -
 *   return:
 *   a(in):
 *   b(in):
 */
static int
generic_func_casecmp (const void *a, const void *b)
{
  return
    intl_identifier_casecmp (((const GENERIC_FUNCTION_RECORD *) a)->
			     function_name,
			     ((const GENERIC_FUNCTION_RECORD *) b)->
			     function_name);
}

/*
 * init_generic_funcs () -
 *   return:
 *   void(in):
 */
static void
init_generic_funcs (void)
{
  qsort (pt_Generic_functions,
	 (sizeof (pt_Generic_functions) / sizeof (GENERIC_FUNCTION_RECORD)),
	 sizeof (GENERIC_FUNCTION_RECORD), &generic_func_casecmp);
  pt_Generic_functions_sorted = 1;
}
#endif /* ENABLE_UNUSED_FUNCTION */

/*
 * pt_type_generic_func () - Searches the generic_funcs table to find
 * 			     the given generic function
 *   return: 1 if the generic function is defined, 0 otherwise
 *   parser(in):
 *   node(in):
 */

int
pt_type_generic_func (PARSER_CONTEXT * parser, PT_NODE * node)
{
#if !defined(ENABLE_UNUSED_FUNCTION)
  /* If you want to use generic function, remove this block. */
  return 0;
#else /* !ENABLE_UNUSED_FUNCTION */
  GENERIC_FUNCTION_RECORD *record_p, key;
  PT_NODE *offset;

  if (!pt_Generic_functions_sorted)
    {
      init_generic_funcs ();
    }

  if (node->node_type != PT_FUNCTION
      || node->info.function.function_type != PT_GENERIC
      || !node->info.function.generic_name)
    {
      return 0;			/* this is not a generic function */
    }

  /* Check first to see if the function exists in our table. */
  key.function_name = node->info.function.generic_name;
  record_p =
    (GENERIC_FUNCTION_RECORD *) bsearch (&key, pt_Generic_functions,
					 (sizeof (pt_Generic_functions) /
					  sizeof (GENERIC_FUNCTION_RECORD)),
					 sizeof (GENERIC_FUNCTION_RECORD),
					 &generic_func_casecmp);
  if (!record_p)
    {
      return 0;			/* we can't find it */
    }

  if ((offset = parser_new_node (parser, PT_VALUE)) == NULL)
    {
      return 0;
    }

  offset->type_enum = PT_TYPE_INTEGER;
  offset->info.value.data_value.i = record_p->func_ptr_offset;
  node->info.function.arg_list =
    parser_append_node (node->info.function.arg_list, offset);

  /* type the node */
  pt_string_to_data_type (parser, record_p->return_type, node);

  return 1;
#endif /* ENABLE_UNUSED_FUNCTION */
}


/*
 * pt_compare_bounds_to_value () - compare constant value to base type
 * 	boundaries.  If value is out of bounds, we already know the
 * 	result of a logical comparison (<, >, <=, >=, ==)
 *   return: null or logical value node set to true or false.
 *   parser(in): parse tree
 *   expr(in): logical expression to be examined
 *   op(in): expression operator
 *   lhs_type(in): type of left hand operand
 *   rhs_val(in): value of right hand operand
 *   rhs_type(in): type of right hand operand
 *
 * Note :
 *    This function coerces a PT_VALUE to another PT_VALUE of compatible type.
 */

static PT_NODE *
pt_compare_bounds_to_value (PARSER_CONTEXT * parser,
			    PT_NODE * expr,
			    PT_OP_TYPE op,
			    PT_TYPE_ENUM lhs_type,
			    DB_VALUE * rhs_val, PT_TYPE_ENUM rhs_type)
{
  bool lhs_less = false;
  bool lhs_greater = false;
  bool always_false = false;
  bool always_false_due_to_null = false;
  bool always_true = false;
  PT_NODE *result = expr;
  double dtmp;

  /* we can't determine anything if the types are the same */
  if (lhs_type == rhs_type)
    {
      return result;
    }

  /* check if op is always false due to null */
  if (op != PT_IS_NULL && op != PT_IS_NOT_NULL)
    {
      if (DB_IS_NULL (rhs_val)
	  && rhs_type != PT_TYPE_SET
	  && rhs_type != PT_TYPE_SEQUENCE && rhs_type != PT_TYPE_MULTISET)
	{
	  always_false_due_to_null = true;
	  goto end;
	}
    }

  /* we only allow PT_EQ, PT_GT, PT_GE, PT_LT, PT_LE. */
  if (op != PT_EQ && op != PT_GT && op != PT_GE && op != PT_LT && op != PT_LE)
    {
      return result;
    }

  /* we need to extend the following to compare dates and times, but
   * probably not until we make the ranges of PT_* and DB_* the same */
  switch (lhs_type)
    {
    case PT_TYPE_SMALLINT:
      switch (rhs_type)
	{
	case PT_TYPE_INTEGER:
	  if (DB_GET_INTEGER (rhs_val) > DB_INT16_MAX)
	    lhs_less = true;
	  else if (DB_GET_INTEGER (rhs_val) < DB_INT16_MIN)
	    lhs_greater = true;
	  break;
	case PT_TYPE_BIGINT:
	  if (DB_GET_BIGINT (rhs_val) > DB_INT16_MAX)
	    lhs_less = true;
	  else if (DB_GET_BIGINT (rhs_val) < DB_INT16_MIN)
	    lhs_greater = true;
	  break;
	case PT_TYPE_FLOAT:
	  if (DB_GET_FLOAT (rhs_val) > DB_INT16_MAX)
	    lhs_less = true;
	  else if (DB_GET_FLOAT (rhs_val) < DB_INT16_MIN)
	    lhs_greater = true;
	  break;

	case PT_TYPE_DOUBLE:
	  if (DB_GET_DOUBLE (rhs_val) > DB_INT16_MAX)
	    lhs_less = true;
	  else if (DB_GET_DOUBLE (rhs_val) < DB_INT16_MIN)
	    lhs_greater = true;
	  break;

	case PT_TYPE_NUMERIC:
	  numeric_coerce_num_to_double (db_locate_numeric (rhs_val),
					DB_VALUE_SCALE (rhs_val), &dtmp);
	  if (dtmp > DB_INT16_MAX)
	    lhs_less = true;
	  else if (dtmp < DB_INT16_MIN)
	    lhs_greater = true;
	  break;

	case PT_TYPE_MONETARY:
	  dtmp = (DB_GET_MONETARY (rhs_val))->amount;
	  if (dtmp > DB_INT16_MAX)
	    lhs_less = true;
	  else if (dtmp < DB_INT16_MIN)
	    lhs_greater = true;
	  break;

	default:
	  break;
	}
      break;

    case PT_TYPE_INTEGER:
      switch (rhs_type)
	{
	case PT_TYPE_BIGINT:
	  if (DB_GET_BIGINT (rhs_val) > DB_INT32_MAX)
	    lhs_less = true;
	  else if (DB_GET_BIGINT (rhs_val) < DB_INT32_MIN)
	    lhs_greater = true;
	  break;
	case PT_TYPE_FLOAT:
	  if (DB_GET_FLOAT (rhs_val) > DB_INT32_MAX)
	    lhs_less = true;
	  else if (DB_GET_FLOAT (rhs_val) < DB_INT32_MIN)
	    lhs_greater = true;
	  break;

	case PT_TYPE_DOUBLE:
	  if (DB_GET_DOUBLE (rhs_val) > DB_INT32_MAX)
	    lhs_less = true;
	  else if (DB_GET_DOUBLE (rhs_val) < DB_INT32_MIN)
	    lhs_greater = true;
	  break;

	case PT_TYPE_NUMERIC:
	  numeric_coerce_num_to_double (db_locate_numeric
					(rhs_val),
					DB_VALUE_SCALE (rhs_val), &dtmp);
	  if (dtmp > DB_INT32_MAX)
	    lhs_less = true;
	  else if (dtmp < DB_INT32_MIN)
	    lhs_greater = true;
	  break;

	case PT_TYPE_MONETARY:
	  dtmp = (DB_GET_MONETARY (rhs_val))->amount;
	  if (dtmp > DB_INT32_MAX)
	    lhs_less = true;
	  else if (dtmp < DB_INT32_MIN)
	    lhs_greater = true;
	  break;
	default:
	  break;
	}
      break;

    case PT_TYPE_BIGINT:
      switch (rhs_type)
	{
	case PT_TYPE_FLOAT:
	  if (DB_GET_FLOAT (rhs_val) > DB_BIGINT_MAX)
	    lhs_less = true;
	  else if (DB_GET_FLOAT (rhs_val) < DB_BIGINT_MIN)
	    lhs_greater = true;
	  break;
	case PT_TYPE_DOUBLE:
	  if (DB_GET_DOUBLE (rhs_val) > DB_BIGINT_MAX)
	    lhs_less = true;
	  else if (DB_GET_DOUBLE (rhs_val) < DB_BIGINT_MIN)
	    lhs_greater = true;
	  break;
	case PT_TYPE_NUMERIC:
	  numeric_coerce_num_to_double (db_locate_numeric
					(rhs_val),
					DB_VALUE_SCALE (rhs_val), &dtmp);
	  if (dtmp > DB_BIGINT_MAX)
	    lhs_less = true;
	  else if (dtmp < DB_BIGINT_MIN)
	    lhs_greater = true;
	  break;
	case PT_TYPE_MONETARY:
	  dtmp = (DB_GET_MONETARY (rhs_val))->amount;
	  if (dtmp > DB_BIGINT_MAX)
	    lhs_less = true;
	  else if (dtmp < DB_BIGINT_MIN)
	    lhs_greater = true;
	  break;
	default:
	  break;
	}
      break;

    case PT_TYPE_FLOAT:
      switch (rhs_type)
	{
	case PT_TYPE_DOUBLE:
	  if (DB_GET_DOUBLE (rhs_val) > FLT_MAX)
	    lhs_less = true;
	  else if (DB_GET_DOUBLE (rhs_val) < -(FLT_MAX))
	    lhs_greater = true;
	  break;

	case PT_TYPE_NUMERIC:
	  numeric_coerce_num_to_double (db_locate_numeric
					(rhs_val),
					DB_VALUE_SCALE (rhs_val), &dtmp);
	  if (dtmp > FLT_MAX)
	    lhs_less = true;
	  else if (dtmp < -(FLT_MAX))
	    lhs_greater = true;
	  break;

	case PT_TYPE_MONETARY:
	  dtmp = (DB_GET_MONETARY (rhs_val))->amount;
	  if (dtmp > FLT_MAX)
	    lhs_less = true;
	  else if (dtmp < -(FLT_MAX))
	    lhs_greater = true;
	  break;

	default:
	  break;
	}
      break;

    default:
      break;
    }

  if (lhs_less)
    {
      if (op == PT_EQ || op == PT_GT || op == PT_GE)
	{
	  always_false = true;
	}
      else if (op == PT_LT || op == PT_LE)
	{
	  always_true = true;
	}
    }
  else if (lhs_greater)
    {
      if (op == PT_EQ || op == PT_LT || op == PT_LE)
	{
	  always_false = true;
	}
      else if (op == PT_GT || op == PT_GE)
	{
	  always_true = true;
	}
    }

end:
  if (always_false)
    {
      result = parser_new_node (parser, PT_VALUE);
      if (result == NULL)
	{
	  PT_INTERNAL_ERROR (parser, "allocate new node");
	  return NULL;
	}

      result->type_enum = PT_TYPE_LOGICAL;
      result->info.value.data_value.i = false;
      result->info.value.location = expr->info.expr.location;
      (void) pt_value_to_db (parser, result);
    }
  else if (always_false_due_to_null)
    {
      result = parser_new_node (parser, PT_VALUE);
      if (result == NULL)
	{
	  PT_INTERNAL_ERROR (parser, "allocate new node");
	  return NULL;
	}

      result->type_enum = PT_TYPE_NULL;
      result->info.value.location = expr->info.expr.location;
      (void) pt_value_to_db (parser, result);
    }
  else if (always_true)
    {
      result = parser_new_node (parser, PT_VALUE);
      if (result == NULL)
	{
	  PT_INTERNAL_ERROR (parser, "allocate new node");
	  return NULL;
	}

      result->type_enum = PT_TYPE_LOGICAL;
      result->info.value.data_value.i = true;
      result->info.value.location = expr->info.expr.location;
      (void) pt_value_to_db (parser, result);
    }

  return result;
}


/*
 * pt_converse_op () - Figure out the converse of a relational operator,
 * 	so that we can flip a relational expression into a canonical form
 *   return:
 *   op(in):
 */

PT_OP_TYPE
pt_converse_op (PT_OP_TYPE op)
{
  switch (op)
    {
    case PT_EQ:
      return PT_EQ;
    case PT_LT:
      return PT_GT;
    case PT_LE:
      return PT_GE;
    case PT_GT:
      return PT_LT;
    case PT_GE:
      return PT_LE;
    case PT_NE:
      return PT_NE;
    default:
      return (PT_OP_TYPE) 0;
    }
}


/*
 * pt_is_between_range_op () -
 *   return:
 *   op(in):
 */
int
pt_is_between_range_op (PT_OP_TYPE op)
{
  switch (op)
    {
    case PT_BETWEEN_AND:
    case PT_BETWEEN_GE_LE:
    case PT_BETWEEN_GE_LT:
    case PT_BETWEEN_GT_LE:
    case PT_BETWEEN_GT_LT:
    case PT_BETWEEN_EQ_NA:
    case PT_BETWEEN_INF_LE:
    case PT_BETWEEN_INF_LT:
    case PT_BETWEEN_GE_INF:
    case PT_BETWEEN_GT_INF:
      return 1;
    default:
      return 0;
    }
}


/*
 * pt_is_comp_op () -
 *   return:
 *   op(in):
 */
int
pt_is_comp_op (PT_OP_TYPE op)
{
  switch (op)
    {
    case PT_LIKE:
    case PT_NOT_LIKE:
    case PT_IS_IN:
    case PT_IS_NOT_IN:
    case PT_IS_NULL:
    case PT_IS_NOT_NULL:
    case PT_IS:
    case PT_IS_NOT:
    case PT_EQ_SOME:
    case PT_NE_SOME:
    case PT_GE_SOME:
    case PT_GT_SOME:
    case PT_LT_SOME:
    case PT_LE_SOME:
    case PT_EQ_ALL:
    case PT_NE_ALL:
    case PT_GE_ALL:
    case PT_GT_ALL:
    case PT_LT_ALL:
    case PT_LE_ALL:
    case PT_EQ:
    case PT_NE:
    case PT_GE:
    case PT_GT:
    case PT_LT:
    case PT_LE:
    case PT_NULLSAFE_EQ:
    case PT_GT_INF:
    case PT_LT_INF:
    case PT_BETWEEN:
    case PT_BETWEEN_AND:
    case PT_BETWEEN_GE_LE:
    case PT_BETWEEN_GE_LT:
    case PT_BETWEEN_GT_LE:
    case PT_BETWEEN_GT_LT:
    case PT_BETWEEN_EQ_NA:
    case PT_BETWEEN_INF_LE:
    case PT_BETWEEN_INF_LT:
    case PT_BETWEEN_GE_INF:
    case PT_BETWEEN_GT_INF:
    case PT_RANGE:
      return 1;
    default:
      return 0;
    }
}


/*
 * pt_negate_op () -
 *   return:
 *   op(in):
 */
PT_OP_TYPE
pt_negate_op (PT_OP_TYPE op)
{
  switch (op)
    {
    case PT_EQ:
      return PT_NE;
    case PT_NE:
      return PT_EQ;
    case PT_SETEQ:
      return PT_SETNEQ;
    case PT_SETNEQ:
      return PT_SETEQ;
    case PT_GT:
      return PT_LE;
    case PT_GE:
      return PT_LT;
    case PT_LT:
      return PT_GE;
    case PT_LE:
      return PT_GT;
    case PT_BETWEEN:
      return PT_NOT_BETWEEN;
    case PT_NOT_BETWEEN:
      return PT_BETWEEN;
    case PT_IS_IN:
      return PT_IS_NOT_IN;
    case PT_IS_NOT_IN:
      return PT_IS_IN;
    case PT_LIKE:
      return PT_NOT_LIKE;
    case PT_NOT_LIKE:
      return PT_LIKE;
    case PT_RLIKE:
      return PT_NOT_RLIKE;
    case PT_NOT_RLIKE:
      return PT_RLIKE;
    case PT_RLIKE_BINARY:
      return PT_NOT_RLIKE_BINARY;
    case PT_NOT_RLIKE_BINARY:
      return PT_RLIKE_BINARY;
    case PT_IS_NULL:
      return PT_IS_NOT_NULL;
    case PT_IS_NOT_NULL:
      return PT_IS_NULL;
    case PT_EQ_SOME:
      return PT_NE_ALL;
    case PT_NE_SOME:
      return PT_EQ_ALL;
    case PT_GT_SOME:
      return PT_LE_ALL;
    case PT_GE_SOME:
      return PT_LT_ALL;
    case PT_LT_SOME:
      return PT_GE_ALL;
    case PT_LE_SOME:
      return PT_GT_ALL;
    case PT_EQ_ALL:
      return PT_NE_SOME;
    case PT_NE_ALL:
      return PT_EQ_SOME;
    case PT_GT_ALL:
      return PT_LE_SOME;
    case PT_GE_ALL:
      return PT_LT_SOME;
    case PT_LT_ALL:
      return PT_GE_SOME;
    case PT_LE_ALL:
      return PT_GT_SOME;
    case PT_IS:
      return PT_IS_NOT;
    case PT_IS_NOT:
      return PT_IS;
    default:
      return (PT_OP_TYPE) 0;
    }
}


/*
 * pt_comp_to_between_op () -
 *   return:
 *   left(in):
 *   right(in):
 *   type(in):
 *   between(out):
 */
int
pt_comp_to_between_op (PT_OP_TYPE left,
		       PT_OP_TYPE right,
		       PT_COMP_TO_BETWEEN_OP_CODE_TYPE
		       type, PT_OP_TYPE * between)
{
  size_t i;

  for (i = 0; i < COMPARE_BETWEEN_OPERATOR_COUNT; i++)
    {
      if (left == pt_Compare_between_operator_table[i].left
	  && right == pt_Compare_between_operator_table[i].right)
	{
	  *between = pt_Compare_between_operator_table[i].between;

	  return 0;
	}
    }

  if (type == PT_RANGE_INTERSECTION)
    {				/* range intersection */
      if ((left == PT_GE && right == PT_EQ)
	  || (left == PT_EQ && right == PT_LE))
	{
	  *between = PT_BETWEEN_EQ_NA;
	  return 0;
	}
    }

  return -1;
}


/*
 * pt_between_to_comp_op () -
 *   return:
 *   between(in):
 *   left(out):
 *   right(out):
 */
int
pt_between_to_comp_op (PT_OP_TYPE between,
		       PT_OP_TYPE * left, PT_OP_TYPE * right)
{
  size_t i;

  for (i = 0; i < COMPARE_BETWEEN_OPERATOR_COUNT; i++)
    if (between == pt_Compare_between_operator_table[i].between)
      {
	*left = pt_Compare_between_operator_table[i].left;
	*right = pt_Compare_between_operator_table[i].right;

	return 0;
      }

  return -1;
}

/*
 * pt_get_equivalent_type_with_op () - get the type to which a node should be
 *			       converted to in order to match an expression
 *			       definition;
 *   return	  : the new type
 *   def_type(in) : the type defined in the expression signature
 *   arg_type(in) : the type of the received expression argument
 *   op(in)	  : operator
 *
 *  Note : this is a wrapper for 'pt_get_equivalent_type' : the default
 *	   equivalent type may be overridden for certain operators
 */
static PT_TYPE_ENUM
pt_get_equivalent_type_with_op (const PT_ARG_TYPE def_type,
				const PT_TYPE_ENUM arg_type, PT_OP_TYPE op)
{
  if (pt_is_op_hv_late_bind (op)
      && (def_type.is_generic && arg_type == PT_TYPE_MAYBE))
    {
      /* leave undetermined type */
      return PT_TYPE_MAYBE;
    }
  return pt_get_equivalent_type (def_type, arg_type);
}

/*
 * pt_is_op_hv_late_bind () - checks if the operator is in the list of
 *			      operators that should perform late binding on
 *			      their host variable arguments
 *
 *   return: true if arguments types should be mirrored
 *   op(in): operator type
 *
 *  Note: this functions is used by type inference algorithm to check if an
 *	  expression should leave its HV arguments as TYPE_MAYBE (the default
 *	  type inference behavior would be to match it with a concrete type
 *	  according to one of its signatures). Also, such expression is
 *	  wrapped with cast rather then its result type be forced to an
 *	  "expected domain" dictated by the expression context.
 */
bool
pt_is_op_hv_late_bind (PT_OP_TYPE op)
{
  switch (op)
    {
    case PT_ABS:
    case PT_CEIL:
    case PT_FLOOR:
    case PT_PLUS:
    case PT_DIVIDE:
    case PT_MODULUS:
    case PT_TIMES:
    case PT_MINUS:
    case PT_ROUND:
    case PT_TRUNC:
    case PT_UNARY_MINUS:
    case PT_EVALUATE_VARIABLE:
    case PT_DEFINE_VARIABLE:
    case PT_ADDTIME:
    case PT_TO_CHAR:
    case PT_HEX:
    case PT_CONV:
    case PT_ASCII:
    case PT_IFNULL:
    case PT_NVL:
    case PT_NVL2:
    case PT_COALESCE:
    case PT_NULLIF:
    case PT_LEAST:
    case PT_GREATEST:
      return true;
    default:
      return false;
    }
  return false;
}

/*
 * pt_wrap_expr_w_exp_dom_cast () - checks if the expression requires wrapping
 *	      with a cast to the type set in expected domain and performs the
 *	      wrap with cast if needed.
 *
 *   return: new node (if wrap is performed), or unaltered node, if wrap is
 *	     not needed
 *   parser(in): parser context
 *   expr(in): expression node to be checked and wrapped
 */
static PT_NODE *
pt_wrap_expr_w_exp_dom_cast (PARSER_CONTEXT * parser, PT_NODE * expr)
{
  /*expressions returning MAYBE, but with an expected domain are wrapped with
   * cast*/
  if (expr != NULL
      && expr->type_enum == PT_TYPE_MAYBE
      && pt_is_op_hv_late_bind (expr->info.expr.op)
      && expr->expected_domain != NULL)
    {
      PT_NODE *new_expr = NULL;

      if (expr->type_enum == DB_TYPE_ENUMERATION)
	{
	  /* expressions should not return PT_TYPE_ENUMERATION */
	  assert (false);
	  PT_INTERNAL_ERROR (parser,
			     "INVALID expected domain (PT_TYPE_ENUMERATION)");
	  return NULL;
	}

      new_expr = pt_wrap_with_cast_op (parser, expr,
				       pt_db_to_type_enum
				       (expr->expected_domain->type->id),
				       expr->expected_domain->precision,
				       expr->expected_domain->scale, NULL);

      if (new_expr != NULL)
	{
	  /* reset expected domain of wrapped expression to NULL: it will be
	   * replaced at XASL generation with DB_TYPE_VARIABLE domain*/
	  expr->expected_domain = NULL;
	  expr = new_expr;
	}
    }

  return expr;
}

/*
 * pt_is_op_with_forced_common_type () - checks if the operator is in the list
 *			      of operators that should force its arguments to
 *			      the same type if none of the arguments has a
 *			      determined type
 *
 *   return: true if arguments types should be mirrored
 *   op(in): operator type
 *
 *  Note: this functions is used by type inference algorithm
 */
bool
pt_is_op_with_forced_common_type (PT_OP_TYPE op)
{
  switch (op)
    {
    case PT_IFNULL:
    case PT_COALESCE:
    case PT_NVL:
    case PT_NVL2:
    case PT_NULLIF:
    case PT_LEAST:
    case PT_GREATEST:
    case PT_BETWEEN:
      return true;
    default:
      return false;
    }
  return false;
}

/*
 * pt_check_const_fold_op_w_args () - checks if it is safe to perform constant
 *				     folding on the expression, given the
 *				     arguments' values
 *   return: true if folding is allowed, false otherwise
 *   op(in): a PT_OP_TYPE (the desired operation)
 *   arg1(in): 1st db_value operand
 *   arg2(in): 2nd db_value operand
 *   arg3(in): 3rd db_value operand
 *
 *  Note : this function is used in context of constant folding to check if
 *	   folding an expression produces a large sized (string) result
 *	   which may cause performance problem
 */
static bool
pt_check_const_fold_op_w_args (PT_OP_TYPE op,
			       DB_VALUE * arg1,
			       DB_VALUE * arg2, DB_VALUE * arg3)
{
  const int MAX_RESULT_SIZE_ON_CONST_FOLDING = 256;
  switch (op)
    {
    case PT_SPACE:
      if (DB_VALUE_DOMAIN_TYPE (arg1) == DB_TYPE_INTEGER)
	{
	  int count_i = DB_GET_INTEGER (arg1);
	  if (count_i > MAX_RESULT_SIZE_ON_CONST_FOLDING)
	    {
	      return false;
	    }
	}
      else if (DB_VALUE_DOMAIN_TYPE (arg1) == DB_TYPE_SHORT)
	{
	  short count_sh = DB_GET_SHORT (arg1);
	  if (count_sh > MAX_RESULT_SIZE_ON_CONST_FOLDING)
	    {
	      return false;
	    }
	}
      else if (DB_VALUE_DOMAIN_TYPE (arg1) == DB_TYPE_BIGINT)
	{
	  DB_BIGINT count_b = DB_GET_BIGINT (arg1);
	  if (count_b > MAX_RESULT_SIZE_ON_CONST_FOLDING)
	    {
	      return false;
	    }
	}
      break;

    case PT_REPEAT:
      if (DB_VALUE_DOMAIN_TYPE (arg2) == DB_TYPE_INTEGER)
	{
	  int count_i = DB_GET_INTEGER (arg2);
	  if (QSTR_IS_ANY_CHAR (DB_VALUE_DOMAIN_TYPE (arg1)))
	    {
	      int arg1_len = DB_GET_STRING_SIZE (arg1);

	      if (arg1_len * count_i > MAX_RESULT_SIZE_ON_CONST_FOLDING)
		{
		  return false;
		}
	    }
	}
      break;

    case PT_LPAD:
    case PT_RPAD:
      /* check if constant folding is OK */
      if (DB_VALUE_DOMAIN_TYPE (arg2) == DB_TYPE_INTEGER)
	{
	  int count_i = DB_GET_INTEGER (arg2);
	  if (QSTR_IS_ANY_CHAR (DB_VALUE_DOMAIN_TYPE (arg3)))
	    {
	      int arg3_len = DB_GET_STRING_SIZE (arg3);

	      if (arg3_len * count_i > MAX_RESULT_SIZE_ON_CONST_FOLDING)
		{
		  return false;
		}
	    }
	}
      break;

    default:
      break;
    }

  return true;
}

/*
 * pt_is_op_w_collation () - check if is required to check collation or
 *			     codeset of this operator
 *
 *   return:
 *   node(in): a parse tree node
 *
 */
static bool
pt_is_op_w_collation (const PT_OP_TYPE op)
{
  switch (op)
    {
    case PT_EQ:
    case PT_NE:
    case PT_GE:
    case PT_GT:
    case PT_LT:
    case PT_LE:
    case PT_NULLSAFE_EQ:
    case PT_BETWEEN:
    case PT_BETWEEN_GE_LE:
    case PT_BETWEEN_GE_LT:
    case PT_BETWEEN_GT_LE:
    case PT_BETWEEN_GT_LT:
    case PT_CONCAT:
    case PT_CONCAT_WS:
    case PT_PLUS:
    case PT_LIKE:
    case PT_NOT_LIKE:
    case PT_SUBSTRING_INDEX:
    case PT_RPAD:
    case PT_LPAD:
    case PT_MID:
    case PT_SUBSTRING:
    case PT_REPLACE:
    case PT_TRANSLATE:
    case PT_COALESCE:
    case PT_STRCAT:
    case PT_TIME_FORMAT:
    case PT_DATE_FORMAT:
    case PT_TIMEF:
    case PT_DATEF:
    case PT_SETEQ:
    case PT_SETNEQ:
    case PT_SUBSET:
    case PT_SUBSETEQ:
    case PT_SUPERSET:
    case PT_SUPERSETEQ:
    case PT_GREATEST:
    case PT_LEAST:
    case PT_NULLIF:
    case PT_LOWER:
    case PT_UPPER:
    case PT_RTRIM:
    case PT_LTRIM:
    case PT_TRIM:
    case PT_LEFT:
    case PT_RIGHT:
    case PT_NVL:
    case PT_NVL2:
    case PT_IFNULL:
    case PT_IS_IN:
    case PT_IS_NOT_IN:
    case PT_EQ_SOME:
    case PT_NE_SOME:
    case PT_GE_SOME:
    case PT_GT_SOME:
    case PT_LT_SOME:
    case PT_LE_SOME:
    case PT_EQ_ALL:
    case PT_NE_ALL:
    case PT_GE_ALL:
    case PT_GT_ALL:
    case PT_LT_ALL:
    case PT_LE_ALL:
    case PT_FINDINSET:
    case PT_INSTR:
    case PT_LOCATE:
    case PT_POSITION:
    case PT_STRCMP:
    case PT_IF:
    case PT_FIELD:
      return true;
    default:
      return false;
    }
  return false;
}

/*
 * pt_get_collation_info () - get the collation info of parse tree node
 *
 *   return: true if node has collation
 *   node(in): a parse tree node
 *   coll_id(out): collation of node
 *   codeset(out): codeset of node
 *   coerc_level: collation coercibility level of node
 *
 */
bool
pt_get_collation_info (PT_NODE * node, int *coll_id, INTL_CODESET * codeset,
		       PT_COLL_COERC_LEV * coerc_level)
{
  bool has_collation = false;
  assert (node != NULL);

  *coerc_level = PT_COLLATION_NOT_COERC;
  *codeset = LANG_COERCIBLE_CODESET;
  *coll_id = LANG_COERCIBLE_COLL;

  if (node->data_type != NULL)
    {
      if (PT_HAS_COLLATION (node->type_enum))
	{
	  *coll_id = node->data_type->info.data_type.collation_id;
	  *codeset = node->data_type->info.data_type.units;
	  has_collation = true;
	}
    }
  else if (node->expected_domain != NULL)
    {
      if (TP_TYPE_HAS_COLLATION (TP_DOMAIN_TYPE (node->expected_domain)))
	{
	  *coll_id = TP_DOMAIN_COLLATION (node->expected_domain);
	  *codeset = TP_DOMAIN_CODESET (node->expected_domain);
	  has_collation = true;
	}
    }
  else
    {
      if (node->type_enum == PT_TYPE_MAYBE
	  || (node->node_type == PT_VALUE
	      && PT_HAS_COLLATION (node->type_enum)))
	{
	  *coll_id = lang_get_client_collation ();
	  *codeset = lang_get_client_charset ();
	  has_collation = true;
	}
    }

  if (has_collation && PT_GET_COLLATION_MODIFIER (node) != -1)
    {
      LANG_COLLATION *lc =
	lang_get_collation (PT_GET_COLLATION_MODIFIER (node));

      assert (lc != NULL);

      *coll_id = PT_GET_COLLATION_MODIFIER (node);

      if (node->data_type != NULL)
	{
	  assert (node->data_type->info.data_type.units == lc->codeset);
	}
      else if (node->expected_domain != NULL)
	{
	  assert (TP_DOMAIN_CODESET (node->expected_domain) == lc->codeset);
	}

      *coerc_level = PT_COLLATION_NOT_COERC;
      return has_collation;
    }

  switch (node->node_type)
    {
    case PT_VALUE:
      if (*coll_id == LANG_COLL_ISO_BINARY)
	{
	  *coerc_level = PT_COLLATION_L4_ISO_BIN_COERC;
	}
      else if (LANG_IS_COERCIBLE_COLL (*coll_id))
	{
	  *coerc_level = PT_COLLATION_L4_BIN_COERC;
	}
      else
	{
	  *coerc_level = PT_COLLATION_L4_COERC;
	}
      break;

    case PT_HOST_VAR:
      *coerc_level = PT_COLLATION_FULLY_COERC;
      break;

    case PT_EXPR:
      if (node->info.expr.op == PT_CURRENT_USER
	  || node->info.expr.op == PT_USER
	  || node->info.expr.op == PT_DATABASE
	  || node->info.expr.op == PT_SCHEMA
	  || node->info.expr.op == PT_VERSION)
	{
	  *coerc_level = PT_COLLATION_L3_COERC;
	  break;
	}

      if (node->info.expr.op == PT_EVALUATE_VARIABLE
	  || node->info.expr.op == PT_DEFINE_VARIABLE)
	{
	  *coerc_level = PT_COLLATION_FULLY_COERC;
	  break;
	}

      if (node->info.expr.op == PT_CAST
	  && PT_EXPR_INFO_IS_FLAGED (node, PT_EXPR_INFO_CAST_SHOULD_FOLD))
	{
	  int dummy;
	  INTL_CODESET dummy_cs;
	  /* collation and codeset of wrapped CAST, but get coercibility
	   * from original node */
	  pt_get_collation_info (node->info.expr.arg1, &dummy, &dummy_cs,
				 coerc_level);
	  break;
	}
      /* fall through */
    case PT_SELECT:
    case PT_UNION:
    case PT_DIFFERENCE:
    case PT_INTERSECTION:
    case PT_FUNCTION:
    case PT_METHOD_CALL:
      if (*coll_id == LANG_COLL_ISO_BINARY)
	{
	  *coerc_level = PT_COLLATION_L2_ISO_BIN_COERC;
	}
      else if (LANG_IS_COERCIBLE_COLL (*coll_id))
	{
	  *coerc_level = PT_COLLATION_L2_BIN_COERC;
	}
      else
	{
	  *coerc_level = PT_COLLATION_L2_COERC;
	}
      break;

    case PT_NAME:
    case PT_DOT_:
      if (*coll_id == LANG_COLL_ISO_BINARY)
	{
	  *coerc_level = PT_COLLATION_L1_ISO_BIN_COERC;
	}
      else if (LANG_IS_COERCIBLE_COLL (*coll_id))
	{
	  *coerc_level = PT_COLLATION_L1_BIN_COERC;
	}
      else
	{
	  *coerc_level = PT_COLLATION_L1_COERC;
	}
      break;

    default:
      *coerc_level = PT_COLLATION_NOT_COERC;
    }

  return has_collation;
}

/*
 * pt_get_collation_info_for_collection_type () - get the collation info of a
 *	    parse tree node with collection type
 *
 *   return:  0 = node doesn't have collation;
 *	      1 = node has collation
 *	     -1 = node has multiple component types with collation and
 *		  collations are not compatible
 *
 *   node(in): a parse tree node
 *   coll_id(out): collation of node
 *   codeset(out): codeset of node
 *   coerc_level: collation coercibility level of node
 *
 */
static int
pt_get_collation_info_for_collection_type (PARSER_CONTEXT * parser,
					   PT_NODE * node, int *coll_id,
					   INTL_CODESET * codeset,
					   PT_COLL_COERC_LEV * coerc_level)
{
  bool has_collation = false;
  assert (node != NULL);

  *coerc_level = PT_COLLATION_NOT_COERC;
  *codeset = LANG_COERCIBLE_CODESET;
  *coll_id = LANG_COERCIBLE_COLL;

  assert (PT_IS_COLLECTION_TYPE (node->type_enum));

  if (node->data_type != NULL)
    {
      PT_NODE *dt_node = node->data_type;

      /* check all collatable types of collection */
      do
	{
	  if (PT_HAS_COLLATION (dt_node->type_enum))
	    {
	      if ((!LANG_IS_COERCIBLE_COLL (*coll_id)
		   || *codeset != LANG_COERCIBLE_CODESET)
		  && (*coll_id != dt_node->info.data_type.collation_id
		      || *codeset != dt_node->info.data_type.units))
		{
		  /* error : different collations in same collection */
		  goto error;
		}

	      *codeset = (INTL_CODESET) dt_node->info.data_type.units;
	      *coll_id = dt_node->info.data_type.collation_id;
	      has_collation = true;
	    }

	  dt_node = dt_node->next;
	}
      while (dt_node != NULL);
    }
  else
    {
      if (node->node_type == PT_FUNCTION
	  && node->info.function.arg_list != NULL
	  && node->info.function.arg_list->type_enum == PT_TYPE_MAYBE)
	{
	  /* charset and collation of system */
	  has_collation = true;
	}
    }

  if (!has_collation)
    {
      return 0;
    }

  if (has_collation && PT_GET_COLLATION_MODIFIER (node) != -1)
    {
      LANG_COLLATION *lc =
	lang_get_collation (PT_GET_COLLATION_MODIFIER (node));

      assert (lc != NULL);

      *coll_id = PT_GET_COLLATION_MODIFIER (node);

      assert (*codeset == lc->codeset);

      *coerc_level = PT_COLLATION_NOT_COERC;
      return 1;
    }

  switch (node->node_type)
    {
    case PT_VALUE:
      assert (has_collation);
      assert (has_collation);
      if (*coll_id == LANG_COLL_ISO_BINARY)
	{
	  *coerc_level = PT_COLLATION_L4_ISO_BIN_COERC;
	}
      else if (LANG_IS_COERCIBLE_COLL (*coll_id))
	{
	  *coerc_level = PT_COLLATION_L4_BIN_COERC;
	}
      else
	{
	  *coerc_level = PT_COLLATION_L4_COERC;
	}
      break;

    case PT_HOST_VAR:
      assert (has_collation);
      *coerc_level = PT_COLLATION_FULLY_COERC;
      break;

    case PT_EXPR:
    case PT_FUNCTION:
    case PT_SELECT:
    case PT_UNION:
    case PT_DIFFERENCE:
    case PT_INTERSECTION:
      if ((!has_collation && LANG_SYS_COLLATION == LANG_COLL_ISO_BINARY)
	  || (*coll_id == LANG_COLL_ISO_BINARY))
	{
	  *coerc_level = PT_COLLATION_L2_ISO_BIN_COERC;
	}
      else if (!has_collation || LANG_IS_COERCIBLE_COLL (*coll_id))
	{
	  *coerc_level = PT_COLLATION_L2_BIN_COERC;
	}
      else
	{
	  *coerc_level = PT_COLLATION_L2_COERC;
	}
      break;

    case PT_NAME:
    case PT_DOT_:
      assert (has_collation);
      if (*coll_id == LANG_COLL_ISO_BINARY)
	{
	  *coerc_level = PT_COLLATION_L1_ISO_BIN_COERC;
	}
      else if (LANG_IS_COERCIBLE_COLL (*coll_id))
	{
	  *coerc_level = PT_COLLATION_L1_BIN_COERC;
	}
      else
	{
	  *coerc_level = PT_COLLATION_L1_COERC;
	}
      break;

    default:
      assert (!has_collation);
      *coerc_level = PT_COLLATION_NOT_COERC;
    }

  return has_collation ? 1 : 0;

error:
  PT_ERRORm (parser, node, MSGCAT_SET_PARSER_SEMANTIC,
	     MSGCAT_SEMANTIC_COLLECTION_EL_COLLATION_ERROR);

  return -1;
}

/*
 * pt_coerce_node_collation () - get the collation info of parse tree node
 *
 *   return: parse node after coercion
 *   node(in): a parse tree node
 *   coll_id(in): collation
 *   codeset(in): codeset
 *   force_mode(in): true if codeset and collation have to forced
 *   use_collate_modifier(in): true if collation coercion should be done using
 *			       a COLLATE expression modifier
 *
 *  Note : 'force_mode' controlls how new collation and charset are applied:
 *	   When 'force_mode' in set, collation and charset are forced;
 *	   if not set, and codeset change is detected, then collation
 *	   coercion will require a CAST (wrap with cast) or a value coerce
 *	   in order to ensure charset conversion. If charset doesn't change,
 *	   it is safe to force directly the new collation.
 *	   When 'node' is an argument of an expression 'force_mode' is false;
 *	   'force_mode' is on when 'node' is a result (expression) : for this
 *	   case it is assumed the algorithm ensures the result's collation and
 *	   codeset by previously applying the coercion on its arguments.
 *
 *	   use_collate_modifier : if true and wrap_with_cast is done, the
 *	   CAST operator is flagged with PT_EXPR_INFO_CAST_COLL_MODIFIER; this 
 *	   flag is set only when CAST operation does not change charset, but
 *	   only collation; this kind of CAST is not transformed into T_CAST
 *	   during XASL generation, but into a flagged REGU_VARIABLE which 
 *	   "knows" to "update" its collation after "fetch". See usage of 
 *	   REGU_VARIABLE_APPLY_COLLATION flag.
 *
 */
static PT_NODE *
pt_coerce_node_collation (PARSER_CONTEXT * parser, PT_NODE * node,
			  const int coll_id, const INTL_CODESET codeset,
			  bool force_mode, bool use_collate_modifier)
{
  assert (node != NULL);

  switch (node->node_type)
    {
    case PT_NAME:
    case PT_DOT_:
    case PT_EXPR:
    case PT_SELECT:
    case PT_FUNCTION:
    case PT_METHOD_CALL:
    case PT_UNION:
    case PT_INTERSECTION:
    case PT_DIFFERENCE:
      if (!PT_HAS_COLLATION (node->type_enum)
	  && !PT_IS_COLLECTION_TYPE (node->type_enum))
	{
	  goto cannot_coerce;
	}
      break;
    case PT_VALUE:
    case PT_HOST_VAR:
      break;
    default:
      /* by default, no not coerce */
      goto cannot_coerce;
    }

  if (node->data_type != NULL)
    {
      if (PT_IS_COLLECTION_TYPE (node->type_enum))
	{
	  PT_NODE *dt_node;
	  PT_NODE *dt = NULL;
	  bool apply_wrap_cast = false;

	  dt_node = node->data_type;

	  /* check if wrap with cast is necessary */
	  if (!force_mode
	      || node->node_type != PT_EXPR || node->info.expr.op != PT_CAST)
	    {
	      do
		{
		  if (PT_HAS_COLLATION (dt_node->type_enum)
		      && dt_node->info.data_type.collation_id != coll_id)
		    {
		      apply_wrap_cast = true;
		      break;
		    }

		  dt_node = dt_node->next;
		}
	      while (dt_node != NULL);
	    }

	  if (apply_wrap_cast)
	    {
	      dt = parser_copy_tree_list (parser, node->data_type);
	      dt_node = dt;
	    }
	  else
	    {
	      dt_node = node->data_type;
	    }

	  /* apply new collation and codeset for all collatable sub-types */
	  do
	    {
	      if (PT_HAS_COLLATION (dt_node->type_enum))
		{
		  dt_node->info.data_type.collation_id = coll_id;
		  dt_node->info.data_type.units = (int) codeset;
		}

	      dt_node = dt_node->next;
	    }
	  while (dt_node != NULL);

	  if (apply_wrap_cast)
	    {
	      node = pt_wrap_collection_with_cast_op (parser, node,
						      node->type_enum, dt);
	      /* flag PT_EXPR_INFO_CAST_COLL_MODIFIER is not set here;
	       * COLLATE modifier is not supported for this context */
	    }
	  else if (dt != NULL)
	    {
	      parser_free_node (parser, dt);
	    }
	}
      else
	{
	  assert (PT_HAS_COLLATION (node->type_enum));

	  /* always wrap with cast except when node is CAST expression (avoid
	     CAST recursion) or force mode is enabled */
	  if (!force_mode
	      && (node->data_type->info.data_type.collation_id != coll_id)
	      && (node->node_type != PT_EXPR
		  || node->info.expr.op != PT_CAST)
	      && (node->node_type != PT_HOST_VAR))
	    {
	      PT_NODE *dt = parser_copy_tree_list (parser, node->data_type);

	      dt->info.data_type.collation_id = coll_id;
	      dt->info.data_type.units = (int) codeset;

	      if (node->node_type == PT_VALUE
		  && codeset == INTL_CODESET_ISO88591
		  && node->data_type->info.data_type.units !=
		  INTL_CODESET_ISO88591)
		{
		  /* cannot have values of ENUM type here */
		  assert (PT_IS_CHAR_STRING_TYPE (node->type_enum));
		  /* converting from multibyte charset to ISO charset, may
		   * truncate the string data (precision is kept);
		   * this workaround ensures that new precision (after charset
		   * conversion) grows to the size in bytes of original data:
		   * conversion rule from multibyte charset to ISO is to
		   * reinterpret the bytes as ISO characters.
		   */
		  if (node->info.value.data_value.str != NULL)
		    {
		      dt->info.data_type.precision =
			node->info.value.data_value.str->length;
		    }
		  else if (node->info.value.db_value_is_initialized)
		    {
		      dt->info.data_type.precision =
			DB_GET_STRING_SIZE (&(node->info.value.db_value));
		    }
		}


	      if (node->node_type == PT_SELECT
		  || node->node_type == PT_DIFFERENCE
		  || node->node_type == PT_INTERSECTION
		  || node->node_type == PT_UNION)
		{
		  PT_NODE *select_list;
		  int nb_select_list;

		  if (node->node_type == PT_SELECT)
		    {
		      select_list = node->info.query.q.select.list;
		    }
		  else
		    {
		      /* It is enough to count the number of select list items
		       * from one of the union/intersect/difference arguments.
		       * If they would be different, this code would not be
		       * reached, an arg incompatibility is thrown before.
		       * Because of the left side recursivity of table ops,
		       * arg2 is always a PT_SELECT.
		       */
		      PT_NODE *union_arg2 = node->info.query.q.union_.arg2;
		      assert (union_arg2->node_type == PT_SELECT);
		      select_list = union_arg2->info.query.q.select.list;
		    }

		  nb_select_list = pt_length_of_list (select_list);
		  if (nb_select_list != 1)
		    {
		      goto cannot_coerce;
		    }
		  if (pt_wrap_select_list_with_cast_op (parser, node,
							node->type_enum,
							dt->info.data_type.
							precision,
							dt->info.data_type.
							dec_precision,
							dt, true) != NO_ERROR)
		    {
		      goto cannot_coerce;
		    }
		  /* flag PT_EXPR_INFO_CAST_COLL_MODIFIER is not set here;
		   * COLLATE modifier is not supported for this context */
		}
	      else
		{
		  if (node->node_type == PT_VALUE)
		    {
		      /* force using COLLATE modifier for VALUEs;
		       * this avoids printing an invalid data type (CHAR(-1)
		       * in PT_CAST expression */
		      use_collate_modifier = true;
		    }

		  node =
		    pt_wrap_with_cast_op (parser, node, node->type_enum,
					  dt->info.data_type.precision,
					  dt->info.data_type.dec_precision,
					  dt);
		  if (node != NULL && use_collate_modifier)
		    {
		      assert (node->node_type == PT_EXPR
			      && node->info.expr.op == PT_CAST);
		      PT_EXPR_INFO_SET_FLAG (node,
					     PT_EXPR_INFO_CAST_COLL_MODIFIER);
		      PT_SET_NODE_COLL_MODIFIER (node, coll_id);
		    }
		}

	      /* 'dt' is copied in 'pt_wrap_with_cast_op' */
	      parser_free_node (parser, dt);
	    }
	  else
	    {
	      node->data_type->info.data_type.collation_id = coll_id;
	      node->data_type->info.data_type.units = (int) codeset;
	    }
	}
    }
  else if (node->expected_domain != NULL
	   && (TP_DOMAIN_CODESET (node->expected_domain) != codeset
	       || TP_DOMAIN_COLLATION (node->expected_domain) != coll_id))
    {
      TP_DOMAIN *new_domain;

      if (PT_IS_COLLECTION_TYPE (node->type_enum))
	{
	  goto cannot_coerce;
	}

      assert (TP_TYPE_HAS_COLLATION (TP_DOMAIN_TYPE (node->expected_domain)));

      if (node->expected_domain->is_cached)
	{
	  /* create new domain */
	  new_domain = tp_domain_copy (node->expected_domain, false);
	  new_domain->codeset = (unsigned char) codeset;
	  new_domain->collation_id = coll_id;

	  /* the existing 'expected_domain' may have been created for this
	   * specific node and cached, it will remain cached */
	  new_domain = tp_domain_cache (new_domain);
	  node->expected_domain = new_domain;
	}
      else
	{
	  /* safe to change the domain directly */
	  node->expected_domain->codeset = (unsigned char) codeset;
	  node->expected_domain->collation_id = coll_id;
	}
    }
  else
    {
      return node;
    }

  switch (node->node_type)
    {
    case PT_VALUE:
      if (node->info.value.db_value_is_initialized && node->data_type != NULL)
	{
	  if (PT_IS_COLLECTION_TYPE (node->type_enum))
	    {
	      int i;
	      DB_VALUE *sub_value = NULL;
	      SETREF *setref = DB_PULL_SET (&(node->info.value.db_value));
	      int set_size = setobj_size (setref->set);

	      for (i = 0; i < set_size; i++)
		{
		  setobj_get_element_ptr (setref->set, i, &sub_value);
		  if (sub_value != NULL
		      && TP_IS_CHAR_TYPE (DB_VALUE_TYPE (sub_value)))
		    {
		      db_string_put_cs_and_collation (sub_value,
						      (unsigned char) codeset,
						      coll_id);
		    }
		}
	    }
	  else
	    {
	      assert (PT_IS_CHAR_STRING_TYPE (node->type_enum));
	      db_string_put_cs_and_collation (&(node->info.value.db_value),
					      (unsigned char) codeset,
					      coll_id);
	    }
	}
      break;
    case PT_HOST_VAR:
      if (node->expected_domain != NULL)
	{
	  pt_preset_hostvar (parser, node);
	}
      else if (node->data_type != NULL
	       && PT_HAS_COLLATION (node->data_type->type_enum))
	{
	  /* this is a HV from an auto-parametrization */
	  node->data_type->info.data_type.collation_id = coll_id;
	  node->data_type->info.data_type.units = (int) codeset;
	}
      break;
    case PT_EXPR:
      /* special case : CAST */
      if (node->info.expr.op == PT_CAST && node->info.expr.cast_type != NULL)
	{
	  /* propagate the collation and codeset to cast */
	  PT_NODE *cast_type = node->info.expr.cast_type;

	  if (PT_IS_COLLECTION_TYPE (cast_type->type_enum))
	    {
	      PT_NODE *dt_node = cast_type->data_type;

	      assert (dt_node != NULL);

	      /* force collation on each collection component */
	      do
		{
		  if (PT_HAS_COLLATION (dt_node->type_enum))
		    {
		      dt_node->info.data_type.collation_id = coll_id;
		      dt_node->info.data_type.units = (int) codeset;
		    }

		  dt_node = dt_node->next;
		}
	      while (dt_node != NULL);
	    }
	  else
	    {
	      assert (PT_HAS_COLLATION (cast_type->type_enum));

	      cast_type->info.data_type.collation_id = coll_id;
	      cast_type->info.data_type.units = (int) codeset;
	    }
	}
      break;
    default:
      break;
    }

  return node;

cannot_coerce:
  if (codeset != LANG_COERCIBLE_CODESET || !LANG_IS_COERCIBLE_COLL (coll_id))
    {
      return NULL;
    }
  return node;
}



/*
 * pt_common_collation () - compute common collation of an operation
 *
 *   return: 0 comon collation and codeset have been detected, -1 otherwise
 *   arg1_coll(in): collation id of arg1
 *   arg1_cs(in): codeset of arg1
 *   arg1_coerc_level(in): coercibility level of arg1
 *   arg2_coll(in): collation id of arg2
 *   arg2_cs(in): codeset of arg2
 *   arg2_coerc_level(in): coercibility level of arg2
 *   arg3_coll(in): collation id of arg3
 *   arg3_cs(in): codeset of arg3
 *   arg3_coerc_level(in): coercibility level of arg3
 *   args_w_coll(in): how many arguments have collation
 *   op_has_3_args(in): true operation has 3 arguments
 *   common_coll(out): common collation detected
 *   common_cs(out): common codeset detected
 *
 */
int
pt_common_collation (const int arg1_coll, const INTL_CODESET arg1_cs,
		     PT_COLL_COERC_LEV arg1_coerc_level,
		     const int arg2_coll, const INTL_CODESET arg2_cs,
		     PT_COLL_COERC_LEV arg2_coerc_level,
		     const int arg3_coll, const INTL_CODESET arg3_cs,
		     PT_COLL_COERC_LEV arg3_coerc_level,
		     const int args_w_coll, bool op_has_3_args,
		     int *common_coll, INTL_CODESET * common_cs)
{
  assert (common_coll != NULL);
  assert (common_cs != NULL);

  /* arguments can have only one multibyte charset */
  if ((arg1_cs == INTL_CODESET_UTF8 || arg2_cs == INTL_CODESET_UTF8
       || arg3_cs == INTL_CODESET_UTF8)
      && (arg1_cs == INTL_CODESET_KSC5601_EUC
	  || arg2_cs == INTL_CODESET_KSC5601_EUC
	  || arg3_cs == INTL_CODESET_KSC5601_EUC))
    {
      goto error;
    }

  if (arg1_coll != arg2_coll && arg1_coerc_level == arg2_coerc_level)
    {
      goto error;
    }
  else if (arg1_coerc_level > arg2_coerc_level)
    {
      /* coerce arg1 collation */
      if (!INTL_CAN_COERCE_CS (arg1_cs, arg2_cs))
	{
	  goto error;
	}
      *common_coll = arg2_coll;
      *common_cs = arg2_cs;

      /* check arg3 */
      if (op_has_3_args && arg3_coll != *common_coll)
	{
	  bool set_arg3 = false;

	  if (arg2_coerc_level > arg3_coerc_level)
	    {
	      set_arg3 = true;
	    }
	  else if (arg2_coerc_level < arg3_coerc_level)
	    {
	      set_arg3 = false;
	    }
	  else
	    {
	      goto error;
	    }

	  if (set_arg3)
	    {
	      /* coerce arg2 collation */
	      if (!INTL_CAN_COERCE_CS (arg2_cs, arg3_cs))
		{
		  goto error;
		}

	      *common_coll = arg3_coll;
	      *common_cs = arg3_cs;
	    }
	  else
	    {
	      /* coerce arg3 collation */
	      if (!INTL_CAN_COERCE_CS (arg3_cs, arg2_cs))
		{
		  goto error;
		}

	      assert (*common_coll == arg2_coll);
	      assert (*common_cs == arg2_cs);
	    }
	}
    }
  else
    {
      assert ((arg1_coerc_level < arg2_coerc_level)
	      || (arg1_coerc_level == arg2_coerc_level
		  && arg1_coll == arg2_coll));

      /* coerce arg2 collation */
      if (!INTL_CAN_COERCE_CS (arg2_cs, arg1_cs))
	{
	  goto error;
	}

      *common_coll = arg1_coll;
      *common_cs = arg1_cs;

      /* check arg3 */
      if (op_has_3_args && arg3_coll != *common_coll)
	{
	  bool set_arg3 = false;
	  if (arg1_coerc_level > arg3_coerc_level)
	    {
	      set_arg3 = true;
	    }
	  else if (arg1_coerc_level < arg3_coerc_level)
	    {
	      set_arg3 = false;
	    }
	  else
	    {
	      goto error;
	    }

	  if (set_arg3)
	    {
	      /* coerce arg1 collation */
	      if (!INTL_CAN_COERCE_CS (arg1_cs, arg3_cs))
		{
		  goto error;
		}

	      *common_coll = arg3_coll;
	      *common_cs = arg3_cs;
	    }
	  else
	    {
	      /* coerce arg3 collation */
	      if (!INTL_CAN_COERCE_CS (arg3_cs, arg1_cs))
		{
		  goto error;
		}

	      assert (*common_coll == arg1_coll);
	      assert (*common_cs == arg1_cs);
	    }
	}
    }

  return 0;

error:

  return -1;
}

/*
 * pt_check_expr_collation () - checks the collation of an expression node
 *
 *   return: error code
 *   parser(in): parser context
 *   node(in): a parse tree expression node
 *
 */
static int
pt_check_expr_collation (PARSER_CONTEXT * parser, PT_NODE ** node)
{
  PT_TYPE_ENUM arg1_type = PT_TYPE_NONE, arg2_type = PT_TYPE_NONE;
  PT_TYPE_ENUM arg3_type = PT_TYPE_NONE;
  PT_OP_TYPE op;
  int arg1_coll = LANG_COERCIBLE_COLL, arg2_coll = LANG_COERCIBLE_COLL;
  int arg3_coll = LANG_COERCIBLE_COLL;
  INTL_CODESET arg1_cs = LANG_COERCIBLE_CODESET;
  INTL_CODESET arg2_cs = LANG_COERCIBLE_CODESET;
  INTL_CODESET arg3_cs = LANG_COERCIBLE_CODESET;
  PT_COLL_COERC_LEV arg1_coerc_level = PT_COLLATION_NOT_COERC;
  PT_COLL_COERC_LEV arg2_coerc_level = PT_COLLATION_NOT_COERC;
  PT_COLL_COERC_LEV arg3_coerc_level = PT_COLLATION_NOT_COERC;
  PT_NODE *expr = *node;
  PT_NODE *arg1, *arg2, *arg3;
  PT_NODE *new_node;
  int common_coll = LANG_COERCIBLE_COLL;
  INTL_CODESET common_cs = LANG_COERCIBLE_CODESET;
  int args_w_coll = 0;
  bool op_has_3_args;
  bool reverse_arg2_arg3;
  int expr_coll_modifier = -1;
  INTL_CODESET expr_cs_modifier = INTL_CODESET_NONE;
  bool use_cast_collate_modifier = false;

  assert (expr != NULL);
  assert (expr->node_type == PT_EXPR);

  /* NULL has no collation */
  if (expr->type_enum == PT_TYPE_NULL)
    {
      return NO_ERROR;
    }

  op = expr->info.expr.op;
  arg1 = expr->info.expr.arg1;
  arg2 = expr->info.expr.arg2;
  arg3 = expr->info.expr.arg3;

  if (op == PT_DEFINE_VARIABLE
      && pt_get_collation_info (arg2, &arg2_coll, &arg2_cs,
				&arg2_coerc_level))
    {
      if (arg2_coll != LANG_COERCIBLE_COLL)
	{
	  /* user defined variables must have sytem collation */
	  PT_ERRORmf (parser, *node, MSGCAT_SET_PARSER_SEMANTIC,
		      MSGCAT_SEMANTIC_SESSION_VAR_COLLATION,
		      lang_get_collation_name (LANG_COERCIBLE_COLL));
	  goto error_exit;
	}
    }
  expr_coll_modifier = PT_GET_COLLATION_MODIFIER (expr);

  if (expr_coll_modifier != -1)
    {
      LANG_COLLATION *lc = lang_get_collation (expr_coll_modifier);

      assert (lc != NULL);
      expr_cs_modifier = lc->codeset;
    }

  if (!pt_is_op_w_collation (op))
    {
      if (expr_coll_modifier != -1)
	{
	  if (!(op == PT_EVALUATE_VARIABLE || pt_is_comp_op (op))
	      && !PT_HAS_COLLATION (expr->type_enum))
	    {
	      PT_ERRORm (parser, expr, MSGCAT_SET_PARSER_SEMANTIC,
			 MSGCAT_SEMANTIC_COLLATE_NOT_ALLOWED);
	      goto error_exit;
	    }
	  goto coerce_result;
	}
      return NO_ERROR;
    }

  op_has_3_args =
    (op == PT_CONCAT_WS || op == PT_REPLACE || op == PT_TRANSLATE
     || op == PT_BETWEEN || op == PT_NOT_BETWEEN || op == PT_IF
     || op == PT_FIELD);
  reverse_arg2_arg3 = (op == PT_RPAD || op == PT_LPAD);

  /* step 1 : get info */
  if (reverse_arg2_arg3)
    {
      PT_NODE *tmp = arg2;
      arg2 = arg3;
      arg3 = tmp;
    }

  if (arg1)
    {
      arg1_type = arg1->type_enum;
    }

  if (arg2)
    {
      arg2_type = arg2->type_enum;
    }

  if (arg3)
    {
      arg3_type = arg3->type_enum;
    }

  if (PT_HAS_COLLATION (arg1_type) || arg1_type == PT_TYPE_MAYBE)
    {
      if (pt_get_collation_info (arg1, &arg1_coll, &arg1_cs,
				 &arg1_coerc_level))
	{
	  args_w_coll++;
	}

      if (arg1_type != PT_TYPE_MAYBE)
	{
	  common_coll = arg1_coll;
	  common_cs = arg1_cs;
	}
    }
  else if (PT_IS_COLLECTION_TYPE (arg1_type))
    {
      int status =
	pt_get_collation_info_for_collection_type (parser, arg1, &arg1_coll,
						   &arg1_cs,
						   &arg1_coerc_level);

      if (status == -1)
	{
	  goto error_exit;
	}
      else if (status == 1)
	{
	  args_w_coll++;
	  common_coll = arg1_coll;
	  common_cs = arg1_cs;
	}
    }

  if (PT_HAS_COLLATION (arg2_type) || arg2_type == PT_TYPE_MAYBE)
    {
      if (pt_get_collation_info (arg2, &arg2_coll, &arg2_cs,
				 &arg2_coerc_level))
	{
	  args_w_coll++;
	}

      if (arg2_type != PT_TYPE_MAYBE)
	{
	  common_coll = arg2_coll;
	  common_cs = arg2_cs;
	}
    }
  else if (PT_IS_COLLECTION_TYPE (arg2_type))
    {
      int status =
	pt_get_collation_info_for_collection_type (parser, arg2, &arg2_coll,
						   &arg2_cs,
						   &arg2_coerc_level);

      if (status == -1)
	{
	  goto error_exit;
	}
      else if (status == 1)
	{
	  args_w_coll++;
	  common_coll = arg2_coll;
	  common_cs = arg2_cs;
	}
    }

  if (op_has_3_args)
    {
      if (PT_HAS_COLLATION (arg3_type) || arg3_type == PT_TYPE_MAYBE)
	{
	  if (pt_get_collation_info (arg3, &arg3_coll, &arg3_cs,
				     &arg3_coerc_level))
	    {
	      args_w_coll++;
	    }

	  if (arg3_type != PT_TYPE_MAYBE)
	    {
	      common_coll = arg3_coll;
	      common_cs = arg3_cs;
	    }
	}
      else if (PT_IS_COLLECTION_TYPE (arg3_type))
	{
	  int status =
	    pt_get_collation_info_for_collection_type (parser, arg3,
						       &arg3_coll,
						       &arg3_cs,
						       &arg3_coerc_level);

	  if (status == -1)
	    {
	      goto error_exit;
	    }
	  else if (status == 1)
	    {
	      args_w_coll++;
	      common_coll = arg3_coll;
	      common_cs = arg3_cs;
	    }
	}
    }

  if (expr_coll_modifier != -1 && pt_is_comp_op (op) && args_w_coll > 0)
    {
      /* for comparisons, force the collation of each argument to have
       * the collation of expression */
      if (PT_HAS_COLLATION (arg1_type) && arg1_cs != expr_cs_modifier)
	{
	  PT_ERRORmf2 (parser, arg1, MSGCAT_SET_PARSER_SEMANTIC,
		       MSGCAT_SEMANTIC_CS_MATCH_COLLATE,
		       lang_get_codeset_name (arg1_cs),
		       lang_get_codeset_name (expr_cs_modifier));
	  goto error_exit;
	}
      if (PT_HAS_COLLATION (arg2_type) && arg2_cs != expr_cs_modifier)
	{
	  PT_ERRORmf2 (parser, arg2, MSGCAT_SET_PARSER_SEMANTIC,
		       MSGCAT_SEMANTIC_CS_MATCH_COLLATE,
		       lang_get_codeset_name (arg2_cs),
		       lang_get_codeset_name (expr_cs_modifier));
	  goto error_exit;
	}
      if (PT_HAS_COLLATION (arg3_type) && arg3_cs != expr_cs_modifier)
	{
	  PT_ERRORmf2 (parser, arg3, MSGCAT_SET_PARSER_SEMANTIC,
		       MSGCAT_SEMANTIC_CS_MATCH_COLLATE,
		       lang_get_codeset_name (arg3_cs),
		       lang_get_codeset_name (expr_cs_modifier));
	  goto error_exit;
	}

      common_cs = expr_cs_modifier;
      common_coll = expr_coll_modifier;
      use_cast_collate_modifier = true;
      goto coerce_arg;
    }

  if (args_w_coll <= 1)
    {
      goto coerce_result;
    }

  /* step 2 : compute collation to use */
  assert (args_w_coll >= 2);

  if (op_has_3_args)
    {
      if (args_w_coll < 3)
	{
	  if (!(PT_HAS_COLLATION (arg1_type) || arg1_type == PT_TYPE_MAYBE
		|| PT_IS_COLLECTION_TYPE (arg1_type)))
	    {
	      arg1_coll = common_coll;
	      arg1_cs = common_cs;
	      arg1_coerc_level = PT_COLLATION_FULLY_COERC;
	    }

	  if (!(PT_HAS_COLLATION (arg2_type) || arg2_type == PT_TYPE_MAYBE
		|| PT_IS_COLLECTION_TYPE (arg2_type)))
	    {
	      arg2_coll = common_coll;
	      arg2_cs = common_cs;
	      arg2_coerc_level = PT_COLLATION_FULLY_COERC;
	    }

	  if (!(PT_HAS_COLLATION (arg3_type) || arg3_type == PT_TYPE_MAYBE
		|| PT_IS_COLLECTION_TYPE (arg3_type)))
	    {
	      arg3_coll = common_coll;
	      arg3_cs = common_cs;
	      arg3_coerc_level = PT_COLLATION_FULLY_COERC;
	    }
	}

      if (arg1_coll == arg2_coll && arg2_coll == arg3_coll)
	{
	  assert (arg1_cs == arg2_cs && arg2_cs == arg3_cs);
	  goto coerce_result;
	}
    }
  else
    {
      if (arg1_coll == arg2_coll)
	{
	  assert (arg1_cs == arg2_cs);
	  goto coerce_result;
	}
    }

  assert (arg1_coll != arg2_coll || arg1_coll != arg3_coll);

  if (pt_common_collation (arg1_coll, arg1_cs, arg1_coerc_level,
			   arg2_coll, arg2_cs, arg2_coerc_level,
			   arg3_coll, arg3_cs, arg3_coerc_level,
			   args_w_coll, op_has_3_args, &common_coll,
			   &common_cs) != 0)
    {
      goto error;
    }

coerce_arg:
  /* step 3 : coerce collation of expression arguments */
  if ((common_coll != arg1_coll || common_cs != arg1_cs)
      && (PT_HAS_COLLATION (arg1_type) || arg1_type == PT_TYPE_MAYBE
	  || PT_IS_COLLECTION_TYPE (arg1_type)))
    {
      new_node = pt_coerce_node_collation (parser, arg1, common_coll,
					   common_cs, false,
					   use_cast_collate_modifier);

      if (new_node == NULL)
	{
	  goto error;
	}

      expr->info.expr.arg1 = new_node;
    }

  if ((common_coll != arg2_coll || common_cs != arg2_cs)
      && (PT_HAS_COLLATION (arg2_type) || arg2_type == PT_TYPE_MAYBE
	  || PT_IS_COLLECTION_TYPE (arg2_type)))
    {
      new_node = pt_coerce_node_collation (parser, arg2, common_coll,
					   common_cs, false,
					   use_cast_collate_modifier);

      if (new_node == NULL)
	{
	  goto error;
	}

      if (reverse_arg2_arg3)
	{
	  expr->info.expr.arg3 = new_node;
	}
      else
	{
	  expr->info.expr.arg2 = new_node;
	}
    }

  if (op_has_3_args && (common_coll != arg3_coll || common_cs != arg3_cs)
      && (PT_HAS_COLLATION (arg3_type) || arg3_type == PT_TYPE_MAYBE))
    {
      new_node = pt_coerce_node_collation (parser, arg3, common_coll,
					   common_cs, false,
					   use_cast_collate_modifier);

      if (new_node == NULL)
	{
	  goto error;
	}

      expr->info.expr.arg3 = new_node;
    }

  /* step 4: update collation of expression result */
coerce_result:
  if (op == PT_CHR || op == PT_CLOB_TO_CHAR)
    {
      /* for these operators, we don't we the arguments' collaitons to infere
       * common collation, but special values for arg2 */
      common_cs = expr->data_type->info.data_type.units;
      common_coll = expr->data_type->info.data_type.collation_id;
    }

  if (expr_coll_modifier != -1 && expr->data_type != NULL)
    {
      if (expr_cs_modifier != common_cs)
	{
	  PT_ERRORmf2 (parser, expr, MSGCAT_SET_PARSER_SEMANTIC,
		       MSGCAT_SEMANTIC_CS_MATCH_COLLATE,
		       lang_get_codeset_name (common_cs),
		       lang_get_codeset_name (expr_cs_modifier));
	  goto error_exit;
	}

      common_coll = expr_coll_modifier;
      common_cs = expr_cs_modifier;
    }

  switch (op)
    {
    case PT_COALESCE:
    case PT_NVL:
    case PT_NVL2:
    case PT_IFNULL:
    case PT_GREATEST:
    case PT_LEAST:
    case PT_NULLIF:
      if (expr->type_enum == PT_TYPE_MAYBE
	  && !LANG_IS_COERCIBLE_COLL (common_coll))
	{
	  PT_NODE *dt;
	  PT_TYPE_ENUM expr_type;

	  assert (args_w_coll > 0);
	  if (op == PT_NVL2)
	    {
	      if (arg2_type == PT_TYPE_MAYBE)
		{
		  expr_type = arg3_type;
		}
	      else
		{
		  expr_type = arg2_type;
		}
	    }
	  else
	    {
	      if (arg1_type == PT_TYPE_MAYBE)
		{
		  expr_type = arg2_type;
		}
	      else
		{
		  expr_type = arg1_type;
		}
	    }

	  dt = parser_new_node (parser, PT_DATA_TYPE);
	  if (dt == NULL)
	    {
	      goto error;
	    }

	  assert (PT_HAS_COLLATION (expr_type));
	  dt->type_enum = expr_type;
	  dt->info.data_type.precision = TP_FLOATING_PRECISION_VALUE;
	  dt->info.data_type.collation_id = common_coll;
	  dt->info.data_type.units = common_cs;

	  new_node = pt_wrap_with_cast_op (parser, expr, expr->type_enum,
					   dt->info.data_type.precision,
					   dt->info.data_type.dec_precision,
					   dt);
	  /* 'dt' is copied in 'pt_wrap_with_cast_op' */
	  parser_free_node (parser, dt);
	  if (new_node == NULL)
	    {
	      goto error;
	    }

	  expr = new_node;

	  break;
	}
      /* fall through */
    case PT_PLUS:
      if (expr->type_enum == PT_TYPE_MAYBE)
	{
	  break;
	}
      /* fall through */
    case PT_CONCAT:
    case PT_CONCAT_WS:
    case PT_RPAD:
    case PT_LPAD:
    case PT_SUBSTRING:
    case PT_MID:
    case PT_REPLACE:
    case PT_TRANSLATE:
    case PT_STRCAT:
    case PT_DATE_FORMAT:
    case PT_TIME_FORMAT:
    case PT_LOWER:
    case PT_UPPER:
    case PT_REPEAT:
    case PT_RTRIM:
    case PT_LTRIM:
    case PT_TRIM:
    case PT_LEFT:
    case PT_RIGHT:
    case PT_SUBSTRING_INDEX:
    case PT_DATEF:
    case PT_TIMEF:
    case PT_IF:
      new_node = pt_coerce_node_collation (parser, expr, common_coll,
					   common_cs, true, false);
      if (new_node == NULL)
	{
	  goto error;
	}

      expr = new_node;
      break;
    default:
      break;
    }

  *node = expr;

  return NO_ERROR;

error:
  PT_ERRORmf (parser, expr, MSGCAT_SET_PARSER_SEMANTIC,
	      MSGCAT_SEMANTIC_COLLATION_OP_ERROR, pt_show_binopcode (op));

error_exit:
  return ER_FAILED;
}

/*
 * pt_check_recursive_expr_collation () - checks the collation of a recursive
 *					  expression node
 *
 *   return: error code
 *   parser(in): parser context
 *   node(in): a parse tree expression node
 *
 */
static int
pt_check_recursive_expr_collation (PARSER_CONTEXT * parser, PT_NODE ** node)
{
  PT_NODE *expr = *node;
  PT_OP_TYPE op;
  int recurs_coll = -1;
  INTL_CODESET recurs_cs = INTL_CODESET_NONE;
  PT_COLL_COERC_LEV recurs_coerc_level = PT_COLLATION_FULLY_COERC;
  bool has_utf8_cs = false;
  bool has_euc_cs = false;
  bool need_arg_coerc = false;

  assert (expr != NULL);

  op = expr->info.expr.op;
  assert (op == PT_DECODE || op == PT_CASE);

  while (PT_IS_RECURSIVE_EXPRESSION (expr) && op == expr->info.expr.op)
    {
      PT_NODE *arg1 = expr->info.expr.arg1;
      PT_NODE *arg2 = expr->info.expr.arg2;
      int arg1_coll = -1;
      int arg2_coll = -1;
      INTL_CODESET arg1_cs, arg2_cs;
      PT_COLL_COERC_LEV arg1_coerc_level, arg2_coerc_level;

      if (pt_get_collation_info (arg1, &arg1_coll, &arg1_cs,
				 &arg1_coerc_level))
	{
	  if (arg1_cs == INTL_CODESET_KSC5601_EUC)
	    {
	      has_euc_cs = true;
	    }
	  else if (arg1_cs == INTL_CODESET_UTF8)
	    {
	      has_utf8_cs = true;
	    }

	  if (has_utf8_cs && has_euc_cs)
	    {
	      goto error;
	    }

	  if (recurs_coll != -1 && recurs_coll != arg1_coll
	      && recurs_coerc_level == arg1_coerc_level)
	    {
	      goto error;
	    }
	  else
	    {
	      if (recurs_coll != -1)
		{
		  need_arg_coerc = true;
		}

	      if (recurs_coerc_level > arg1_coerc_level || recurs_coll == -1)
		{
		  recurs_coerc_level = arg1_coerc_level;
		  recurs_coll = arg1_coll;
		  recurs_cs = arg1_cs;
		}
	    }
	}

      if (arg2 != NULL
	  && (arg2->node_type != PT_EXPR || op != arg2->info.expr.op)
	  && pt_get_collation_info (arg2, &arg2_coll, &arg2_cs,
				    &arg2_coerc_level))
	{
	  if (arg2_cs == INTL_CODESET_KSC5601_EUC)
	    {
	      has_euc_cs = true;
	    }
	  else if (arg2_cs == INTL_CODESET_UTF8)
	    {
	      has_utf8_cs = true;
	    }

	  if (has_utf8_cs && has_euc_cs)
	    {
	      goto error;
	    }

	  if (recurs_coll != -1 && recurs_coll != arg2_coll
	      && recurs_coerc_level == arg2_coerc_level)
	    {
	      goto error;
	    }
	  else
	    {
	      if (recurs_coll != -1)
		{
		  need_arg_coerc = true;
		}

	      if (recurs_coerc_level > arg2_coerc_level || recurs_coll == -1)
		{
		  recurs_coerc_level = arg2_coerc_level;
		  recurs_coll = arg2_coll;
		  recurs_cs = arg2_cs;
		}
	    }
	}

      assert (PT_IS_RIGHT_RECURSIVE_EXPRESSION (expr));
      expr = arg2;
    }

  expr = *node;
  while (need_arg_coerc && PT_IS_RECURSIVE_EXPRESSION (expr)
	 && op == expr->info.expr.op)
    {
      PT_NODE *arg1 = expr->info.expr.arg1;
      PT_NODE *arg2 = expr->info.expr.arg2;
      int arg1_coll = -1;
      int arg2_coll = -1;
      INTL_CODESET arg1_cs, arg2_cs;
      PT_COLL_COERC_LEV arg1_coerc_level, arg2_coerc_level;

      if (PT_HAS_COLLATION (arg1->type_enum))
	{
	  (void) pt_get_collation_info (arg1, &arg1_coll, &arg1_cs,
					&arg1_coerc_level);
	}

      if ((PT_HAS_COLLATION (arg1->type_enum) && arg1_coll != recurs_coll)
	  || arg1->type_enum == PT_TYPE_MAYBE)
	{
	  arg1 = pt_coerce_node_collation (parser, arg1, recurs_coll,
					   recurs_cs, false, false);
	  if (arg1 == NULL)
	    {
	      goto error;
	    }
	  expr->info.expr.arg1 = arg1;
	}

      if (arg2 != NULL)
	{
	  if (arg2->node_type != PT_EXPR || op != arg2->info.expr.op)
	    {
	      if (PT_HAS_COLLATION (arg2->type_enum))
		{
		  (void) pt_get_collation_info (arg2, &arg2_coll, &arg2_cs,
						&arg2_coerc_level);
		}

	      if ((PT_HAS_COLLATION (arg2->type_enum)
		   && arg2_coll != recurs_coll)
		  || arg2->type_enum == PT_TYPE_MAYBE)
		{
		  arg2 = pt_coerce_node_collation (parser, arg2, recurs_coll,
						   recurs_cs, false, false);
		  if (arg2 == NULL)
		    {
		      goto error;
		    }
		  expr->info.expr.arg2 = arg2;
		}
	    }
	  else if (arg2->node_type == PT_EXPR && op == arg2->info.expr.op
		   && PT_HAS_COLLATION (arg2->type_enum))
	    {
	      /* force collation on recursive expression node */
	      arg2 = pt_coerce_node_collation (parser, arg2, recurs_coll,
					       recurs_cs, true, false);
	      if (arg2 == NULL)
		{
		  goto error;
		}
	      expr->info.expr.arg2 = arg2;
	    }
	}

      assert (PT_IS_RIGHT_RECURSIVE_EXPRESSION (expr));
      expr = arg2;
    }

  expr = *node;
  if (recurs_coll != -1 && PT_HAS_COLLATION (expr->type_enum))
    {
      *node = pt_coerce_node_collation (parser, expr, recurs_coll, recurs_cs,
					true, false);

      if (*node == NULL)
	{
	  goto error;
	}
    }

  return NO_ERROR;

error:
  PT_ERRORmf (parser, expr, MSGCAT_SET_PARSER_SEMANTIC,
	      MSGCAT_SEMANTIC_COLLATION_OP_ERROR, pt_show_binopcode (op));

  return ER_FAILED;
}

/*
* pt_node_to_enumeration_expr () - wrap node with PT_TO_ENUMERATION_VALUE
*				    expression
* return : new node or null
* parser (in) :
* data_type (in) :
* node (in) :
*/
static PT_NODE *
pt_node_to_enumeration_expr (PARSER_CONTEXT * parser, PT_NODE * data_type,
			     PT_NODE * node)
{
  PT_NODE *expr = NULL;
  if (parser == NULL || data_type == NULL || node == NULL)
    {
      assert (false);
      return NULL;
    }

  if (PT_HAS_COLLATION (node->type_enum) && node->data_type != NULL)
    {
      if (!INTL_CAN_COERCE_CS (node->data_type->info.data_type.units,
			       data_type->info.data_type.units))
	{
	  PT_ERRORmf2 (parser, node, MSGCAT_SET_PARSER_SEMANTIC,
		       MSGCAT_SEMANTIC_COERCE_UNSUPPORTED,
		       pt_short_print (parser, node),
		       pt_show_type_enum (PT_TYPE_ENUMERATION));
	  return node;
	}
    }

  expr = parser_new_node (parser, PT_EXPR);
  if (expr == NULL)
    {
      PT_ERRORm (parser, node, MSGCAT_SET_PARSER_SEMANTIC,
		 MSGCAT_SEMANTIC_OUT_OF_MEMORY);
      return NULL;
    }

  expr->info.expr.arg1 = node;
  expr->type_enum = PT_TYPE_ENUMERATION;
  expr->data_type = parser_copy_tree (parser, data_type);
  expr->info.expr.op = PT_TO_ENUMERATION_VALUE;
  return expr;
}

/*
* pt_select_list_to_enumeration_expr () - wrap select list with
*					   PT_TO_ENUMERATION_VALUE expression
* return : new node or null
* parser (in) :
* data_type (in) :
* node (in) :
*/
static PT_NODE *
pt_select_list_to_enumeration_expr (PARSER_CONTEXT * parser,
				    PT_NODE * data_type, PT_NODE * node)
{
  int i = 0;
  PT_NODE *new_node = NULL;

  if (node == NULL || data_type == NULL)
    {
      return node;
    }

  if (!PT_IS_QUERY_NODE_TYPE (node->node_type))
    {
      return node;
    }
  switch (node->node_type)
    {
    case PT_SELECT:
      {
	PT_NODE *item = NULL;
	PT_NODE *prev = NULL;
	PT_NODE *select_list = pt_get_select_list (parser, node);
	for (item = select_list; item != NULL; prev = item, item = item->next)
	  {
	    if (item->type_enum == PT_TYPE_ENUMERATION)
	      {
		/* nothing to do here */
		continue;
	      }
	    new_node = pt_node_to_enumeration_expr (parser, data_type, item);
	    if (new_node == NULL)
	      {
		return NULL;
	      }
	    item = new_node;
	    /* first node in the list */
	    if (prev == NULL)
	      {
		node->info.query.q.select.list = item;
	      }
	    else
	      {
		prev->next = item;
	      }
	  }
	break;
      }
    case PT_DIFFERENCE:
    case PT_INTERSECTION:
    case PT_UNION:
      new_node =
	pt_select_list_to_enumeration_expr (parser, data_type,
					    node->info.query.q.union_.arg1);
      if (new_node == NULL)
	{
	  return NULL;
	}
      node->info.query.q.union_.arg1 = new_node;
      new_node =
	pt_select_list_to_enumeration_expr (parser, data_type,
					    node->info.query.q.union_.arg2);
      if (new_node == NULL)
	{
	  return NULL;
	}
      node->info.query.q.union_.arg2 = new_node;
      break;
    default:
      break;
    }
  return node;
}

/*
* pt_is_enumeration_special_comparison () - check if the comparison is a
*     '=' comparison that involves ENUM types and constants or if it's a IN
*     'IN' comparison in which the left operator is an ENUM.
* return : true if it is a special ENUM comparison or false otherwise.
* arg1 (in) : left argument
* op (in)   : expression operator
* arg2 (in) : right argument
*/
static bool
pt_is_enumeration_special_comparison (PT_NODE * arg1, PT_OP_TYPE op,
				      PT_NODE * arg2)
{
  PT_NODE *arg_tmp = NULL;

  if (arg1 == NULL || arg2 == NULL)
    {
      return false;
    }

  switch (op)
    {
    case PT_EQ:
    case PT_NULLSAFE_EQ:
      if (arg1->type_enum != PT_TYPE_ENUMERATION)
	{
	  if (arg2->type_enum != PT_TYPE_ENUMERATION)
	    {
	      return false;
	    }

	  arg_tmp = arg1;
	  arg1 = arg2;
	  arg2 = arg_tmp;
	}
      else if (arg2->type_enum == PT_TYPE_ENUMERATION
	       && arg1->data_type != NULL && arg2->data_type != NULL)
	{
	  PT_NODE *e1 = arg1->data_type->info.data_type.enumeration;
	  PT_NODE *e2 = arg2->data_type->info.data_type.enumeration;
	  int l1, l2;
	  PARSER_VARCHAR *pvc1, *pvc2;

	  for (; e1 != NULL && e2 != NULL; e1 = e1->next, e2 = e2->next)
	    {
	      pvc1 = e1->info.value.data_value.str;
	      pvc2 = e2->info.value.data_value.str;
	      l1 = pt_get_varchar_length (pvc1);
	      l2 = pt_get_varchar_length (pvc2);
	      if (l1 != l2
		  || memcmp (pt_get_varchar_bytes (pvc1),
			     pt_get_varchar_bytes (pvc2), l1))
		{
		  break;
		}
	    }
	  if (e1 == NULL && e2 == NULL)
	    {
	      return true;
	    }
	}
      if (arg2->node_type == PT_EXPR)
	{
	  if (arg2->info.expr.op != PT_TO_ENUMERATION_VALUE)
	    {
	      return false;
	    }
	}
      else
	{
	  if (!PT_IS_CONST (arg2))
	    {
	      return false;
	    }
	}
      return true;
    case PT_IS_IN:
    case PT_EQ_SOME:
      return (arg1->type_enum == PT_TYPE_ENUMERATION);
    default:
      return false;
    }
}

/*
* pt_fix_enumeration_comparison () - fix comparisons for enumeration type
* return : modified node or NULL
* parser (in) :
* expr (in) :
*/
static PT_NODE *
pt_fix_enumeration_comparison (PARSER_CONTEXT * parser, PT_NODE * expr)
{
  PT_NODE *arg1 = NULL, *arg2 = NULL;
  PT_NODE *node = NULL, *save_next = NULL;
  PT_NODE *list = NULL, *list_prev = NULL, **list_start = NULL;
  PT_OP_TYPE op;
  if (expr == NULL || expr->node_type != PT_EXPR)
    {
      return expr;
    }
  op = expr->info.expr.op;

  switch (op)
    {
    case PT_EQ:
    case PT_NULLSAFE_EQ:
      arg1 = expr->info.expr.arg1;
      arg2 = expr->info.expr.arg2;

      if ((arg1->type_enum != PT_TYPE_ENUMERATION
	   && arg2->type_enum != PT_TYPE_ENUMERATION)
	  || (!PT_IS_CONST (arg1) && !PT_IS_CONST (arg2)))
	{
	  /* we're only handling enumeration comp const */
	  return expr;
	}
      if (PT_IS_CONST (arg1))
	{
	  if (PT_IS_CONST (arg2))
	    {
	      /* const op const does not need special handling */
	      return expr;
	    }
	  /* switch arg1 with arg2 so that we have non cost operand on the
	   * left side
	   */
	  node = expr->info.expr.arg1;
	  expr->info.expr.arg1 = expr->info.expr.arg2;
	  expr->info.expr.arg2 = node;
	}
      break;
    case PT_IS_IN:
    case PT_EQ_SOME:
      break;
    default:
      return expr;
    }

  arg1 = expr->info.expr.arg1;
  arg2 = expr->info.expr.arg2;
  op = expr->info.expr.op;

  if (arg1->data_type == NULL
      || arg1->data_type->info.data_type.enumeration == NULL)
    {
      /* we don't know the actual enumeration type */
      return expr;
    }

  switch (op)
    {
    case PT_EQ:
    case PT_NULLSAFE_EQ:
      node = pt_node_to_enumeration_expr (parser, arg1->data_type, arg2);
      if (node == NULL)
	{
	  return NULL;
	}
      expr->info.expr.arg2 = node;
      break;
    case PT_IS_IN:
    case PT_EQ_SOME:
      if (PT_IS_QUERY_NODE_TYPE (arg2->node_type))
	{
	  node =
	    pt_select_list_to_enumeration_expr (parser, arg1->data_type,
						arg2);
	  if (node == NULL)
	    {
	      return NULL;
	    }
	  expr->info.expr.arg2 = node;
	  return expr;
	}
      /* not a subquery */
      switch (arg2->node_type)
	{
	case PT_VALUE:
	  /* convert this value to a multiset */
	  node = parser_new_node (parser, PT_FUNCTION);
	  if (node == NULL)
	    {
	      PT_ERRORm (parser, expr, MSGCAT_SET_PARSER_SEMANTIC,
			 MSGCAT_SEMANTIC_OUT_OF_MEMORY);
	      return NULL;
	    }
	  node->info.function.function_type = F_SET;
	  node->info.function.arg_list = arg2->info.value.data_value.set;
	  node->type_enum = arg2->type_enum;

	  arg2->info.value.data_value.set = NULL;
	  parser_free_tree (parser, arg2);
	  expr->info.expr.arg2 = arg2 = node;

	  /* fall through */

	case PT_FUNCTION:
	  list = arg2->info.function.arg_list;
	  list_start = &arg2->info.function.arg_list;
	  break;

	default:
	  return expr;
	}

      while (list != NULL)
	{
	  save_next = list->next;
	  list->next = NULL;
	  node = pt_node_to_enumeration_expr (parser, arg1->data_type, list);
	  if (node == NULL)
	    {
	      return NULL;
	    }

	  node->next = save_next;
	  if (list_prev == NULL)
	    {
	      *list_start = node;
	    }
	  else
	    {
	      list_prev->next = node;
	    }
	  list_prev = node;
	  list = node->next;
	}
      if (arg2->data_type != NULL)
	{
	  parser_free_tree (parser, arg2->data_type);
	  arg2->data_type = NULL;
	}
      (void) pt_add_type_to_set (parser,
				 arg2->info.function.arg_list,
				 &arg2->data_type);
      break;

    default:
      break;
    }

  return expr;
}
