//
//  Copyright (C) 2011-2014  Nick Gasson
//
//  This program is free software: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation, either version 3 of the License, or
//  (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

%{
#include "util.h"
#include "ident.h"
#include "tree.h"

#include <sys/types.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <assert.h>
#include <stdbool.h>

#define YYLTYPE loc
typedef struct loc loc;

#define YYLLOC_DEFAULT(Current, Rhs, N) {                            \
   if (N) {                                                          \
      (Current).first_line   = YYRHSLOC(Rhs, 1).first_line;          \
      (Current).first_column = YYRHSLOC(Rhs, 1).first_column;        \
      (Current).last_line    = YYRHSLOC(Rhs, N).last_line;           \
      (Current).last_column  = YYRHSLOC(Rhs, N).last_column;         \
      (Current).file         = YYRHSLOC(Rhs, N).file;                \
      (Current).linebuf      = YYRHSLOC(Rhs, 1).linebuf;             \
   }                                                                 \
   else {                                                            \
      (Current).first_line   = (Current).last_line   =               \
         YYRHSLOC(Rhs, 0).last_line;                                 \
      (Current).first_column = (Current).last_column =               \
         YYRHSLOC(Rhs, 0).last_column;                               \
      (Current).file = YYRHSLOC(Rhs, 0).file;                        \
      (Current).linebuf = YYRHSLOC(Rhs, 0).linebuf;                  \
   }                                                                 \
 }

union listval {
   ident_t   ident;
   range_t   range;
};

#define LISTVAL(x) ((union listval)x)

typedef struct list list_t;
typedef struct tree_list tree_list_t;

struct list {
   list_t       *next;
   union listval item;
};

struct tree_list {
   tree_list_t *next;
   tree_list_t *tail;
   tree_t       value;
};

static int        n_errors = 0;
static const char *read_ptr;
static const char *file_start;
static size_t     file_sz;
static const char *perm_linebuf = NULL;
static const char *perm_file_name = NULL;
static int        n_token_next_start = 0;
static int        n_row = 0;
static bool       last_was_newline = true;
static tree_t     root;
static tree_t     assert_viol = NULL;

int yylex(void);
static void yyerror(const char *s);

static void copy_trees(tree_list_t *from,
                       void (*copy_fn)(tree_t t, tree_t d),
                       tree_t to);
static list_t *list_add(list_t *list, union listval item);
static void list_free(list_t *list);
static void tree_list_append(tree_list_t **l, tree_t t);
static void tree_list_prepend(tree_list_t **l, tree_t t);
static void tree_list_concat(tree_list_t **a, tree_list_t *b);
static void tree_list_free(tree_list_t *l);
static tree_t build_expr1(const char *fn, tree_t left,
                          const struct YYLTYPE *loc);
static tree_t build_expr2(const char *fn, tree_t left, tree_t right,
                          const struct YYLTYPE *loc);
static tree_t str_to_agg(const char *start, const char *end,
                         const loc_t *loc);
static tree_t bit_str_to_agg(const char *str, const loc_t *loc);
static bool to_range_expr(tree_t t, range_t *r, bool force);
static ident_t loc_to_ident(const loc_t *loc);
static tree_t get_time(int64_t fs);
static void set_delay_mechanism(tree_t t, tree_t reject);
static tree_t int_to_physical(tree_t t, tree_t unit);
static tree_t handle_param_expr(tree_t name, tree_list_t *params);
static void add_file_decls(tree_list_t **out, list_t *in, type_t type,
                           tree_t mode, tree_t value, loc_t *loc);

#define parse_error(loc, ...) do {            \
      error_at(loc, __VA_ARGS__);             \
      n_errors++;                             \
   } while (0)
%}

%union {
   tree_t       t;
   ident_t      i;
   tree_list_t *l;
   struct {
      tree_list_t *left;
      tree_list_t *right;
   } p;
   port_mode_t  m;
   type_t       y;
   range_t      r;
   list_t      *g;
   class_t      c;
   bool         b;
   char        *s;
   int64_t      n;
   double       d;
}

%type <t> entity_decl opt_static_expr expr abstract_literal literal
%type <t> numeric_literal library_unit arch_body process_stmt conc_stmt
%type <t> seq_stmt timeout_clause physical_literal target severity
%type <t> package_decl name aggregate string_literal report binding
%type <t> waveform_element seq_stmt_without_label conc_assign_stmt
%type <t> comp_instance_stmt conc_stmt_without_label elsif_list
%type <t> delay_mechanism bit_string_literal block_stmt expr_or_open
%type <t> conc_select_assign_stmt generate_stmt condition_clause
%type <t> null_literal conc_procedure_call_stmt conc_assertion_stmt
%type <t> base_unit_decl choice use_clause_item entity_aspect
%type <t> entity_stmt entity_stmt_without_label
%type <i> id opt_id selected_id func_name func_type operator_name
%type <l> interface_object_decl interface_list shared_variable_decl
%type <l> port_clause generic_clause interface_decl signal_decl
%type <l> block_decl_item block_decl_part conc_stmt_list process_decl_part
%type <l> variable_decl process_decl_item seq_stmt_list type_decl
%type <l> subtype_decl package_decl_part package_decl_item enum_lit_list
%type <l> constant_decl formal_param_list subprogram_decl name_list
%type <l> sensitivity_clause process_sensitivity_clause attr_decl
%type <l> package_body_decl_item package_body_decl_part subprogram_decl_part
%type <l> subprogram_decl_item waveform alias_decl attr_spec elem_decl
%type <l> conditional_waveforms component_decl file_decl elem_decl_list
%type <l> secondary_unit_decls param_list generic_map port_map entity_decl_part
%type <l> entity_decl_item selected_waveforms choice_list case_alt_list
%type <l> entity_stmt_part
%type <l> element_assoc element_assoc_list context_item context_clause
%type <l> use_clause_item_list use_clause config_spec library_clause
%type <p> entity_header generate_body
%type <m> opt_mode
%type <y> subtype_indication type_mark type_def scalar_type_def array_type_def
%type <y> physical_type_def enum_type_def subtype_indication_constraint
%type <y> index_subtype_def access_type_def file_type_def
%type <y> unconstrained_array_def constrained_array_def
%type <y> record_type_def
%type <r> range range_constraint constraint_elem
%type <g> index_constraint constraint constraint_list id_list
%type <c> object_class entity_class
%type <b> opt_postponed

%token <s> tID "$yellow$identifier$$"
%token tENTITY "$yellow$entity$$"
%token tIS "$yellow$is$$"
%token tEND "$yellow$end$$"
%token tGENERIC "$yellow$generic$$"
%token tPORT "$yellow$port$$"
%token tCONSTANT "$yellow$constant$$"
%token tCOMPONENT "$yellow$component$$"
%token tCONFIGURATION "$yellow$configuration$$"
%token tARCHITECTURE "$yellow$architecture$$"
%token tOF "$yellow$of$$"
%token tBEGIN "$yellow$begin$$"
%token tFOR "$yellow$for$$"
%token tTYPE "$yellow$type$$"
%token tTO "$yellow$to$$"
%token tALL "$yellow$all$$"
%token tIN "$yellow$in$$"
%token tOUT "$yellow$out$$"
%token tBUFFER "$yellow$buffer$$"
%token tBUS "$yellow$bus$$"
%token tUNAFFECTED "$yellow$unaffected$$"
%token tSIGNAL "$yellow$signal$$"
%token tDOWNTO "$yellow$downto$$"
%token tPROCESS "$yellow$process$$"
%token tPOSTPONED "$yellow$postponed$$"
%token tWAIT "$yellow$wait$$"
%token tREPORT "$yellow$report$$"
%token tLPAREN "("
%token tRPAREN ")"
%token tSEMI ";"
%token tASSIGN ":="
%token tCOLON ":"
%token tCOMMA ","
%token <n> tINT "$yellow$integer literal$$"
%token <s> tSTRING "$yellow$string literal$$"
%token tERROR "$yellow$error token$$"
%token tINOUT "$yellow$inout$$"
%token tLINKAGE "$yellow$linkage$$"
%token tVARIABLE "$yellow$variable$$"
%token tIF "$yellow$if$$"
%token tRANGE "$yellow$range$$"
%token tSUBTYPE "$yellow$subtype$$"
%token tUNITS "$yellow$units$$"
%token tPACKAGE "$yellow$package$$"
%token tLIBRARY "$yellow$library$$"
%token tUSE "$yellow$use$$"
%token tDOT "."
%token tNULL "$yellow$null$$"
%token tTICK "'"
%token tFUNCTION "$yellow$function$$"
%token tIMPURE "$yellow$impure$$"
%token tRETURN "$yellow$return$$"
%token tPURE "$yellow$pure$$"
%token tARRAY "$yellow$array$$"
%token tBOX "<>"
%token tASSOC "=>"
%token tOTHERS "$yellow$others$$"
%token tASSERT "$yellow$assert$$"
%token tSEVERITY "$yellow$severity$$"
%token tON "$yellow$on$$"
%token tMAP "$yellow$map$$"
%token tTHEN "$yellow$then$$"
%token tELSE "$yellow$else$$"
%token tELSIF "$yellow$elsif$$"
%token tBODY "$yellow$body$$"
%token tWHILE "$yellow$while$$"
%token tLOOP "$yellow$loop$$"
%token tAFTER "$yellow$after$$"
%token tALIAS "$yellow$alias$$"
%token tATTRIBUTE "$yellow$attribute$$"
%token tPROCEDURE "$yellow$procedure$$"
%token tEXIT "$yellow$exit$$"
%token tNEXT "$yellow$next$$"
%token tWHEN "$yellow$when$$"
%token tCASE "$yellow$case$$"
%token tLABEL "$yellow$label$$"
%token tGROUP "$yellow$group$$"
%token tLITERAL "$yellow$literal$$"
%token tBAR "|"
%token tLSQUARE "["
%token tRSQUARE "]"
%token tINERTIAL "$yellow$inertial$$"
%token tTRANSPORT "$yellow$transport$$"
%token tREJECT "$yellow$reject$$"
%token <s> tBITSTRING "$yellow$bit string$$"
%token tBLOCK "$yellow$block$$"
%token tWITH "$yellow$with$$"
%token tSELECT "$yellow$select$$"
%token tGENERATE "$yellow$generate$$"
%token tACCESS "$yellow$access$$"
%token tFILE "$yellow$file$$"
%token tOPEN "$yellow$open$$"
%token <d> tREAL "$yellow$real literal$$"
%token tUNTIL "$yellow$until$$"
%token tRECORD "$yellow$record$$"
%token tNEW "$yellow$new$$"
%token tSHARED "$yellow$shared$$"
%token tEOF 0 "end of file"
%token tAND "$yellow$and$$"
%token tOR "$yellow$or$$"
%token tNAND "$yellow$nand$$"
%token tNOR "$yellow$nir$$"
%token tXOR "$yellow$xor$$"
%token tXNOR "$yellow$xnor$$"
%token tEQ "="
%token tNEQ "/="
%token tLT "<"
%token tLE "<="
%token tGT ">"
%token tGE ">="
%token tPLUS "+"
%token tMINUS "-"
%token tAMP "&"
%token tPOWER "**"
%token tOVER "/"
%token tSLL "$yellow$sll$$"
%token tSRL "$yellow$srl$$"
%token tSLA "$yellow$sla$$"
%token tSRA "$yellow$sra$$"
%token tROL "$yellow$rol$$"
%token tROR "$yellow$ror$$"
%token tMOD "$yellow$mod$$"
%token tREM "$yellow$rem$$"
%token tABS "$yellow$abs$$"
%token tNOT "$yellow$not$$"

%left tAND tOR tNAND tNOR tXOR tXNOR
%left tEQ tNEQ tLT tLE tGT tGE
%left tSLL tSRL tSLA tSRA tROL tROR
%left tPLUS tMINUS tAMP
%left tTIMES tOVER tMOD tREM
%left tPOWER
%nonassoc tABS tNOT tNEW

%error-verbose
%expect 0

%%

design_unit
: context_clause library_unit
  {
     copy_trees($1, tree_add_context, $2);

     root = $2;
     YYACCEPT;
  }
| /* empty */ {}
;

context_clause
: context_clause context_item
  {
     $$ = $1;
     tree_list_concat(&$$, $2);
  }
| /* empty */ { $$ = NULL; }
;

context_item : library_clause | use_clause ;

library_clause
: tLIBRARY id_list tSEMI
  {
     $$ = NULL;

     for (list_t *it = $2; it != NULL; it = it->next) {
        tree_t t = tree_new(T_LIBRARY);
        tree_set_ident(t, it->item.ident);
        tree_set_loc(t, &@$);

        tree_list_prepend(&$$, t);
     }

     list_free($2);
  }
;

use_clause
: tUSE use_clause_item_list tSEMI
  {
     $$ = $2;
  }
;

use_clause_item_list
: use_clause_item
  {
     $$ = NULL;
     tree_list_prepend(&$$, $1);
  }
| use_clause_item tCOMMA use_clause_item_list
  {
     $$ = $3;
     tree_list_prepend(&$$, $1);
  }
;

use_clause_item
: id tDOT id
  {
     $$ = tree_new(T_USE);
     tree_set_loc($$, &@$);
     tree_set_ident($$, ident_prefix($1, $3, '.'));
  }
| id tDOT id tDOT id
  {
     $$ = tree_new(T_USE);
     tree_set_loc($$, &@$);
     tree_set_ident($$, ident_prefix($1, $3, '.'));
     tree_set_ident2($$, $5);
  }
| id tDOT id tDOT tSTRING
  {
     $$ = tree_new(T_USE);
     tree_set_loc($$, &@$);
     tree_set_ident($$, ident_prefix($1, $3, '.'));
     tree_set_ident2($$, ident_new($5));
     free($5);
  }
| id tDOT tALL
  {
     $$ = tree_new(T_USE);
     tree_set_loc($$, &@$);
     tree_set_ident($$, $1);
     tree_set_ident2($$, ident_new("all"));
  }
| id tDOT id tDOT tALL
  {
     $$ = tree_new(T_USE);
     tree_set_loc($$, &@$);
     tree_set_ident($$, ident_prefix($1, $3, '.'));
     tree_set_ident2($$, ident_new("all"));
  }
;

library_unit : entity_decl | arch_body | package_decl ;

id
: tID
  {
     $$ = ident_new($1);
     free($1);
  }
;

selected_id
: id tDOT selected_id
  {
     $$ = ident_prefix($1, $3, '.');
  }
| id
;

id_list
: id
  {
     $$ = list_add(NULL, LISTVAL($1));
  }
| id tCOMMA id_list
  {
     $$ = list_add($3, LISTVAL($1));
  }
;

opt_id : id { $$ = $1; } | { $$ = NULL; } ;

entity_decl
: tENTITY id tIS entity_header entity_decl_part
  tEND opt_entity_token opt_id tSEMI
  {
     $$ = tree_new(T_ENTITY);
     tree_set_ident($$, $2);
     tree_set_loc($$, &@$);
     copy_trees($4.left, tree_add_generic, $$);
     copy_trees($4.right, tree_add_port, $$);
     copy_trees($5, tree_add_decl, $$);

     if ($8 != NULL && $8 != $2) {
        parse_error(&@8, "%s does not match entity name %s",
                    istr($8), istr($2));
     }
  }
| tENTITY id tIS entity_header entity_decl_part
  tBEGIN entity_stmt_part
  tEND opt_entity_token opt_id tSEMI
  {
     $$ = tree_new(T_ENTITY);
     tree_set_ident($$, $2);
     tree_set_loc($$, &@$);
     copy_trees($4.left, tree_add_generic, $$);
     copy_trees($4.right, tree_add_port, $$);
     copy_trees($5, tree_add_decl, $$);
     copy_trees($7, tree_add_stmt, $$);

     if ($10 != NULL && $10 != $2) {
        parse_error(&@10, "%s does not match entity name %s",
                    istr($10), istr($2));
     }
  }
;

entity_decl_part
: entity_decl_part entity_decl_item
 {
    $$ = $1;
    tree_list_concat(&$$, $2);
 }
| /* empty */ { $$ = NULL; }
;

entity_decl_item
: subprogram_decl
| type_decl
| subtype_decl
| constant_decl
| signal_decl
| shared_variable_decl
| file_decl
| alias_decl
| attr_decl
| attr_spec
  /*| disconnection_specification
    | group_template_declaration
    | group_declaration */
;

opt_entity_token : tENTITY | /* empty */ ;

entity_header
: generic_clause port_clause
  {
     $$.left  = $1;
     $$.right = $2;
  }
;

entity_stmt_part
: entity_stmt_part entity_stmt
  {
     $$ = $1;
     tree_list_append(&$$, $2);
  }
| /* empty */ { $$ = NULL; }
;

entity_stmt
: id tCOLON entity_stmt_without_label
  {
     $$ = $3;
     if (tree_has_ident($$) && tree_ident($$) != $1)
        parse_error(&@1, "%s does not match statement name %s",
                    istr(tree_ident($$)), istr($1));
     else
        tree_set_ident($$, $1);
  }
| entity_stmt_without_label
  {
     $$ = $1;
     if (tree_has_ident($$))
        parse_error(&@$, "process does not have a label");
     else
        tree_set_ident($$, loc_to_ident(tree_loc($1)));
  }
;

entity_stmt_without_label
: conc_assertion_stmt      { $$ = $1; }
| conc_procedure_call_stmt { $$ = $1; } /* FIXME: must be passive. */
| process_stmt             { $$ = $1; } /* FIXME: must be passive. */
;

generic_clause
: tGENERIC tLPAREN interface_list tRPAREN tSEMI { $$ = $3; }
| /* empty */ { $$ = NULL; }
;

package_decl
: tPACKAGE id tIS package_decl_part tEND opt_package_token opt_id tSEMI
  {
     $$ = tree_new(T_PACKAGE);
     tree_set_loc($$, &@$);
     tree_set_ident($$, $2);
     copy_trees($4, tree_add_decl, $$);

     if ($7 != NULL && $7 != $2) {
        parse_error(&@7, "%s does not match package name %s",
                    istr($7), istr($2));
     }
  }
| tPACKAGE tBODY id tIS package_body_decl_part
  tEND opt_package_body opt_id tSEMI
  {
     $$ = tree_new(T_PACK_BODY);
     tree_set_loc($$, &@$);
     tree_set_ident($$, $3);
     copy_trees($5, tree_add_decl, $$);

     if ($8 != NULL && $8 != $3) {
        parse_error(&@7, "%s does not match package body name %s",
                    istr($8), istr($3));
     }
  }
;

opt_package_token : tPACKAGE | /* empty */ ;

opt_package_body : tPACKAGE tBODY | /* empty */ ;

package_decl_part
: package_decl_part package_decl_item
  {
     $$ = $1;
     tree_list_concat(&$$, $2);
  }
| /* empty */ { $$ = NULL; }
;

package_decl_item
: type_decl
| subtype_decl
| constant_decl
| subprogram_decl
| alias_decl
| attr_decl
| attr_spec
| component_decl
| file_decl
| shared_variable_decl
| signal_decl
| use_clause
/* | disconnection_specification
   | group_template_declaration
   | group_declaration */
;

package_body_decl_part
: package_body_decl_part package_body_decl_item
  {
     $$ = $1;
     tree_list_concat(&$$, $2);
  }
| /* empty */ { $$ = NULL; }
;

package_body_decl_item
: type_decl
| subtype_decl
| constant_decl
| subprogram_decl
| shared_variable_decl
| file_decl
| alias_decl
| use_clause
| attr_decl
| attr_spec
/* | group_template_declaration
   | group_declaration */
;

port_clause
: tPORT tLPAREN interface_list tRPAREN tSEMI { $$ = $3; }
| /* empty */ { $$ = NULL; }
;

arch_body
: tARCHITECTURE id tOF id tIS block_decl_part tBEGIN conc_stmt_list
  tEND opt_arch_token opt_id tSEMI
  {
     $$ = tree_new(T_ARCH);
     tree_set_ident($$, $2);
     tree_set_ident2($$, $4);
     tree_set_loc($$, &@$);
     copy_trees($6, tree_add_decl, $$);
     copy_trees($8, tree_add_stmt, $$);

     if ($11 != NULL && $11 != $2) {
        parse_error(&@11, "%s does not match architecture name %s",
                    istr($11), istr($2));
     }
  }
;

block_decl_item
: signal_decl
| type_decl
| subtype_decl
| constant_decl
| subprogram_decl
| alias_decl
| attr_decl
| attr_spec
| component_decl
| file_decl
| shared_variable_decl
| config_spec
| use_clause
/* | disconnection_specification
   | group_template_declaration
   | group_declaration */
;

config_spec
: tFOR id_list tCOLON id binding tSEMI
  {
     $$ = NULL;
     for (list_t *it = $2; it != NULL; it = it->next) {
        tree_t t = tree_new(T_SPEC);
        tree_set_ident(t, it->item.ident);
        tree_set_ident2(t, $4);
        tree_set_value(t, $5);
        tree_set_loc(t, &@$);

        tree_list_append(&$$, t);
     }

     list_free($2);
  }
| tFOR tOTHERS tCOLON id binding tSEMI
  {
     tree_t t = tree_new(T_SPEC);
     tree_set_ident2(t, $4);
     tree_set_value(t, $5);
     tree_set_loc(t, &@$);

     $$ = NULL;
     tree_list_append(&$$, t);
  }
| tFOR tALL tCOLON id binding tSEMI
  {
     tree_t t = tree_new(T_SPEC);
     tree_set_ident(t, ident_new("all"));
     tree_set_ident2(t, $4);
     tree_set_value(t, $5);
     tree_set_loc(t, &@$);

     $$ = NULL;
     tree_list_append(&$$, t);
  }
;

binding
: tUSE entity_aspect generic_map port_map
  {
     $$ = $2;
     copy_trees($4, tree_add_param, $$);
     copy_trees($3, tree_add_genmap, $$);
  }
| generic_map port_map
  {
     $$ = tree_new(T_BINDING);
     tree_set_loc($$, &@$);
     tree_set_class($$, C_DEFAULT);
     copy_trees($2, tree_add_param, $$);
     copy_trees($1, tree_add_genmap, $$);
  }
;

entity_aspect
: tENTITY selected_id
  {
     $$ = tree_new(T_BINDING);
     tree_set_loc($$, &@$);
     tree_set_class($$, C_ENTITY);
     tree_set_ident($$, $2);
  }
| tENTITY selected_id tLPAREN id tRPAREN
  {
     $$ = tree_new(T_BINDING);
     tree_set_loc($$, &@$);
     tree_set_class($$, C_ENTITY);
     tree_set_ident($$, $2);
     tree_set_ident2($$, $4);
  }
| tCONFIGURATION selected_id
  {
     $$ = tree_new(T_BINDING);
     tree_set_loc($$, &@$);
     tree_set_class($$, C_CONFIGURATION);
     tree_set_ident($$, $2);
  }
| tOPEN { $$ = NULL; }
;

file_decl
: tFILE id_list tCOLON subtype_indication tOPEN expr tIS expr tSEMI
  {
     add_file_decls(&$$, $2, $4, $6, $8, &@$);
  }
| tFILE id_list tCOLON subtype_indication tIS expr tSEMI
  {
     tree_t m = tree_new(T_REF);
     tree_set_ident(m, ident_new("READ_MODE"));

     add_file_decls(&$$, $2, $4, m, $6, &@$);
  }
| tFILE id_list tCOLON subtype_indication tSEMI
  {
     add_file_decls(&$$, $2, $4, NULL, NULL, &@$);
  }
| tFILE id_list tCOLON subtype_indication tIS tIN expr tSEMI
  {
     tree_t m = tree_new(T_REF);
     tree_set_ident(m, ident_new("READ_MODE"));

     add_file_decls(&$$, $2, $4, m, $7, &@$);
  }
| tFILE id_list tCOLON subtype_indication tIS tOUT expr tSEMI
  {
     tree_t m = tree_new(T_REF);
     tree_set_ident(m, ident_new("WRITE_MODE"));

     add_file_decls(&$$, $2, $4, m, $7, &@$);
  }
;

component_decl
: tCOMPONENT id opt_is generic_clause port_clause tEND
  tCOMPONENT opt_id tSEMI
  {
     tree_t t = tree_new(T_COMPONENT);
     tree_set_ident(t, $2);
     tree_set_loc(t, &@$);
     copy_trees($4, tree_add_generic, t);
     copy_trees($5, tree_add_port, t);

     if ($8 != NULL && $8 != $2) {
        parse_error(&@7, "%s does not match component name %s",
                    istr($8), istr($2));
     }

     $$ = NULL;
     tree_list_append(&$$, t);
  }
;

signal_decl
: tSIGNAL id_list tCOLON subtype_indication
  /* signal_kind */ opt_static_expr tSEMI
  {
     $$ = NULL;
     for (list_t *it = $2; it != NULL; it = it->next) {
        tree_t t = tree_new(T_SIGNAL_DECL);
        tree_set_ident(t, it->item.ident);
        tree_set_type(t, $4);
        tree_set_value(t, $5);
        tree_set_loc(t, &@$);

        tree_list_append(&$$, t);
     }

     list_free($2);
  }
;

constant_decl
: tCONSTANT id_list tCOLON subtype_indication opt_static_expr tSEMI
  {
     $$ = NULL;
     for (list_t *it = $2; it != NULL; it = it->next) {
        tree_t t = tree_new(T_CONST_DECL);
        tree_set_ident(t, it->item.ident);
        tree_set_type(t, $4);
        tree_set_value(t, $5);
        tree_set_loc(t, &@$);

        tree_list_append(&$$, t);
     }

     list_free($2);
  }
;

alias_decl
: tALIAS id tIS name tSEMI
  {
     tree_t a = tree_new(T_ALIAS);
     tree_set_loc(a, &@$);
     tree_set_ident(a, $2);
     tree_set_value(a, $4);

     $$ = NULL;
     tree_list_append(&$$, a);
  }
| tALIAS id tCOLON subtype_indication tIS name tSEMI
  {
     tree_t a = tree_new(T_ALIAS);
     tree_set_loc(a, &@$);
     tree_set_ident(a, $2);
     tree_set_value(a, $6);
     tree_set_type(a, $4);

     $$ = NULL;
     tree_list_append(&$$, a);
  }
;

attr_decl
: tATTRIBUTE id tCOLON type_mark tSEMI
  {
     tree_t a = tree_new(T_ATTR_DECL);
     tree_set_loc(a, &@$);
     tree_set_ident(a, $2);
     tree_set_type(a, $4);

     $$ = NULL;
     tree_list_append(&$$, a);
  }
;

attr_spec
: tATTRIBUTE id tOF id tCOLON entity_class tIS expr tSEMI
  {
     tree_t a = tree_new(T_ATTR_SPEC);
     tree_set_loc(a, &@$);
     tree_set_ident(a, $2);
     tree_set_ident2(a, $4);
     tree_set_value(a, $8);
     tree_set_class(a, $6);

     $$ = NULL;
     tree_list_append(&$$, a);
  }
;

entity_class
: tENTITY { $$ = C_ENTITY; }
| tARCHITECTURE { $$ = C_ARCHITECTURE; }
| tCONFIGURATION { $$ = C_CONFIGURATION; }
| tFUNCTION { $$ = C_FUNCTION; }
| tPROCEDURE { $$ = C_PROCEDURE; }
| tPACKAGE { $$ = C_PACKAGE; }
| tTYPE { $$ = C_TYPE; }
| tSUBTYPE { $$ = C_SUBTYPE; }
| tCONSTANT { $$ = C_CONSTANT; }
| tSIGNAL { $$ = C_SIGNAL; }
| tVARIABLE { $$ = C_VARIABLE; }
| tCOMPONENT { $$ = C_COMPONENT; }
| tLABEL { $$ = C_LABEL; }
;

conc_stmt_list
: conc_stmt_list conc_stmt
  {
     $$ = $1;
     tree_list_append(&$$, $2);
  }
| /* empty */
  {
     $$ = NULL;
  }
;

opt_arch_token : tARCHITECTURE | /* empty */ ;

interface_list
: interface_decl
  {
     $$ = $1;
  }
| interface_decl tSEMI interface_list
  {
     $$ = $1;
     tree_list_concat(&$$, $3);
  }
;

interface_decl
: interface_object_decl
  /* | interface_type_declaration
     | interface_subprogram_declaration
     | interface_package_declaration */
;

interface_object_decl
: object_class id_list tCOLON opt_mode subtype_indication
  /* opt_bus_token */ opt_static_expr
  {
     $$ = NULL;
     for (list_t *it = $2; it != NULL; it = it->next) {
        tree_t t = tree_new(T_PORT_DECL);
        tree_set_ident(t, it->item.ident);
        tree_set_subkind(t, $4);
        tree_set_type(t, $5);
        tree_set_value(t, $6);
        tree_set_loc(t, &@2);
        tree_set_class(t, $1);

        tree_list_append(&$$, t);
     }

     list_free($2);
  }
;

opt_static_expr
: tASSIGN expr
  {
     $$ = $2;
  }
| /* empty */
  { $$ = NULL; }
;

object_class
: tSIGNAL { $$ = C_SIGNAL; }
| tVARIABLE { $$ = C_VARIABLE; }
| tCONSTANT { $$ = C_CONSTANT; }
| tFILE { $$ = C_FILE; }
| /* empty */ { $$ = C_DEFAULT; }
;

opt_mode
: tIN { $$ = PORT_IN; }
| tOUT { $$ = PORT_OUT; }
| tINOUT { $$ = PORT_INOUT; }
| tBUFFER { $$ = PORT_BUFFER; }
| /* empty */ { $$ = PORT_IN; }
;

subtype_indication
: subtype_indication_constraint { $$ = $1; }
| id subtype_indication_constraint
  {
     tree_t r = tree_new(T_REF);
     tree_set_loc(r, &@1);
     tree_set_ident(r, $1);

     if (type_kind($2) != T_SUBTYPE) {
        $$ = type_new(T_SUBTYPE);
        type_set_base($$, $2);
     }
     else
        $$ = $2;

     type_set_resolution($$, r);
  }
;

subtype_indication_constraint
: type_mark { $$ = $1; }
| type_mark constraint
  {
     $$ = type_new(T_SUBTYPE);
     type_set_base($$, $1);

     for (list_t *it = $2; it != NULL; it = it->next)
        type_add_dim($$, it->item.range);
     list_free($2);
  }
;

constraint
: range_constraint
  {
     $$ = list_add(NULL, LISTVAL($1));
  }
| index_constraint
;

conc_stmt
: id tCOLON conc_stmt_without_label
  {
     $$ = $3;
     if (tree_has_ident($$) && tree_ident($$) != $1)
        parse_error(&@1, "%s does not match statement name %s",
                    istr(tree_ident($$)), istr($1));
     else
        tree_set_ident($$, $1);
  }
| conc_stmt_without_label
  {
     $$ = $1;
     if (tree_has_ident($$))
        parse_error(&@$, "process does not have a label");
     else
        tree_set_ident($$, loc_to_ident(tree_loc($1)));
  }
| comp_instance_stmt
| block_stmt
| generate_stmt
;

conc_stmt_without_label
: process_stmt
| conc_assign_stmt
| conc_select_assign_stmt
| conc_procedure_call_stmt
| conc_assertion_stmt
;

generate_stmt
: id tCOLON tIF expr generate_body
  {
     $$ = tree_new(T_IF_GENERATE);
     tree_set_loc($$, &@$);
     tree_set_ident($$, $1);
     tree_set_value($$, $4);
     copy_trees($5.left, tree_add_decl, $$);
     copy_trees($5.right, tree_add_stmt, $$);
  }
| id tCOLON tFOR id tIN range generate_body
  {
     $$ = tree_new(T_FOR_GENERATE);
     tree_set_loc($$, &@$);
     tree_set_ident($$, $1);
     tree_set_ident2($$, $4);
     tree_set_range($$, $6);
     copy_trees($7.left, tree_add_decl, $$);
     copy_trees($7.right, tree_add_stmt, $$);
  }
| id tCOLON tFOR id tIN expr generate_body
  {
     range_t r;
     if (!to_range_expr($6, &r, true))
        parse_error(&@6, "invalid range expression");

     $$ = tree_new(T_FOR_GENERATE);
     tree_set_loc($$, &@$);
     tree_set_ident($$, $1);
     tree_set_ident2($$, $4);
     tree_set_range($$, r);
     copy_trees($7.left, tree_add_decl, $$);
     copy_trees($7.right, tree_add_stmt, $$);
  }
;

generate_body
: tGENERATE block_decl_part tBEGIN conc_stmt_list
  tEND tGENERATE opt_id tSEMI
  {
     $$.left  = $2;
     $$.right = $4;
  }
| tGENERATE conc_stmt_list tEND tGENERATE opt_id tSEMI
  {
     $$.left  = NULL;
     $$.right = $2;
  }
;

comp_instance_stmt
: id tCOLON id generic_map port_map tSEMI
  {
     $$ = tree_new(T_INSTANCE);
     tree_set_loc($$, &@$);
     tree_set_ident($$, $1);
     tree_set_ident2($$, $3);
     tree_set_class($$, C_COMPONENT);
     copy_trees($5, tree_add_param, $$);
     copy_trees($4, tree_add_genmap, $$);
  }
| id tCOLON tCOMPONENT id generic_map port_map tSEMI
  {
     $$ = tree_new(T_INSTANCE);
     tree_set_loc($$, &@$);
     tree_set_ident($$, $1);
     tree_set_ident2($$, $4);
     tree_set_class($$, C_COMPONENT);
     copy_trees($6, tree_add_param, $$);
     copy_trees($5, tree_add_genmap, $$);
  }
| id tCOLON tENTITY selected_id generic_map port_map tSEMI
  {
     $$ = tree_new(T_INSTANCE);
     tree_set_loc($$, &@$);
     tree_set_ident($$, $1);
     tree_set_ident2($$, $4);
     tree_set_class($$, C_ENTITY);
     copy_trees($6, tree_add_param, $$);
     copy_trees($5, tree_add_genmap, $$);
  }
| id tCOLON tENTITY selected_id tLPAREN id tRPAREN generic_map
  port_map tSEMI
  {
     $$ = tree_new(T_INSTANCE);
     tree_set_loc($$, &@$);
     tree_set_ident($$, $1);
     tree_set_ident2($$, ident_prefix($4, $6, '-'));
     tree_set_class($$, C_ENTITY);
     copy_trees($9, tree_add_param, $$);
     copy_trees($8, tree_add_genmap, $$);
  }
| id tCOLON tCONFIGURATION selected_id generic_map port_map tSEMI
  {
     $$ = tree_new(T_INSTANCE);
     tree_set_loc($$, &@$);
     tree_set_ident($$, $1);
     tree_set_ident2($$, $4);
     tree_set_class($$, C_CONFIGURATION);
     copy_trees($6, tree_add_param, $$);
     copy_trees($5, tree_add_genmap, $$);
  }
;

generic_map
: tGENERIC tMAP tLPAREN param_list tRPAREN { $$ = $4; }
| /* empty */ { $$ = NULL; }
;

port_map
: tPORT tMAP tLPAREN param_list tRPAREN { $$ = $4; }
| /* empty */ { $$ = NULL; }
;

process_stmt
: opt_postponed tPROCESS process_sensitivity_clause opt_is
  process_decl_part tBEGIN seq_stmt_list tEND opt_postponed
  tPROCESS opt_id tSEMI
  {
     $$ = tree_new(T_PROCESS);
     tree_set_loc($$, &@3);
     copy_trees($5, tree_add_decl, $$);
     copy_trees($7, tree_add_stmt, $$);
     copy_trees($3, tree_add_trigger, $$);
     if ($11 != NULL)
        tree_set_ident($$, $11);
     if ($1)
        tree_add_attr_int($$, ident_new("postponed"), 1);
  }
;

process_sensitivity_clause
: tLPAREN name_list tRPAREN { $$ = $2; }
| /* empty */ { $$ = NULL; }
;

opt_is : tIS | /* empty */ ;

opt_postponed
: tPOSTPONED { $$ = true; }
| /* empty */ { $$ = false; }
;

process_decl_part
: process_decl_part process_decl_item
  {
     $$ = $1;
     tree_list_concat(&$$, $2);
  }
| /* empty */ { $$ = NULL; }
;

process_decl_item
: variable_decl
| type_decl
| subtype_decl
| constant_decl
| subprogram_decl
| alias_decl
| attr_decl
| attr_spec
| file_decl
| use_clause
  /* | group_template_declaration
     | group_declaration
  */
;

block_stmt
: id tCOLON tBLOCK /* [ ( guard_expression ) ] */ opt_is /*block_header*/
  block_decl_part tBEGIN conc_stmt_list tEND tBLOCK opt_id tSEMI
  {
     $$ = tree_new(T_BLOCK);
     tree_set_ident($$, $1);
     copy_trees($5, tree_add_decl, $$);
     copy_trees($7, tree_add_stmt, $$);

     if ($10 != NULL && $10 != $1)
        parse_error(&@10, "%s does not match block name %s",
                    istr($10), istr($1));
  }
;

block_decl_part
: block_decl_part block_decl_item
  {
     $$ = $1;
     tree_list_concat(&$$, $2);
  }
| /* empty */ { $$ = NULL; }
;

subprogram_decl
: func_type func_name formal_param_list tRETURN type_mark tSEMI
  {
     type_t t = type_new(T_FUNC);
     type_set_ident(t, $2);
     type_set_result(t, $5);

     tree_t f = tree_new(T_FUNC_DECL);
     tree_set_loc(f, &@$);
     tree_set_ident(f, $2);
     tree_set_type(f, t);

     copy_trees($3, tree_add_port, f);

     if ($1 != NULL)
        tree_add_attr_int(f, $1, 1);

     $$ = NULL;
     tree_list_append(&$$, f);
  }
| func_type func_name formal_param_list tRETURN type_mark tIS
  subprogram_decl_part tBEGIN seq_stmt_list tEND opt_func
  opt_func_name tSEMI
  {
     type_t t = type_new(T_FUNC);
     type_set_ident(t, $2);
     type_set_result(t, $5);

     tree_t f = tree_new(T_FUNC_BODY);
     tree_set_loc(f, &@$);
     tree_set_ident(f, $2);
     tree_set_type(f, t);

     copy_trees($3, tree_add_port, f);
     copy_trees($7, tree_add_decl, f);
     copy_trees($9, tree_add_stmt, f);

     if ($1 != NULL)
        tree_add_attr_int(f, $1, 1);

     $$ = NULL;
     tree_list_append(&$$, f);
  }
| tPROCEDURE id formal_param_list tSEMI
  {
     type_t t = type_new(T_PROC);
     type_set_ident(t, $2);

     tree_t p = tree_new(T_PROC_DECL);
     tree_set_loc(p, &@$);
     tree_set_ident(p, $2);
     tree_set_type(p, t);

     copy_trees($3, tree_add_port, p);

     $$ = NULL;
     tree_list_append(&$$, p);
  }
| tPROCEDURE id formal_param_list tIS subprogram_decl_part
  tBEGIN seq_stmt_list tEND opt_proc opt_id tSEMI
  {
     type_t t = type_new(T_PROC);
     type_set_ident(t, $2);

     tree_t p = tree_new(T_PROC_BODY);
     tree_set_loc(p, &@$);
     tree_set_ident(p, $2);
     tree_set_type(p, t);

     copy_trees($3, tree_add_port, p);
     copy_trees($5, tree_add_decl, p);
     copy_trees($7, tree_add_stmt, p);

     $$ = NULL;
     tree_list_append(&$$, p);
  }
;

func_type
: tPURE tFUNCTION { $$ = NULL; }
| tIMPURE tFUNCTION { $$ = ident_new("impure"); }
| tFUNCTION { $$ = NULL; }
;

opt_func : tFUNCTION | /* empty */ ;

opt_proc : tPROCEDURE | /* empty */ ;

operator_name
: tSTRING
  {
     for (char *p = $1; *p != '\0'; p++)
        *p = tolower((int)*p);
     $$ = ident_new($1);
     free($1);
  }
;

func_name : id | operator_name ;

opt_func_name : func_name | /* empty */ ;

subprogram_decl_part
: subprogram_decl_part subprogram_decl_item
  {
     $$ = $1;
     tree_list_concat(&$$, $2);
  }
| /* empty */ { $$ = NULL; }
;

subprogram_decl_item
: subprogram_decl
| subtype_decl
| constant_decl
| variable_decl
| type_decl
| alias_decl
| attr_decl
| attr_spec
| file_decl
| use_clause
  /* | group_template_declaration
     | group_declaration */
;

formal_param_list
: tLPAREN interface_list tRPAREN { $$ = $2; }
| /* empty */ { $$ = NULL; }
;

shared_variable_decl
: tSHARED variable_decl
  {
     $$ = $2;
     for (tree_list_t *it = $$; it != NULL; it = it->next)
        tree_add_attr_int(it->value, ident_new("shared"), 1);
  }
;

variable_decl
: tVARIABLE id_list tCOLON subtype_indication opt_static_expr tSEMI
  {
     $$ = NULL;
     for (list_t *it = $2; it != NULL; it = it->next) {
        tree_t t = tree_new(T_VAR_DECL);
        tree_set_ident(t, it->item.ident);
        tree_set_type(t, $4);
        tree_set_value(t, $5);
        tree_set_loc(t, &@$);

        tree_list_append(&$$, t);
     }

     list_free($2);
  }
;

conc_assign_stmt
: target tLE delay_mechanism conditional_waveforms tSEMI
  {
     $$ = tree_new(T_CASSIGN);
     tree_set_loc($$, &@$);
     tree_set_target($$, $1);

     for (tree_list_t *it = $4; it != NULL; it = it->next)
        set_delay_mechanism(it->value, $3);
     copy_trees($4, tree_add_cond, $$);
  }
;

conditional_waveforms
: waveform
  {
     tree_t c = tree_new(T_COND);
     tree_set_loc(c, &@1);
     copy_trees($1, tree_add_waveform, c);

     $$ = NULL;
     tree_list_append(&$$, c);
  }
| waveform tWHEN expr
  {
     tree_t c = tree_new(T_COND);
     tree_set_loc(c, &@1);
     tree_set_value(c, $3);
     copy_trees($1, tree_add_waveform, c);

     $$ = NULL;
     tree_list_append(&$$, c);
  }
| waveform tWHEN expr tELSE conditional_waveforms
  {
     tree_t c = tree_new(T_COND);
     tree_set_loc(c, &@1);
     tree_set_value(c, $3);
     copy_trees($1, tree_add_waveform, c);

     $$ = $5;
     tree_list_prepend(&$$, c);
  }
;

conc_procedure_call_stmt
: /* [ postponed ] */ name tLPAREN param_list tRPAREN tSEMI
  {
     $$ = tree_new(T_CPCALL);
     tree_set_loc($$, &@$);
     tree_set_ident2($$, tree_ident($1));
     copy_trees($3, tree_add_param, $$);
  }
;

conc_assertion_stmt
: /* [ postponed ] */ tASSERT expr report severity tSEMI
  {
     tree_t sev = $4;
     if (sev == NULL) {
        sev = tree_new(T_REF);
        tree_set_ident(sev, ident_new("ERROR"));
     }

     $$ = tree_new(T_CASSERT);
     tree_set_loc($$, &@$);
     tree_set_value($$, $2);
     tree_set_severity($$, sev);
     tree_set_message($$, $3);
  }
;

conc_select_assign_stmt
: tWITH expr tSELECT target tLE
  delay_mechanism selected_waveforms tSEMI
{
   $$ = tree_new(T_SELECT);
   tree_set_loc($$, &@$);
   tree_set_value($$, $2);

   for (tree_list_t *it = $7; it != NULL; it = it->next) {
      tree_t s = tree_value(it->value);
      tree_set_target(s, $4);
      set_delay_mechanism(s, $6);
   }
   copy_trees($7, tree_add_assoc, $$);
}

selected_waveforms
: waveform tWHEN choice_list tCOMMA selected_waveforms
  {
     tree_t s = tree_new(T_SIGNAL_ASSIGN);
     tree_set_ident(s, loc_to_ident(&@1));
     tree_set_loc(s, &@1);
     copy_trees($1, tree_add_waveform, s);

     for (tree_list_t *it = $3; it != NULL; it = it->next)
        tree_set_value(it->value, s);

     $$ = $3;
     tree_list_concat(&$$, $5);
  }
| waveform tWHEN choice_list
  {
     tree_t s = tree_new(T_SIGNAL_ASSIGN);
     tree_set_ident(s, loc_to_ident(&@1));
     tree_set_loc(s, &@1);
     copy_trees($1, tree_add_waveform, s);

     for (tree_list_t *it = $3; it != NULL; it = it->next)
        tree_set_value(it->value, s);

     $$ = $3;
  }
;

seq_stmt_list
: seq_stmt_list seq_stmt
  {
     $$ = $1;
     tree_list_append(&$$, $2);
  }
| /* empty */
  {
     $$ = NULL;
  }
;

seq_stmt
: id tCOLON seq_stmt_without_label
  {
     $$ = $3;
     tree_set_ident($$, $1);
  }
| seq_stmt_without_label
  {
     $$ = $1;
     tree_set_ident($$, loc_to_ident(&@1));
  }
;

seq_stmt_without_label
: tWAIT sensitivity_clause condition_clause timeout_clause tSEMI
  {
     $$ = tree_new(T_WAIT);
     tree_set_loc($$, &@$);
     if ($3 != NULL)
        tree_set_value($$, $3);
     if ($4 != NULL)
        tree_set_delay($$, $4);
     copy_trees($2, tree_add_trigger, $$);
  }
| target tASSIGN expr tSEMI
  {
     $$ = tree_new(T_VAR_ASSIGN);
     tree_set_target($$, $1);
     tree_set_value($$, $3);
     tree_set_loc($$, &@$);
  }
| target tLE delay_mechanism waveform tSEMI
  {
     $$ = tree_new(T_SIGNAL_ASSIGN);
     tree_set_target($$, $1);
     tree_set_loc($$, &@$);
     copy_trees($4, tree_add_waveform, $$);
     set_delay_mechanism($$, $3);
  }
| name tSEMI
  {
     switch (tree_kind($1)) {
     case T_FCALL:
        $$ = $1;
        tree_change_kind($$, T_PCALL);
        tree_set_ident2($$, tree_ident($$));
        break;

     case T_REF:
        $$ = tree_new(T_PCALL);
        tree_set_loc($$, &@1);
        tree_set_ident2($$, tree_ident($1));
        break;

     default:
        parse_error(&@1, "invalid procedure call");
        $$ = tree_new(T_NULL);
     }
  }
| tASSERT expr report severity tSEMI
  {
     tree_t sev = $4;
     if (sev == NULL) {
        sev = tree_new(T_REF);
        tree_set_ident(sev, ident_new("ERROR"));
     }

     $$ = tree_new(T_ASSERT);
     tree_set_loc($$, &@$);
     tree_set_value($$, $2);
     tree_set_severity($$, sev);
     tree_set_message($$, $3);
  }
| tREPORT expr severity tSEMI
  {
     tree_t false_ref = tree_new(T_REF);
     tree_set_ident(false_ref, ident_new("FALSE"));

     tree_t sev = $3;
     if (sev == NULL) {
        sev = tree_new(T_REF);
        tree_set_ident(sev, ident_new("NOTE"));
     }

     $$ = tree_new(T_ASSERT);
     tree_set_loc($$, &@$);
     tree_set_value($$, false_ref);
     tree_set_severity($$, sev);
     tree_set_message($$, $2);
     tree_add_attr_int($$, ident_new("is_report"), 1);
  }
| tIF expr tTHEN seq_stmt_list tEND tIF opt_id tSEMI
  {
     $$ = tree_new(T_IF);
     tree_set_loc($$, &@$);
     tree_set_value($$, $2);
     copy_trees($4, tree_add_stmt, $$);
  }
| tIF expr tTHEN seq_stmt_list tELSIF elsif_list
  {
     $$ = tree_new(T_IF);
     tree_set_loc($$, &@$);
     tree_set_value($$, $2);
     copy_trees($4, tree_add_stmt, $$);
     tree_add_else_stmt($$, $6);
  }
| tIF expr tTHEN seq_stmt_list tELSE seq_stmt_list
  tEND tIF opt_id tSEMI
  {
     $$ = tree_new(T_IF);
     tree_set_loc($$, &@$);
     tree_set_value($$, $2);
     copy_trees($4, tree_add_stmt, $$);
     copy_trees($6, tree_add_else_stmt, $$);
  }
| tNULL tSEMI
  {
     $$ = tree_new(T_NULL);
     tree_set_loc($$, &@$);
  }
| tRETURN expr tSEMI
  {
     $$ = tree_new(T_RETURN);
     tree_set_loc($$, &@$);
     tree_set_value($$, $2);
  }
| tRETURN tSEMI
  {
     $$ = tree_new(T_RETURN);
     tree_set_loc($$, &@$);
  }
| tWHILE expr tLOOP seq_stmt_list tEND tLOOP opt_id tSEMI
  {
     $$ = tree_new(T_WHILE);
     tree_set_loc($$, &@$);
     tree_set_value($$, $2);
     copy_trees($4, tree_add_stmt, $$);
  }
| tLOOP seq_stmt_list tEND tLOOP opt_id tSEMI
  {
     tree_t true_ref = tree_new(T_REF);
     tree_set_ident(true_ref, ident_new("TRUE"));

     $$ = tree_new(T_WHILE);
     tree_set_loc($$, &@$);
     tree_set_value($$, true_ref);
     copy_trees($2, tree_add_stmt, $$);
  }
| tFOR id tIN range tLOOP seq_stmt_list tEND tLOOP opt_id tSEMI
  {
     $$ = tree_new(T_FOR);
     tree_set_loc($$, &@$);
     tree_set_ident2($$, $2);
     tree_set_range($$, $4);
     copy_trees($6, tree_add_stmt, $$);
  }
| tFOR id tIN expr tLOOP seq_stmt_list tEND tLOOP opt_id tSEMI
  {
     range_t r;
     if (!to_range_expr($4, &r, true))
        parse_error(&@4, "invalid range expression");

     $$ = tree_new(T_FOR);
     tree_set_loc($$, &@$);
     tree_set_ident2($$, $2);
     tree_set_range($$, r);
     copy_trees($6, tree_add_stmt, $$);
  }
| tEXIT tSEMI
  {
     $$ = tree_new(T_EXIT);
     tree_set_loc($$, &@$);
  }
| tEXIT tWHEN expr tSEMI
  {
     $$ = tree_new(T_EXIT);
     tree_set_loc($$, &@$);
     tree_set_value($$, $3);
  }
| tEXIT id tSEMI
  {
     $$ = tree_new(T_EXIT);
     tree_set_loc($$, &@$);
     tree_set_ident2($$, $2);
  }
| tEXIT id tWHEN expr tSEMI
  {
     $$ = tree_new(T_EXIT);
     tree_set_loc($$, &@$);
     tree_set_ident2($$, $2);
     tree_set_value($$, $4);
  }
| tNEXT tSEMI
  {
     $$ = tree_new(T_NEXT);
     tree_set_loc($$, &@$);
  }
| tNEXT tWHEN expr tSEMI
  {
     $$ = tree_new(T_NEXT);
     tree_set_loc($$, &@$);
     tree_set_value($$, $3);
  }
| tNEXT id tSEMI
  {
     $$ = tree_new(T_NEXT);
     tree_set_loc($$, &@$);
     tree_set_ident2($$, $2);
  }
| tNEXT id tWHEN expr tSEMI
  {
     $$ = tree_new(T_NEXT);
     tree_set_loc($$, &@$);
     tree_set_value($$, $4);
     tree_set_ident2($$, $2);
  }
| tCASE expr tIS case_alt_list tEND tCASE opt_id tSEMI
  {
     $$ = tree_new(T_CASE);
     tree_set_loc($$, &@$);
     tree_set_value($$, $2);

     copy_trees($4, tree_add_assoc, $$);
  }
;

case_alt_list
: tWHEN choice_list tASSOC seq_stmt_list
  {
     tree_t b = tree_new(T_BLOCK);
     tree_set_ident(b, loc_to_ident(&@4));
     tree_set_loc(b, &@4);
     copy_trees($4, tree_add_stmt, b);

     for (tree_list_t *it = $2; it != NULL; it = it->next)
        tree_set_value(it->value, b);

     $$ = $2;
  }
| tWHEN choice_list tASSOC seq_stmt_list case_alt_list
  {
     tree_t b = tree_new(T_BLOCK);
     tree_set_ident(b, loc_to_ident(&@4));
     tree_set_loc(b, &@4);
     copy_trees($4, tree_add_stmt, b);

     for (tree_list_t *it = $2; it != NULL; it = it->next)
        tree_set_value(it->value, b);

     $$ = $2;
     tree_list_concat(&$$, $5);
  }
;

choice_list
: choice { $$ = NULL; tree_list_append(&$$, $1); }
| choice tBAR choice_list { $$ = $3; tree_list_prepend(&$$, $1); }
;

choice
: expr
  {
     $$ = tree_new(T_ASSOC);
     tree_set_loc($$, &@$);

     range_t r;
     if (to_range_expr($1, &r, false)) {
        tree_set_subkind($$, A_RANGE);
        tree_set_range($$, r);
     }
     else {
        tree_set_subkind($$, A_NAMED);
        tree_set_name($$, $1);
     }
  }
| range
  {
     $$ = tree_new(T_ASSOC);
     tree_set_loc($$, &@$);
     tree_set_subkind($$, A_RANGE);
     tree_set_range($$, $1);
  }
| tOTHERS
  {
     $$ = tree_new(T_ASSOC);
     tree_set_loc($$, &@$);
     tree_set_subkind($$, A_OTHERS);
  }
;


elsif_list
: expr tTHEN seq_stmt_list tEND tIF opt_id tSEMI
  {
     $$ = tree_new(T_IF);
     tree_set_ident($$, ident_uniq("elsif"));
     tree_set_loc($$, &@$);
     tree_set_value($$, $1);
     copy_trees($3, tree_add_stmt, $$);
  }
| expr tTHEN seq_stmt_list tELSE seq_stmt_list
  tEND tIF opt_id tSEMI
  {
     $$ = tree_new(T_IF);
     tree_set_ident($$, ident_uniq("elsif"));
     tree_set_loc($$, &@$);
     tree_set_value($$, $1);
     copy_trees($3, tree_add_stmt, $$);
     copy_trees($5, tree_add_else_stmt, $$);
  }
| expr tTHEN seq_stmt_list tELSIF elsif_list
  {
     $$ = tree_new(T_IF);
     tree_set_ident($$, ident_uniq("elsif"));
     tree_set_loc($$, &@$);
     tree_set_value($$, $1);
     copy_trees($3, tree_add_stmt, $$);
     tree_add_else_stmt($$, $5);
  }
;

report
: /* empty */
  {
     if (assert_viol == NULL)
        assert_viol = str_to_agg("Assertion violation.", NULL, &LOC_INVALID);

     $$ = assert_viol;
  }
| tREPORT expr { $$ = $2; }
;

severity
: /* empty */ { $$ = NULL; }
| tSEVERITY expr { $$ = $2; }
;

target
: name
| aggregate
;

waveform
: waveform_element
  {
     $$ = NULL;
     tree_list_append(&$$, $1);
  }
| waveform_element tCOMMA waveform
  {
     $$ = $3;
     tree_list_prepend(&$$, $1);
  }
| tUNAFFECTED { $$ = NULL; }
;

waveform_element
: expr
  {
     $$ = tree_new(T_WAVEFORM);
     tree_set_loc($$, &@$);
     tree_set_value($$, $1);
  }
| expr tAFTER expr
  {
     $$ = tree_new(T_WAVEFORM);
     tree_set_loc($$, &@$);
     tree_set_value($$, $1);
     tree_set_delay($$, $3);
  }
;

condition_clause
: tUNTIL expr { $$ = $2; }
| /* empty */ { $$ = NULL; }
;

timeout_clause
: tFOR expr { $$ = $2; }
| /* empty */ { $$ = NULL; }
;

sensitivity_clause
: tON name_list { $$ = $2; }
| /* empty */ { $$ = NULL; }
;

subtype_decl
: tSUBTYPE id tIS subtype_indication tSEMI
  {
     type_t sub = $4;
     if (type_kind(sub) != T_SUBTYPE) {
        // Case where subtype_indication did not impose any
        // constraint so we must create the subtype object here
        sub = type_new(T_SUBTYPE);
        type_set_base(sub, $4);
     }
     type_set_ident(sub, $2);

     tree_t t = tree_new(T_TYPE_DECL);
     tree_set_ident(t, $2);
     tree_set_type(t, sub);
     tree_set_loc(t, &@$);

     $$ = NULL;
     tree_list_append(&$$, t);
  }
;

type_decl
: tTYPE id tIS type_def tSEMI
  {
     tree_t t = tree_new(T_TYPE_DECL);
     tree_set_ident(t, $2);
     tree_set_type(t, $4);
     tree_set_loc(t, &@$);

     if (type_has_ident($4)) {
        if (type_ident($4) != $2)
           parse_error(&@$, "expected name %s at end of type declaration",
                       istr($2));
     }
     else
        type_set_ident($4, $2);

     $$ = NULL;
     tree_list_append(&$$, t);
  }
| tTYPE id tSEMI
  {
     type_t i = type_new(T_INCOMPLETE);
     type_set_ident(i, $2);

     tree_t t = tree_new(T_TYPE_DECL);
     tree_set_ident(t, $2);
     tree_set_type(t, i);
     tree_set_loc(t, &@$);

     $$ = NULL;
     tree_list_append(&$$, t);
  }
;

type_def
: scalar_type_def
| array_type_def
| access_type_def
| file_type_def
| record_type_def
;

file_type_def
: tFILE tOF type_mark
  {
     $$ = type_new(T_FILE);
     type_set_file($$, $3);
  }
;

elem_decl_list
: elem_decl elem_decl_list
  {
     $$ = $1;
     tree_list_concat(&$$, $2);
  }
| elem_decl
  {
     $$ = $1;
  }
;

elem_decl
: id_list tCOLON subtype_indication tSEMI
  {
     $$ = NULL;
     for (list_t *it = $1; it != NULL; it = it->next) {
        tree_t t = tree_new(T_FIELD_DECL);
        tree_set_ident(t, it->item.ident);
        tree_set_type(t, $3);
        tree_set_loc(t, &@$);

        tree_list_append(&$$, t);
     }

     list_free($1);
  }
;

record_type_def
: tRECORD elem_decl_list tEND tRECORD opt_id
  {
     $$ = type_new(T_RECORD);

     for (tree_list_t *it = $2; it != NULL; it = it->next)
        type_add_field($$, it->value);
     tree_list_free($2);

     if ($5 != NULL)
        type_set_ident($$, $5);
  }
;

access_type_def
: tACCESS subtype_indication
  {
     $$ = type_new(T_ACCESS);
     type_set_access($$, $2);
  }
;

array_type_def
: unconstrained_array_def
| constrained_array_def
;

constrained_array_def
: tARRAY index_constraint tOF subtype_indication
  {
     $$ = type_new(T_CARRAY);
     type_set_elem($$, $4);

     for (list_t *it = $2; it != NULL; it = it->next)
        type_add_dim($$, it->item.range);
     list_free($2);
  }
;

index_constraint : tLPAREN constraint_list tRPAREN { $$ = $2; } ;

constraint_list
: constraint_elem
  {
     $$ = list_add(NULL, LISTVAL($1));
  }
| constraint_elem tCOMMA constraint_list
  {
     $$ = list_add($3, LISTVAL($1));
  }
;

constraint_elem
: expr tTO expr
  {
     $$.kind  = RANGE_TO;
     $$.left  = $1;
     $$.right = $3;
  }
| expr tDOWNTO expr
  {
     $$.kind  = RANGE_DOWNTO;
     $$.left  = $1;
     $$.right = $3;
  }
| expr
  {
     $$.kind  = RANGE_EXPR;
     $$.left  = $1;
     $$.right = NULL;
  }
| name range_constraint
  {
     if (tree_kind($1) != T_REF)
        parse_error(&@1, "invalid type mark");

     type_t type = type_new(T_UNRESOLVED);
     type_set_ident(type, tree_ident($1));

     $$ = $2;
     tree_set_type($$.left, type);
     tree_set_type($$.right, type);
  }
;

unconstrained_array_def
: tARRAY tLPAREN index_subtype_def tRPAREN tOF subtype_indication
  {
     $$ = $3;
     type_set_elem($$, $6);
  }
;

index_subtype_def
: name tRANGE tBOX
  {
     type_t type = type_new(T_UNRESOLVED);
     type_set_ident(type, tree_ident($1));

     $$ = type_new(T_UARRAY);
     type_add_index_constr($$, type);
  }
| index_subtype_def tCOMMA type_mark tRANGE tBOX
  {
     $$ = $1;
     type_add_index_constr($$, $3);
  }
;

scalar_type_def
: physical_type_def
| enum_type_def
| range_constraint
  {
     bool real = ((tree_kind($1.left) == T_LITERAL)
                  && (tree_subkind($1.left) == L_REAL))
        || ((tree_kind($1.right) == T_LITERAL)
            && (tree_subkind($1.right) == L_REAL));

     $$ = type_new(real ? T_REAL : T_INTEGER);
     type_add_dim($$, $1);
  }
;

enum_type_def
: tLPAREN enum_lit_list tRPAREN
  {
     $$ = type_new(T_ENUM);

     unsigned pos = 0;
     for (tree_list_t *it = $2; it != NULL; it = it->next) {
        tree_set_type(it->value, $$);
        tree_set_pos(it->value, pos++);
        type_enum_add_literal($$, it->value);
     }
     tree_list_free($2);
  }
;

enum_lit_list
: id
  {
     tree_t t = tree_new(T_ENUM_LIT);
     tree_set_ident(t, $1);
     tree_set_loc(t, &@$);

     $$ = NULL;
     tree_list_append(&$$, t);
  }
| id tCOMMA enum_lit_list
  {
     tree_t t = tree_new(T_ENUM_LIT);
     tree_set_ident(t, $1);
     tree_set_loc(t, &@1);

     $$ = $3;
     tree_list_prepend(&$$, t);
  }
;

physical_type_def
: range_constraint tUNITS base_unit_decl secondary_unit_decls tEND
  tUNITS opt_id
  {
     range_t r = $1;
     r.left  = int_to_physical(r.left, $3);
     r.right = int_to_physical(r.right, $3);

     $$ = type_new(T_PHYSICAL);
     type_add_dim($$, r);

     tree_set_type($3, $$);
     type_add_unit($$, $3);

     for (tree_list_t *it = $4; it != NULL; it = it->next) {
        tree_set_type(it->value, $$);
        type_add_unit($$, it->value);
     }
     tree_list_free($4);
  }
;

base_unit_decl
: id tSEMI
  {
     tree_t mult = tree_new(T_LITERAL);
     tree_set_loc(mult, &@$);
     tree_set_subkind(mult, L_INT);
     tree_set_ival(mult, 1);

     $$ = tree_new(T_UNIT_DECL);
     tree_set_loc($$, &@$);
     tree_set_value($$, mult);
     tree_set_ident($$, $1);
  }
;

secondary_unit_decls
: /* empty */ { $$ = NULL; }
| id tEQ physical_literal tSEMI secondary_unit_decls
  {
     tree_t u = tree_new(T_UNIT_DECL);
     tree_set_loc(u, &@$);
     tree_set_ident(u, $1);
     tree_set_value(u, $3);

     $$ = $5;
     tree_list_prepend(&$$, u);
  }
;

range_constraint : tRANGE range { $$ = $2; } ;

range
: expr tTO expr
  {
     $$.left  = $1;
     $$.right = $3;
     $$.kind  = RANGE_TO;
  }
| expr tDOWNTO expr
  {
     $$.left  = $1;
     $$.right = $3;
     $$.kind  = RANGE_DOWNTO;
  }
;

delay_mechanism
: tTRANSPORT { $$ = get_time(0); }
| tREJECT expr tINERTIAL { $$ = $2; }
| tINERTIAL { $$ = NULL; }
| /* empty */ { $$ = NULL; }  // Default to inertial
;

expr
: expr tAND expr { $$ = build_expr2("\"and\"", $1, $3, &@$); }
| expr tOR expr { $$ = build_expr2("\"or\"", $1, $3, &@$); }
| expr tNAND expr { $$ = build_expr2("\"nand\"", $1, $3, &@$); }
| expr tNOR expr { $$ = build_expr2("\"nor\"", $1, $3, &@$); }
| expr tXOR expr { $$ = build_expr2("\"xor\"", $1, $3, &@$); }
| expr tXNOR expr { $$ = build_expr2("\"xnor\"", $1, $3, &@$); }
| expr tEQ expr { $$ = build_expr2("\"=\"", $1, $3, &@$); }
| expr tNEQ expr { $$ = build_expr2("\"/=\"", $1, $3, &@$); }
| expr tLT expr { $$ = build_expr2("\"<\"", $1, $3, &@$); }
| expr tLE expr { $$ = build_expr2("\"<=\"", $1, $3, &@$); }
| expr tGT expr { $$ = build_expr2("\">\"", $1, $3, &@$); }
| expr tGE expr { $$ = build_expr2("\">=\"", $1, $3, &@$); }
| expr tSLL expr { $$ = build_expr2("\"sll\"", $1, $3, &@$); }
| expr tSRL expr { $$ = build_expr2("\"srl\"", $1, $3, &@$); }
| expr tSLA expr { $$ = build_expr2("\"sla\"", $1, $3, &@$); }
| expr tSRA expr { $$ = build_expr2("\"sra\"", $1, $3, &@$); }
| expr tROL expr { $$ = build_expr2("\"rol\"", $1, $3, &@$); }
| expr tROR expr { $$ = build_expr2("\"ror\"", $1, $3, &@$); }
| expr tPLUS expr { $$ = build_expr2("\"+\"", $1, $3, &@$); }
| expr tMINUS expr { $$ = build_expr2("\"-\"", $1, $3, &@$); }
| expr tTIMES expr { $$ = build_expr2("\"*\"", $1, $3, &@$); }
| expr tOVER expr { $$ = build_expr2("\"/\"", $1, $3, &@$); }
| expr tMOD expr { $$ = build_expr2("\"mod\"", $1, $3, &@$); }
| expr tREM expr { $$ = build_expr2("\"rem\"", $1, $3, &@$); }
| expr tPOWER expr { $$ = build_expr2("\"**\"", $1, $3, &@$); }
| tNOT expr { $$ = build_expr1("\"not\"", $2, &@$); }
| tABS expr { $$ = build_expr1("\"abs\"", $2, &@$); }
| tMINUS expr { $$ = build_expr1("\"-\"", $2, &@$); }
| tPLUS expr { $$ = build_expr1("\"+\"", $2, &@$); }
| expr tAMP expr
  {
     $$ = tree_new(T_CONCAT);
     tree_set_loc($$, &@$);

     tree_t p1 = tree_new(T_PARAM);
     tree_set_loc(p1, &@1);
     tree_set_subkind(p1, P_POS);
     tree_set_value(p1, $1);

     tree_t p2 = tree_new(T_PARAM);
     tree_set_loc(p2, &@3);
     tree_set_subkind(p2, P_POS);
     tree_set_value(p2, $3);

     tree_add_param($$, p1);
     tree_add_param($$, p2);
  }
| name
| literal
| name tTICK aggregate
  {
     $$ = tree_new(T_QUALIFIED);
     tree_set_value($$, $3);
     tree_set_loc($$, &@$);

     if (tree_kind($1) != T_REF)
        parse_error(&@1, "invalid qualified expression");
     else
        tree_set_ident($$, tree_ident($1));
  }
| aggregate
| tNEW expr
  {
     $$ = tree_new(T_NEW);
     tree_set_value($$, $2);
     tree_set_loc($$, &@$);
  }
;

param_list
: expr_or_open
  {
     tree_t p = tree_new(T_PARAM);
     tree_set_subkind(p, P_POS);
     tree_set_value(p, $1);
     tree_set_loc(p, &@$);

     $$ = NULL;
     tree_list_prepend(&$$, p);
  }
| expr_or_open tCOMMA param_list
  {
     tree_t p = tree_new(T_PARAM);
     tree_set_subkind(p, P_POS);
     tree_set_value(p, $1);
     tree_set_loc(p, &@1);

     $$ = $3;
     tree_list_prepend(&$$, p);
  }
| name tASSOC expr_or_open
  {
     tree_t p = tree_new(T_PARAM);
     tree_set_subkind(p, P_NAMED);
     tree_set_value(p, $3);
     tree_set_name(p, $1);
     tree_set_loc(p, &@$);

     $$ = NULL;
     tree_list_prepend(&$$, p);
  }
| name tASSOC expr_or_open tCOMMA param_list
  {
     tree_t p = tree_new(T_PARAM);
     tree_set_subkind(p, P_NAMED);
     tree_set_value(p, $3);
     tree_set_name(p, $1);
     tree_set_loc(p, &@3);

     $$ = $5;
     tree_list_prepend(&$$, p);
  }
;

expr_or_open
: expr
| tOPEN
  {
     $$ = tree_new(T_OPEN);
     tree_set_loc($$, &@$);
  }
;

aggregate
: tLPAREN element_assoc_list tRPAREN
  {
     // The grammar is ambiguous between an aggregate with a
     // single positional element and a parenthesised expression
     if (($2->next == NULL) && (tree_subkind($2->value) == A_POS)) {
        $$ = tree_value($2->value);
        tree_list_free($2);
     }
     else {
        $$ = tree_new(T_AGGREGATE);
        tree_set_loc($$, &@$);
        copy_trees($2, tree_add_assoc, $$);
     }
  }
;

element_assoc_list
: element_assoc
  {
     $$ = $1;
  }
| element_assoc_list tCOMMA element_assoc
  {
     $$ = $1;
     tree_list_concat(&$$, $3);
  }
;

element_assoc
: expr
  {
     tree_t a = tree_new(T_ASSOC);
     tree_set_loc(a, tree_loc($1));
     tree_set_subkind(a, A_POS);
     tree_set_value(a, $1);

     $$ = NULL;
     tree_list_append(&$$, a);
  }
| choice_list tASSOC expr
  {
     $$ = $1;
     for (tree_list_t *it = $1; it != NULL; it = it->next)
        tree_set_value(it->value, $3);
  }
;

literal
: numeric_literal
| string_literal
| bit_string_literal
| null_literal
;

null_literal
: tNULL
  {
     $$ = tree_new(T_LITERAL);
     tree_set_loc($$, &@$);
     tree_set_subkind($$, L_NULL);
  }
;

string_literal
: tSTRING
  {
     size_t len = strlen($1);
     $$ = str_to_agg($1 + 1, $1 + len - 1, &@$);
     tree_set_loc($$, &@$);

     free($1);
  }
;

bit_string_literal
: tBITSTRING
  {
     $$ = bit_str_to_agg($1, &@$);
     free($1);
  }

numeric_literal : abstract_literal | physical_literal ;

abstract_literal
: tINT
  {
     $$ = tree_new(T_LITERAL);
     tree_set_loc($$, &@$);
     tree_set_subkind($$, L_INT);
     tree_set_ival($$, $1);
  }
| tREAL
  {
     $$ = tree_new(T_LITERAL);
     tree_set_loc($$, &@$);
     tree_set_subkind($$, L_REAL);
     tree_set_dval($$, $1);
  }
;

physical_literal
: abstract_literal id
  {
     tree_t unit = tree_new(T_REF);
     tree_set_ident(unit, $2);
     tree_set_loc(unit, &@2);

     $$ = tree_new(T_FCALL);
     tree_set_loc($$, &@$);
     tree_set_ident($$, ident_new("\"*\""));

     tree_t left = tree_new(T_PARAM);
     tree_set_subkind(left, P_POS);
     tree_set_value(left, $1);

     tree_t right = tree_new(T_PARAM);
     tree_set_subkind(right, P_POS);
     tree_set_value(right, unit);

     tree_add_param($$, left);
     tree_add_param($$, right);
  }
;

type_mark
: selected_id
  {
     $$ = type_new(T_UNRESOLVED);
     type_set_ident($$, $1);
  }
;

name
: id
  {
     $$ = tree_new(T_REF);
     tree_set_ident($$, $1);
     tree_set_loc($$, &@$);
  }
| name tTICK id
  {
     $$ = tree_new(T_ATTR_REF);
     tree_set_name($$, $1);
     tree_set_ident($$, $3);
     tree_set_loc($$, &@$);
  }
| name tTICK tRANGE
  {
     $$ = tree_new(T_ATTR_REF);
     tree_set_name($$, $1);
     tree_set_ident($$, ident_new("RANGE"));
     tree_set_loc($$, &@$);
  }
| name tLPAREN param_list tRPAREN
  {
     $$ = handle_param_expr($1, $3);
     tree_set_loc($$, &@$);
  }
| operator_name tLPAREN param_list tRPAREN
  {
     tree_t name = tree_new(T_REF);
     tree_set_ident(name, $1);
     tree_set_loc(name, &@1);

     $$ = handle_param_expr(name, $3);
     tree_set_loc($$, &@$);
  }
| name tLPAREN range tRPAREN
  {
     $$ = tree_new(T_ARRAY_SLICE);
     tree_set_value($$, $1);
     tree_set_range($$, $3);
     tree_set_loc($$, &@$);
  }
| name tDOT tALL
  {
     $$ = tree_new(T_ALL);
     tree_set_value($$, $1);
     tree_set_loc($$, &@$);
  }
| name tDOT id
  {
     if (tree_kind($1) == T_REF) {
        $$ = $1;
        tree_set_ident($$, ident_prefix(tree_ident($1), $3, '.'));
        tree_set_loc($$, &@$);
     }
     else {
        $$ = tree_new(T_RECORD_REF);
        tree_set_value($$, $1);
        tree_set_ident($$, $3);
        tree_set_loc($$, &@$);
     }
  }
| name tDOT tSTRING
  {
     if (tree_kind($1) == T_REF) {
        $$ = $1;
        tree_set_ident($$, ident_prefix(tree_ident($1), ident_new($3), '.'));
        tree_set_loc($$, &@$);
        free($3);
     }
     else
        parse_error(&@1, "invalid selected name");
  }
;

name_list
: name
  {
     $$ = NULL;
     tree_list_append(&$$, $1);
  }
| name tCOMMA name_list
  {
     $$ = $3;
     tree_list_prepend(&$$, $1);
  }
;

%%

static void tree_list_append(struct tree_list **l, tree_t t)
{
   struct tree_list *new = xmalloc(sizeof(struct tree_list));
   new->next  = NULL;
   new->value = t;
   new->tail  = new;

   tree_list_concat(l, new);
}

static void tree_list_prepend(struct tree_list **l, tree_t t)
{
   struct tree_list *new = xmalloc(sizeof(struct tree_list));
   new->next  = *l;
   new->value = t;
   new->tail  = (*l != NULL) ? (*l)->tail : new;

   *l = new;
}

static void tree_list_concat(struct tree_list **a, struct tree_list *b)
{
   if (*a == NULL)
      *a = b;
   else if (b != NULL) {
      assert((*a)->tail->next == NULL);
      (*a)->tail->next = b;
      (*a)->tail = b->tail;
   }
}

static void tree_list_free(struct tree_list *l)
{
   while (l != NULL) {
      struct tree_list *next = l->next;
      free(l);
      l = next;
   }
}

static void copy_trees(tree_list_t *from,
                       void (*copy_fn)(tree_t t, tree_t d),
                       tree_t to)
{
   while (from != NULL) {
      (*copy_fn)(to, from->value);
      tree_list_t *next = from->next;
      free(from);
      from = next;
   }
}

static list_t *list_add(list_t *list, union listval item)
{
   list_t *new = xmalloc(sizeof(list_t));
   new->next = list;
   new->item = item;
   return new;
}

static void list_free(list_t *list)
{
   while (list != NULL) {
      list_t *next = list->next;
      free(list);
      list = next;
   }
}

static tree_t build_expr1(const char *fn, tree_t arg,
                          const struct YYLTYPE *loc)
{
   tree_t t = tree_new(T_FCALL);
   tree_set_ident(t, ident_new(fn));

   tree_t parg = tree_new(T_PARAM);
   tree_set_loc(parg, tree_loc(arg));
   tree_set_subkind(parg, P_POS);
   tree_set_value(parg, arg);

   tree_add_param(t, parg);
   tree_set_loc(t, loc);

   return t;
}

static tree_t build_expr2(const char *fn, tree_t left, tree_t right,
                          const struct YYLTYPE *loc)
{
   tree_t t = tree_new(T_FCALL);
   tree_set_ident(t, ident_new(fn));

   tree_t pleft = tree_new(T_PARAM);
   tree_set_loc(pleft, tree_loc(left));
   tree_set_subkind(pleft, P_POS);
   tree_set_value(pleft, left);

   tree_t pright = tree_new(T_PARAM);
   tree_set_loc(pright, tree_loc(right));
   tree_set_subkind(pright, P_POS);
   tree_set_value(pright, right);

   tree_add_param(t, pleft);
   tree_add_param(t, pright);
   tree_set_loc(t, loc);

   return t;
}

static tree_t str_to_agg(const char *start, const char *end, const loc_t *loc)
{
   tree_t t = tree_new(T_AGGREGATE);

   for (const char *p = start; *p != '\0' && p != end; p++) {
      if (*p == -127)
         continue;

      const char ch[] = { '\'', *p, '\'', '\0' };

      tree_t ref = tree_new(T_REF);
      tree_set_ident(ref, ident_new(ch));
      tree_set_loc(ref, loc);

      tree_t a = tree_new(T_ASSOC);
      tree_set_subkind(a, A_POS);
      tree_set_value(a, ref);

      tree_add_assoc(t, a);
   }

   return t;
}

static tree_t handle_param_expr(tree_t name, tree_list_t *params)
{
   // This is ambiguous between an array reference and a function
   // call: in the case where $1 is a simple reference assume it
   // is a function call for now and the semantic checker will
   // fix things up later. We stash the value $1 in the tree
   // anyway to make changing the kind easier.

   range_t r;
   if ((params->next == NULL)
       && to_range_expr(tree_value(params->value), &r, false)) {
      // Convert range parameters into array slices
      tree_t s = tree_new(T_ARRAY_SLICE);
      tree_set_value(s, name);
      tree_set_range(s, r);
      free(params);
      return s;
   }
   else {
      tree_t a;
      if ((tree_kind(name) == T_ATTR_REF) && (tree_params(name) == 0))
         a = name;
      else {
         a = tree_new(T_ARRAY_REF);
         tree_set_value(a, name);

         if (tree_kind(name) == T_REF) {
            tree_change_kind(a, T_FCALL);
            tree_set_ident(a, tree_ident(name));
         }
      }

      copy_trees(params, tree_add_param, a);
      return a;
   }
}

static tree_t bit_str_to_agg(const char *str, const loc_t *loc)
{
   tree_t t = tree_new(T_AGGREGATE);
   tree_set_loc(t, loc);

   char base_ch = str[0];
   int base;
   switch (base_ch) {
   case 'X': case 'x': base = 16; break;
   case 'O': case 'o': base = 8;  break;
   case 'B': case 'b': base = 2;  break;
   default:
      parse_error(loc, "invalid base '%c' for bit string", base_ch);
      return t;
   }

   tree_t one = tree_new(T_REF);
   tree_set_ident(one, ident_new("'1'"));
   tree_set_loc(one, loc);

   tree_t zero = tree_new(T_REF);
   tree_set_ident(zero, ident_new("'0'"));
   tree_set_loc(zero, loc);

   for (const char *p = str + 2; *p != '\"'; p++) {
      if (*p == '_')
         continue;

      int n = (isdigit((int)*p) ? (*p - '0')
               : 10 + (isupper((int)*p) ? (*p - 'A') : (*p - 'a')));

      if (n >= base) {
         parse_error(loc, "invalid digit '%c' in bit string", *p);
         return t;
      }

      for (int d = (base >> 1); d > 0; n = n % d, d >>= 1) {
         tree_t a = tree_new(T_ASSOC);
         tree_set_subkind(a, A_POS);
         tree_set_value(a, (n / d) ? one : zero);

         tree_add_assoc(t, a);
      }
   }

   return t;
}

static bool to_range_expr(tree_t t, range_t *r, bool force)
{
   switch (tree_kind(t)) {
   case T_ATTR_REF:
      {
         ident_t a = tree_ident(t);
         if (icmp(a, "RANGE")) {
            r->kind  = RANGE_EXPR;
            r->left  = t;
            r->right = NULL;

            return true;
         }
         else if (icmp(a, "REVERSE_RANGE")) {
            r->kind  = RANGE_EXPR;
            r->left  = NULL;
            r->right = t;

            return true;
         }
         else
            return false;
      }

   case T_REF:
      if (force) {
         r->kind  = RANGE_EXPR;
         r->left  = t;
         r->right = NULL;
         return true;
      }
      else
         return false;

   default:
      return false;
   }
}

static tree_t get_time(int64_t fs)
{
   tree_t lit = tree_new(T_LITERAL);
   tree_set_subkind(lit, L_INT);
   tree_set_ival(lit, fs);

   tree_t unit = tree_new(T_REF);
   tree_set_ident(unit, ident_new("FS"));

   tree_t f = tree_new(T_FCALL);
   tree_set_ident(f, ident_new("\"*\""));

   tree_t left = tree_new(T_PARAM);
   tree_set_subkind(left, P_POS);
   tree_set_value(left, lit);

   tree_t right = tree_new(T_PARAM);
   tree_set_subkind(right, P_POS);
   tree_set_value(right, unit);

   tree_add_param(f, left);
   tree_add_param(f, right);

   return f;
}

static void add_file_decls(tree_list_t **out, list_t *in, type_t type,
                           tree_t mode, tree_t value, loc_t *loc)
{
   *out = NULL;
   for (list_t *it = in; it != NULL; it = it->next) {
      tree_t t = tree_new(T_FILE_DECL);
      tree_set_ident(t, it->item.ident);
      tree_set_type(t, type);
      tree_set_file_mode(t, mode);
      if (value != NULL)
         tree_set_value(t, value);
      tree_set_loc(t, loc);

      tree_list_append(out, t);
   }

   list_free(in);
}

static tree_t int_to_physical(tree_t t, tree_t unit)
{
   tree_t ref = tree_new(T_REF);
   tree_set_ident(ref, tree_ident(unit));

   tree_t fcall = tree_new(T_FCALL);
   tree_set_loc(fcall, tree_loc(t));
   tree_set_ident(fcall, ident_new("\"*\""));

   tree_t a = tree_new(T_PARAM);
   tree_set_subkind(a, P_POS);
   tree_set_value(a, t);

   tree_t b = tree_new(T_PARAM);
   tree_set_subkind(b, P_POS);
   tree_set_value(b, ref);

   tree_add_param(fcall, a);
   tree_add_param(fcall, b);

   return fcall;
}

static void set_delay_mechanism(tree_t t, tree_t reject)
{
   if (reject == NULL) {
      // Inertial delay with same value as waveform
      // LRM 93 section 8.4 the rejection limit in this case is
      // specified by the time expression of the first waveform
      tree_t w = (tree_kind(t) == T_CASSIGN
                  ? tree_waveform(tree_cond(t, 0), 0)
                  :  tree_waveform(t, 0));
      if (tree_has_delay(w))
         tree_set_reject(t, tree_delay(w));
      else
         tree_set_reject(t, get_time(0));
   }
   else
      tree_set_reject(t, reject);
}

static ident_t loc_to_ident(const loc_t *loc)
{
   int bufsz = 128;
   char *buf = xmalloc(bufsz);
   checked_sprintf(buf, bufsz, "line_%d", loc->first_line);
   const int nprint = strlen(buf);

   for (int i = 0; ident_interned(buf); i++) {
      if (nprint + 1 > bufsz) {
         bufsz *= 2;
         buf = xrealloc(buf, bufsz);
      }
      buf[nprint] = 'a' + i;
      buf[nprint + 1] = '\0';
   }

   ident_t ident = ident_new(buf);
   free(buf);
   return ident;
}

static void yyerror(const char *s)
{
   parse_error(&yylloc, "%s", s);
}

void begin_token(char *tok)
{
   const char *newline = strrchr(tok, '\n');
   int n_token_start, n_token_length;
   if (newline != NULL) {
      n_token_start = 0;
      n_token_length = strlen(tok) - (newline - tok);
      n_token_next_start = n_token_length - 1;
   }
   else {
      n_token_start = n_token_next_start;
      n_token_length = strlen(tok);
      n_token_next_start += n_token_length;
   }

   const int last_col = n_token_start + n_token_length - 1;

   yylloc.first_line   = MIN(n_row, LINE_INVALID);
   yylloc.first_column = MIN(n_token_start, COLUMN_INVALID);
   yylloc.last_line    = MIN(n_row, LINE_INVALID);
   yylloc.last_column  = MIN(last_col, COLUMN_INVALID);
   yylloc.file         = perm_file_name;
   yylloc.linebuf      = perm_linebuf;
}

int get_next_char(char *b, int max_buffer)
{
   if (last_was_newline) {
      n_row += 1;
      perm_linebuf = read_ptr;
      last_was_newline = false;
   }

   const bool eof = read_ptr >= file_start + file_sz;
   if (eof)
      return 0;
   else
      *b = *read_ptr++;

   if (perm_linebuf == NULL)
      perm_linebuf = read_ptr;

   if (*b == '\n')
      last_was_newline = true;

   return *b == 0 ? 0 : 1;
}

void input_from_file(const char *file)
{
   int fd = open(file, O_RDONLY);
   if (fd < 0)
      fatal_errno("opening %s", file);

   struct stat buf;
   if (fstat(fd, &buf) != 0)
      fatal_errno("fstat");

   if (!S_ISREG(buf.st_mode))
      fatal("opening %s: not a regular file", file);

   file_sz = buf.st_size;

   file_start = mmap(NULL, file_sz, PROT_READ, MAP_PRIVATE, fd, 0);
   if (file_start == MAP_FAILED)
      fatal_errno("mmap");

   read_ptr           = file_start;
   last_was_newline   = true;
   perm_file_name     = strdup(file);
   n_row              = 0;
   n_token_next_start = 0;
}

tree_t parse(void)
{
   root = NULL;
   assert_viol = NULL;

   if (yyparse() || (n_errors > 0))
      return NULL;
   else
      return root;
}

int parse_errors(void)
{
   return n_errors;
}
