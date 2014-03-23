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

#include "phase.h"
#include "util.h"
#include "hash.h"

#include <assert.h>
#include <string.h>
#include <ctype.h>
#include <inttypes.h>

static void dump_expr(tree_t t);
static void dump_stmt(tree_t t, int indent);
static void dump_port(tree_t t, int indent);

typedef tree_t (*get_fn_t)(tree_t, unsigned);

static void tab(int indent)
{
   while (indent--)
      fputc(' ', stdout);
}

static void cannot_dump(tree_t t, const char *hint)
{
   printf("\n");
   fflush(stdout);
   fatal("cannot dump %s kind %s", hint, tree_kind_str(tree_kind(t)));
}

static void dump_params(tree_t t, get_fn_t get, int n, const char *prefix)
{
   if (n > 0) {
      if (prefix != NULL)
         printf("%s ", prefix);
      printf("(");
      for (int i = 0; i < n; i++) {
         if (i > 0)
            printf(", ");
         tree_t p = (*get)(t, i);
         switch (tree_subkind(p)) {
         case P_POS:
            break;
         case P_NAMED:
            dump_expr(tree_name(p));
            printf(" => ");
            break;
         }
         dump_expr(tree_value(p));
      }
      printf(")");
   }
}

static void dump_range(range_t r)
{
   dump_expr(r.left);
   switch (r.kind) {
   case RANGE_TO:
      printf(" to "); break;
   case RANGE_DOWNTO:
      printf(" downto "); break;
   case RANGE_DYN:
      printf(" dynamic "); break;
   case RANGE_RDYN:
      printf(" reverse_dynamic "); break;
   default:
      assert(false);
   }
   dump_expr(r.right);
}

static void dump_expr(tree_t t)
{
   switch (tree_kind(t)) {
   case T_FCALL:
      printf("%s", istr(tree_ident(tree_ref(t))));
      dump_params(t, tree_param, tree_params(t), NULL);
      break;

   case T_LITERAL:
      switch (tree_subkind(t)) {
      case L_INT:
         printf("%"PRIi64, tree_ival(t));
         break;
      case L_REAL:
         printf("%lf", tree_dval(t));
         break;
      case L_NULL:
         printf("null");
         break;
      default:
         assert(false);
      }
      break;

   case T_NEW:
      printf("new ");
      dump_expr(tree_value(t));
      break;

   case T_ALL:
      dump_expr(tree_value(t));
      printf(".all");
      break;

   case T_AGGREGATE:
      printf("(");
      for (unsigned i = 0; i < tree_assocs(t); i++) {
         if (i > 0)
            printf(", ");
         tree_t a = tree_assoc(t, i);
         tree_t value = tree_value(a);
         switch (tree_subkind(a)) {
         case A_POS:
            dump_expr(value);
            break;
         case A_NAMED:
            dump_expr(tree_name(a));
            printf(" => ");
            dump_expr(value);
            break;
         case A_OTHERS:
            printf("others => ");
            dump_expr(value);
            break;
         case A_RANGE:
            dump_range(tree_range(a));
            printf(" => ");
            dump_expr(value);
            break;
         default:
            assert(false);
         }
      }
      printf(")");
      break;

   case T_REF:
      printf("%s", istr(tree_ident(tree_ref(t))));
      break;

   case T_ATTR_REF:
      dump_expr(tree_name(t));
      printf("'%s", istr(tree_ident(t)));
      break;

   case T_ARRAY_REF:
      dump_expr(tree_value(t));
      dump_params(t, tree_param, tree_params(t), NULL);
      break;

   case T_ARRAY_SLICE:
      dump_expr(tree_value(t));
      printf("(");
      dump_range(tree_range(t));
      printf(")");
      break;

   case T_RECORD_REF:
      dump_expr(tree_value(t));
      printf(".%s", istr(tree_ident(t)));
      break;

   case T_TYPE_CONV:
      printf("%s(", istr(tree_ident(tree_ref(t))));
      dump_expr(tree_value(tree_param(t, 0)));
      printf(")");
      break;

   case T_CONCAT:
      printf("(");
      dump_expr(tree_value(tree_param(t, 0)));
      printf(" & ");
      dump_expr(tree_value(tree_param(t, 1)));
      printf(")");
      break;

   case T_QUALIFIED:
      printf("%s'(", istr(type_ident(tree_type(t))));
      dump_expr(tree_value(t));
      printf(")");
      break;

   case T_OPEN:
      printf("open");
      break;

   default:
      cannot_dump(t, "expr");
   }
}

static void dump_type(type_t type)
{
   if (type_is_array(type) && (type_kind(type) != T_UARRAY)) {
      printf("%s(", istr(type_ident(type)));
      for (unsigned i = 0; i < type_dims(type); i++) {
         if (i > 0)
            printf(", ");
         range_t r = type_dim(type, i);
         dump_expr(r.left);
         switch (r.kind) {
         case RANGE_TO:
            printf(" to ");
            dump_expr(r.right);
            break;
         case RANGE_DOWNTO:
            printf(" downto ");
            dump_expr(r.right);
            break;
         case RANGE_DYN:
            printf(" dynamic ");
            dump_expr(r.right);
            break;
         case RANGE_RDYN:
            printf(" reverse_dynamic ");
            dump_expr(r.right);
            break;
         case RANGE_EXPR:
            break;
         }
      }
      printf(")");
   }
   else
      printf("%s", type_pp(type));
}

static void dump_decl(tree_t t, int indent)
{
   tab(indent);

   switch (tree_kind(t)) {
   case T_SIGNAL_DECL:
      printf("signal %s : ", istr(tree_ident(t)));
      break;

   case T_VAR_DECL:
      printf("variable %s : ", istr(tree_ident(t)));
      break;

   case T_CONST_DECL:
      printf("constant %s : ", istr(tree_ident(t)));
      break;

   case T_TYPE_DECL:
      printf("type %s is ", istr(tree_ident(t)));
      {
         type_t type = tree_type(t);
         if (type_is_integer(type)) {
            printf("range ");
            dump_range(type_dim(type, 0));
         }
         else if (type_kind(type) == T_PHYSICAL) {
            printf("range ");
            dump_range(type_dim(type, 0));
            printf("\n");
            tab(indent + 2);
            printf("units\n");
            {
               const int nunits = type_units(type);
               for (int i = 0; i < nunits; i++) {
                  tree_t u = type_unit(type, i);
                  tab(indent + 4);
                  printf("%s = ", istr(tree_ident(u)));
                  dump_expr(tree_value(u));
                  printf(";\n");
               }
            }
            tab(indent + 2);
            printf("end units\n");
         }
         else if (type_is_array(type)) {
            printf("array (");
            if (type_kind(type) == T_UARRAY) {
               const int nindex = type_index_constrs(type);
               for (int i = 0; i < nindex; i++) {
                  if (i > 0) printf(", ");
                  dump_type(type_index_constr(type, i));
                  printf(" range <>");
               }
            }
            else {
               const int ndims = type_dims(type);
               for (int i = 0; i < ndims; i++) {
                  if (i > 0) printf(", ");
                  dump_range(type_dim(type, i));
               }
            }
            printf(") of ");
            dump_type(type_elem(type));
         }
         else
            dump_type(type);
      }
      printf(";\n");
      return;

   case T_ALIAS:
      printf("alias %s : ", istr(tree_ident(t)));
      dump_type(tree_type(t));
      printf(" is ");
      dump_expr(tree_value(t));
      printf(";\n");
      return;

   case T_ATTR_SPEC:
      printf("TODO: T_ATTR_SPEC\n");
      return;

   case T_ATTR_DECL:
      printf("TODO: T_ATTR_DECL\n");
      return;

   case T_FUNC_DECL:
      printf("function %s ", istr(tree_ident(t)));
      {
         const int nports = tree_ports(t);
         if (nports > 0) {
            printf("(\n");
            for (int i = 0; i < nports; i++) {
               if (i > 0)
                  printf(";\n");
               dump_port(tree_port(t, i), indent + 4);
            }
            printf(" )\n");
            tab(indent + 2);
         }
      }
      printf("return %s;\n", type_pp(type_result(tree_type(t))));
      return;

   case T_FUNC_BODY:
      printf("function %s (\n", istr(tree_ident(t)));
      for (unsigned i = 0; i < tree_ports(t); i++) {
         if (i > 0)
            printf(";\n");
         dump_port(tree_port(t, i), indent + 4);
      }
      printf(" )\n");
      tab(indent + 2);
      printf("return %s is\n", type_pp(type_result(tree_type(t))));
      for (unsigned i = 0; i < tree_decls(t); i++)
         dump_decl(tree_decl(t, i), indent + 2);
      tab(indent);
      printf("begin\n");
      for (unsigned i = 0; i < tree_stmts(t); i++)
         dump_stmt(tree_stmt(t, i), indent + 2);
      tab(indent);
      printf("end function;\n\n");
      return;

   case T_PROC_DECL:
      printf("procedure %s (\n", istr(tree_ident(t)));
      for (unsigned i = 0; i < tree_ports(t); i++) {
         if (i > 0)
            printf(";\n");
         dump_port(tree_port(t, i), indent + 4);
      }
      printf(" );\n");
      return;

   case T_PROC_BODY:
      printf("procedure %s (\n", istr(tree_ident(t)));
      for (unsigned i = 0; i < tree_ports(t); i++) {
         if (i > 0)
            printf(";\n");
         dump_port(tree_port(t, i), indent + 4);
      }
      printf(" ) is\n");
      for (unsigned i = 0; i < tree_decls(t); i++)
         dump_decl(tree_decl(t, i), indent + 2);
      tab(indent);
      printf("begin\n");
      for (unsigned i = 0; i < tree_stmts(t); i++)
         dump_stmt(tree_stmt(t, i), indent + 2);
      tab(indent);
      printf("end procedure;\n\n");
      return;

   case T_HIER:
      printf("-- Enter scope %s\n", istr(tree_ident(t)));
      return;

   case T_DRIVER:
      printf("-- Driver for %s ", istr(tree_ident(tree_ref(t))));
      if (tree_has_value(t)) {
         dump_expr(tree_value(t));
         printf("\n");
      }
      else
         printf("(full signal)\n");
      return;

   case T_COMPONENT:
      printf("component %s is\n", istr(tree_ident(t)));
      if (tree_generics(t) > 0) {
         printf("    generic (\n");
         for (unsigned i = 0; i < tree_generics(t); i++) {
            if (i > 0)
               printf(";\n");
            tab(4);
            dump_port(tree_generic(t, i), 2);
         }
         printf(" );\n");
      }
      printf("    port (\n");
      for (unsigned i = 0; i < tree_ports(t); i++) {
         if (i > 0)
            printf(";\n");
         tab(4);
         dump_port(tree_port(t, i), 2);
      }
      printf(" );\n");
      printf("  end component;\n");
      return;

   default:
      cannot_dump(t, "decl");
   }

   dump_type(tree_type(t));

   if (tree_has_value(t)) {
      printf(" := ");
      dump_expr(tree_value(t));
   }
   printf(";");

   if (tree_attr_int(t, ident_new("returned"), 0))
      printf(" -- returned");

   printf("\n");

}

static void dump_stmt(tree_t t, int indent)
{
   tab(indent);
   printf("%s: ", istr(tree_ident(t)));

   switch (tree_kind(t)) {
   case T_PROCESS:
      printf("process ");
      if (tree_triggers(t) > 0) {
         printf("(");
         for (unsigned i = 0; i < tree_triggers(t); i++) {
            if (i > 0)
               printf(", ");
            dump_expr(tree_trigger(t, i));
         }
         printf(") ");
      }
      printf("is\n");
      for (unsigned i = 0; i < tree_decls(t); i++)
         dump_decl(tree_decl(t, i), indent + 2);
      tab(indent);
      printf("begin\n");
      for (unsigned i = 0; i < tree_stmts(t); i++)
         dump_stmt(tree_stmt(t, i), indent + 2);
      tab(indent);
      printf("end process;\n\n");
      return;

   case T_SIGNAL_ASSIGN:
      dump_expr(tree_target(t));
      printf(" <= reject ");
      dump_expr(tree_reject(t));
      printf(" inertial ");
      for (unsigned i = 0; i < tree_waveforms(t); i++) {
         if (i > 0)
            printf(", ");
         tree_t w = tree_waveform(t, i);
         dump_expr(tree_value(w));
         if (tree_has_delay(w)) {
            printf(" after ");
            dump_expr(tree_delay(w));
         }
      }
      break;

   case T_VAR_ASSIGN:
      dump_expr(tree_target(t));
      printf(" := ");
      dump_expr(tree_value(t));
      break;

   case T_WAIT:
      printf("wait");
      if (tree_triggers(t) > 0) {
         printf(" on ");
         for (unsigned i = 0; i < tree_triggers(t); i++) {
            if (i > 0)
               printf(", ");
            dump_expr(tree_trigger(t, i));
         }
      }
      if (tree_has_delay(t)) {
         printf(" for ");
         dump_expr(tree_delay(t));
      }
      printf(";");
      if (tree_attr_int(t, ident_new("static"), 0))
         printf("   -- static");
      printf("\n");
      return;

   case T_BLOCK:
      printf("block is\n");
      for (unsigned i = 0; i < tree_decls(t); i++)
         dump_decl(tree_decl(t, i), indent + 2);
      tab(indent);
      printf("begin\n");
      for (unsigned i = 0; i < tree_stmts(t); i++)
         dump_stmt(tree_stmt(t, i), indent + 2);
      tab(indent);
      printf("end block");
      break;

   case T_ASSERT:
      printf("assert ");
      dump_expr(tree_value(t));
      printf(" report ");
      dump_expr(tree_message(t));
      printf(" severity ");
      dump_expr(tree_severity(t));
      break;

   case T_WHILE:
      if (tree_has_value(t)) {
         printf("while ");
         dump_expr(tree_value(t));
         printf(" ");
      }
      printf("loop\n");
      for (unsigned i = 0; i < tree_stmts(t); i++)
         dump_stmt(tree_stmt(t, i), indent + 2);
      tab(indent);
      printf("end loop");
      break;

   case T_IF:
      printf("if ");
      dump_expr(tree_value(t));
      printf(" then\n");
      for (unsigned i = 0; i < tree_stmts(t); i++)
         dump_stmt(tree_stmt(t, i), indent + 2);
      if (tree_else_stmts(t) > 0) {
         tab(indent);
         printf("else\n");
         for (unsigned i = 0; i < tree_else_stmts(t); i++)
            dump_stmt(tree_else_stmt(t, i), indent + 2);
      }
      tab(indent);
      printf("end if");
      break;

   case T_EXIT:
      printf("exit %s", istr(tree_ident2(t)));
      if (tree_has_value(t)) {
         printf(" when ");
         dump_expr(tree_value(t));
      }
      break;

   case T_CASE:
      printf("case ");
      dump_expr(tree_value(t));
      printf(" is\n");
      for (unsigned i = 0; i < tree_assocs(t); i++) {
         tab(indent + 2);
         tree_t a = tree_assoc(t, i);
         switch (tree_subkind(a)) {
         case A_NAMED:
            printf("when ");
            dump_expr(tree_name(a));
            printf(" =>\n");
            break;
         case A_OTHERS:
            printf("when others =>\n");
            break;
         default:
            assert(false);
         }
         dump_stmt(tree_value(a), indent + 4);
      }
      tab(indent);
      printf("end case");
      break;

   case T_RETURN:
      printf("return");
      if (tree_has_value(t)) {
         printf(" ");
         dump_expr(tree_value(t));
      }
      break;

   case T_FOR:
      printf("for %s in ", istr(tree_ident2(t)));
      dump_range(tree_range(t));
      printf(" loop\n");
      for (unsigned i = 0; i < tree_stmts(t); i++)
         dump_stmt(tree_stmt(t, i), indent + 2);
      tab(indent);
      printf("end for");
      break;

   case T_PCALL:
      printf("%s", istr(tree_ident(tree_ref(t))));
      dump_params(t, tree_param, tree_params(t), NULL);
      break;

   case T_FOR_GENERATE:
      printf("for %s in ", istr(tree_ident2(t)));
      dump_range(tree_range(t));
      printf(" generate\n");
      for (unsigned i = 0; i < tree_decls(t); i++)
         dump_decl(tree_decl(t, i), indent + 2);
      tab(indent);
      printf("begin\n");
      for (unsigned i = 0; i < tree_stmts(t); i++)
         dump_stmt(tree_stmt(t, i), indent + 2);
      tab(indent);
      printf("end generate");
      break;

   case T_IF_GENERATE:
      printf("if ");
      dump_expr(tree_value(t));
      printf(" generate\n");
      for (unsigned i = 0; i < tree_decls(t); i++)
         dump_decl(tree_decl(t, i), indent + 2);
      tab(indent);
      printf("begin\n");
      for (unsigned i = 0; i < tree_stmts(t); i++)
         dump_stmt(tree_stmt(t, i), indent + 2);
      tab(indent);
      printf("end generate");
      break;

   case T_INSTANCE:
      switch (tree_class(t)) {
      case C_ENTITY:    printf("entity "); break;
      case C_COMPONENT: printf("component "); break;
      default:
         assert(false);
      }
      printf("%s\n", istr(tree_ident2(t)));
      if (tree_genmaps(t) > 0) {
         tab(indent + 4);
         dump_params(t, tree_genmap, tree_genmaps(t), "generic map");
         printf("\n");
      }
      if (tree_params(t) > 0) {
         tab(indent + 4);
         dump_params(t, tree_param, tree_params(t), "port map");
      }
      printf(";\n\n");
      return;

   default:
      cannot_dump(t, "stmt");
   }

   printf(";\n");
}

static void dump_port(tree_t t, int indent)
{
   tab(indent);
   const char *class = NULL, *dir = NULL;
   switch (tree_class(t)) {
   case C_SIGNAL:   class = "signal";   break;
   case C_VARIABLE: class = "variable"; break;
   case C_DEFAULT:  class = "";         break;
   case C_CONSTANT: class = "constant"; break;
   case C_FILE:     class = "file";     break;
   default:
      assert(false);
   }
   switch (tree_subkind(t)) {
   case PORT_IN:      dir = "in";     break;
   case PORT_OUT:     dir = "out";    break;
   case PORT_INOUT:   dir = "inout";  break;
   case PORT_BUFFER:  dir = "buffer"; break;
   case PORT_INVALID: dir = "??";     break;
   }
   printf("%s %s : %s ", class, istr(tree_ident(t)), dir);
   dump_type(tree_type(t));
}

static void dump_context(tree_t t)
{
   const int nctx = tree_contexts(t);
   for (int i = 0; i < nctx; i++) {
      tree_t c = tree_context(t, i);
      printf("use %s", istr(tree_ident(c)));
      if (tree_has_ident2(c)) {
         printf(".%s", istr(tree_ident2(c)));
      }
      printf(";\n");
   }

   if (nctx > 0)
      printf("\n");
}

static void dump_elab(tree_t t)
{
   dump_context(t);
   printf("entity %s is\nend entity;\n\n", istr(tree_ident(t)));
   printf("architecture elab of %s is\n", istr(tree_ident(t)));
   for (unsigned i = 0; i < tree_decls(t); i++)
      dump_decl(tree_decl(t, i), 2);
   printf("begin\n");
   for (unsigned i = 0; i < tree_stmts(t); i++)
      dump_stmt(tree_stmt(t, i), 2);
   printf("end architecture;\n");
}

static void dump_entity(tree_t t)
{
   dump_context(t);
   printf("entity %s is\n", istr(tree_ident(t)));
   if (tree_generics(t) > 0) {
      printf("  generic (\n");
      for (unsigned i = 0; i < tree_generics(t); i++) {
         if (i > 0)
            printf(";\n");
         tab(4);
         dump_port(tree_generic(t, i), 2);
      }
      printf("  );\n");
   }
   if (tree_ports(t) > 0) {
      printf("  port (\n");
      for (unsigned i = 0; i < tree_ports(t); i++) {
         if (i > 0)
            printf(";\n");
         tab(4);
         dump_port(tree_port(t, i), 2);
      }
      printf("  );\n");
   }
   if (tree_stmts(t) > 0) {
      printf("begin\n");
      for (unsigned i = 0; i < tree_stmts(t); i++) {
         dump_stmt(tree_stmt(t, i), 2);
      }
   }
   printf("end entity;\n");
}

static void dump_arch(tree_t t)
{
   dump_context(t);
   printf("architecture %s of %s is\n",
          istr(tree_ident(t)), istr(tree_ident2(t)));
   for (unsigned i = 0; i < tree_decls(t); i++)
      dump_decl(tree_decl(t, i), 2);
   printf("begin\n");
   for (unsigned i = 0; i < tree_stmts(t); i++)
      dump_stmt(tree_stmt(t, i), 2);
   printf("end architecture;\n");
}

static void dump_package(tree_t t)
{
   dump_context(t);
   printf("package %s is\n", istr(tree_ident(t)));
   for (unsigned i = 0; i < tree_decls(t); i++)
      dump_decl(tree_decl(t, i), 2);
   printf("end package;\n");
}

static void dump_package_body(tree_t t)
{
   dump_context(t);
   printf("package body %s is\n", istr(tree_ident(t)));
   for (unsigned i = 0; i < tree_decls(t); i++)
      dump_decl(tree_decl(t, i), 2);
   printf("end package body;\n");
}

void dump(tree_t t)
{
   switch (tree_kind(t)) {
   case T_ELAB:
      dump_elab(t);
      break;
   case T_ENTITY:
      dump_entity(t);
      break;
   case T_ARCH:
      dump_arch(t);
      break;
   case T_PACKAGE:
      dump_package(t);
      break;
   case T_PACK_BODY:
      dump_package_body(t);
      break;
   case T_FCALL:
   case T_LITERAL:
   case T_AGGREGATE:
   case T_REF:
   case T_ARRAY_REF:
   case T_ARRAY_SLICE:
   case T_TYPE_CONV:
   case T_CONCAT:
      dump_expr(t);
      printf("\n");
      break;
   default:
      cannot_dump(t, "tree");
   }
}

void dump_nets(tree_t top)
{
   hash_t *h = hash_new(2048, false);

   const int ndecls = tree_decls(top);
   for (int i = 0; i < ndecls; i++) {
      tree_t d = tree_decl(top, i);

      if (tree_kind(d) != T_SIGNAL_DECL)
         continue;

      const int nnets = tree_nets(d);
      for (int j = 0; j < nnets; j++)
         hash_put(h, (const void *)(uintptr_t)(tree_net(d, j) + 1), d);
   }

   const int nnets = tree_attr_int(top, ident_new("nnets"), -1);
   assert(nnets != -1);

   for (netid_t n = 0; n < (netid_t)nnets; n++) {
      int tmp, k = 0;
      tree_t d;
      while ((tmp = k++),
             (d = hash_get_nth(h, (const void *)(uintptr_t)(n + 1), &tmp))) {
         if (k == 1)
            printf("%4d\t", n);
         else
            printf(" ");
         printf("%s", istr(tree_ident(d)));

         if (type_is_array(tree_type(d))) {
            const int nnets = tree_nets(d);
            for (int j = 0; j < nnets; j++) {
               if (n == tree_net(d, j)) {
                  printf("[%d]", j);
                  break;
               }
            }
         }
      }

      printf("\n");
   }

   hash_free(h);
}
