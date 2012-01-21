//
//  Copyright (C) 2011  Nick Gasson
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

#include <assert.h>
#include <string.h>
#include <stdarg.h>

#define MAX_BUILTIN_ARGS 2

static ident_t std_bool_i = NULL;
static ident_t builtin_i  = NULL;

static int errors = 0;

#define simp_error(t, ...) \
   { errors++; error_at(tree_loc(t), __VA_ARGS__); return t; }

static tree_t simp_tree(tree_t t, void *context);

static bool folded_num(tree_t t, literal_t *l)
{
   if (tree_kind(t) == T_LITERAL) {
      *l = tree_literal(t);
      return true;
   }
   else
      return false;
}

static bool folded_bool(tree_t t, bool *b)
{
   if (tree_kind(t) == T_REF) {
      tree_t decl = tree_ref(t);
      if (tree_kind(decl) == T_ENUM_LIT
          && type_ident(tree_type(decl)) == std_bool_i) {
         *b = (tree_pos(decl) == 1);
         return true;
      }
   }

   return false;
}

static tree_t get_int_lit(tree_t t, int64_t i)
{
   tree_t fdecl = tree_ref(t);
   assert(tree_kind(fdecl) == T_FUNC_DECL);

   literal_t l;
   l.kind = L_INT;
   l.i = i;

   tree_t f = tree_new(T_LITERAL);
   tree_set_loc(f, tree_loc(t));
   tree_set_literal(f, l);
   tree_set_type(f, tree_type(t));

   return f;
}

static tree_t get_bool_lit(tree_t t, bool v)
{
   tree_t fdecl = tree_ref(t);
   assert(tree_kind(fdecl) == T_FUNC_DECL);

   type_t std_bool = type_result(tree_type(fdecl));

   assert(type_ident(std_bool) == std_bool_i);
   assert(type_enum_literals(std_bool) == 2);

   tree_t lit = type_enum_literal(std_bool, v ? 1 : 0);

   tree_t b = tree_new(T_REF);
   tree_set_loc(b, tree_loc(t));
   tree_set_ref(b, lit);
   tree_set_type(b, std_bool);
   tree_set_ident(b, tree_ident(lit));

   return b;
}

static tree_t simp_fcall_log(tree_t t, const char *builtin, bool *args)
{
   if (strcmp(builtin, "not") == 0)
      return get_bool_lit(t, !args[0]);
   else if (strcmp(builtin, "and") == 0)
      return get_bool_lit(t, args[0] && args[1]);
   else if (strcmp(builtin, "nand") == 0)
      return get_bool_lit(t, !(args[0] && args[1]));
   else if (strcmp(builtin, "or") == 0)
      return get_bool_lit(t, args[0] || args[1]);
   else if (strcmp(builtin, "nor") == 0)
      return get_bool_lit(t, !(args[0] || args[1]));
   else if (strcmp(builtin, "xor") == 0)
      return get_bool_lit(t, args[0] ^ args[1]);
   else if (strcmp(builtin, "xnor") == 0)
      return get_bool_lit(t, !(args[0] ^ args[1]));
   else
      return t;
}

static tree_t simp_fcall_num(tree_t t, const char *builtin, literal_t *args)
{
   const int lkind = args[0].kind;  // Assume all types checked same
   assert(lkind == L_INT);

   if (strcmp(builtin, "mul") == 0) {
      return get_int_lit(t, args[0].i * args[1].i);
   }
   else if (strcmp(builtin, "div") == 0) {
      return get_int_lit(t, args[0].i / args[1].i);
   }
   else if (strcmp(builtin, "add") == 0) {
      return get_int_lit(t, args[0].i + args[1].i);
   }
   else if (strcmp(builtin, "sub") == 0) {
      return get_int_lit(t, args[0].i - args[1].i);
   }
   else if (strcmp(builtin, "neg") == 0) {
      return get_int_lit(t, -args[0].i);
   }
   else if (strcmp(builtin, "identity") == 0) {
      return get_int_lit(t, args[0].i);
   }
   else if (strcmp(builtin, "eq") == 0) {
      if (args[0].kind == L_INT && args[1].kind == L_INT)
         return get_bool_lit(t, args[0].i == args[1].i);
      else
         assert(false);
   }
   else if (strcmp(builtin, "neq") == 0) {
      if (args[0].kind == L_INT && args[1].kind == L_INT)
         return get_bool_lit(t, args[0].i != args[1].i);
      else
         assert(false);
   }
   else if (strcmp(builtin, "gt") == 0) {
      if (args[0].kind == L_INT && args[1].kind == L_INT)
         return get_bool_lit(t, args[0].i > args[1].i);
      else
         assert(false);
   }
   else if (strcmp(builtin, "lt") == 0) {
      if (args[0].kind == L_INT && args[1].kind == L_INT)
         return get_bool_lit(t, args[0].i < args[1].i);
      else
         assert(false);
   }
   else
      return t;
}

static tree_t simp_fcall(tree_t t)
{
   tree_t decl = tree_ref(t);
   assert(tree_kind(decl) == T_FUNC_DECL
          || tree_kind(decl) == T_FUNC_BODY);

   const char *builtin = tree_attr_str(decl, builtin_i);
   if (builtin == NULL)
      return t;     // TODO: expand pure function calls

   if (tree_params(t) > MAX_BUILTIN_ARGS)
      return t;

   bool can_fold_num = true;
   bool can_fold_log = true;
   literal_t largs[MAX_BUILTIN_ARGS];
   bool bargs[MAX_BUILTIN_ARGS];
   for (unsigned i = 0; i < tree_params(t); i++) {
      param_t p = tree_param(t, i);
      assert(p.kind == P_POS);
      can_fold_num = can_fold_num && folded_num(p.value, &largs[i]);
      can_fold_log = can_fold_log && folded_bool(p.value, &bargs[i]);
   }

   if (can_fold_num)
      return simp_fcall_num(t, builtin, largs);
   else if (can_fold_log)
      return simp_fcall_log(t, builtin, bargs);
   else
      return t;
}

static tree_t simp_call_builtin(const char *name, const char *builtin,
                                type_t type, ...)
{
   ident_t name_i = ident_new(name);

   tree_t decl = tree_new(T_FUNC_DECL);
   tree_set_ident(decl, name_i);
   tree_add_attr_str(decl, ident_new("builtin"), builtin);

   tree_t call = tree_new(T_FCALL);
   tree_set_ident(call, name_i);
   tree_set_ref(call, decl);
   if (type != NULL)
      tree_set_type(call, type);

   va_list ap;
   va_start(ap, type);
   tree_t arg;
   while ((arg = va_arg(ap, tree_t))) {
      param_t p = { .kind = P_POS, .value = arg };
      tree_add_param(call, p);
   }
   va_end(ap);

   return call;
}

static tree_t simp_ref(tree_t t)
{
   tree_t decl = tree_ref(t);

   switch (tree_kind(decl)) {
   case T_CONST_DECL:
      if (type_kind(tree_type(decl)) == T_PHYSICAL) {
         // Slight hack to constant-fold the definitions of
         // physical units that and generated during the sem phase
         return tree_rewrite(tree_value(decl), simp_tree, NULL);
      }
      else
         return tree_value(decl);
   default:
      return t;
   }
}

static tree_t simp_attr_ref(tree_t t)
{
   if (tree_has_value(t))
      return tree_value(t);
   else {
      tree_t decl = tree_ref(t);
      assert(tree_kind(decl) == T_FUNC_DECL);

      const char *builtin = tree_attr_str(decl, builtin_i);
      assert(builtin != NULL);

      if (strcmp(builtin, "length") == 0) {
         tree_t array = tree_param(t, 0).value;
         if (type_kind(tree_type(array)) == T_CARRAY) {
            range_t r = type_dim(tree_type(array), 0);
            if (tree_kind(r.left) == T_LITERAL
                && tree_kind(r.right) == T_LITERAL) {
               int64_t low, high;
               range_bounds(type_dim(tree_type(array), 0), &low, &high);
               return get_int_lit(t, high - low + 1);
            }
         }
      }

      // Convert attributes like 'EVENT to function calls
      tree_t fcall = tree_new(T_FCALL);
      tree_set_loc(fcall, tree_loc(t));
      tree_set_type(fcall, tree_type(t));
      tree_set_ident(fcall, tree_ident2(t));
      tree_set_ref(fcall, decl);

      for (unsigned i = 0; i < tree_params(t); i++)
         tree_add_param(fcall, tree_param(t, i));

      return fcall;
   }
}

static tree_t simp_array_ref(tree_t t)
{
   tree_t decl = tree_ref(t);
   // XXX: may not be decl e.g. nested array ref

   if (tree_kind(decl) == T_ALIAS) {
      // Generate code to map from alias indexing to the
      // indexing of the underlying array

      tree_t base_decl = tree_ref(tree_value(decl));
      assert(tree_kind(base_decl) != T_ALIAS);

      type_t alias_type = tree_type(decl);
      type_t base_type  = tree_type(base_decl);

      tree_t new = tree_new(T_ARRAY_REF);
      tree_set_loc(new, tree_loc(t));
      tree_set_ref(new, base_decl);
      tree_set_type(new, type_base(base_type));

      assert(type_kind(alias_type) == T_CARRAY);
      assert(type_dims(alias_type) == 1);  // TODO: multi-dimensional arrays

      range_t alias_r = type_dim(alias_type, 0);

      param_t p = tree_param(t, 0);
      type_t ptype = tree_type(p.value);

      tree_t off = simp_call_builtin(
         "-", "sub", ptype, p.value, alias_r.left, NULL);

      switch (type_kind(base_type)) {
      case T_CARRAY:
         // The transformation is a constant offset of indices
         {
            range_t base_r  = type_dim(base_type, 0);
            if (alias_r.kind == base_r.kind) {
               // Range in same direction
               p.value = simp_call_builtin(
                  "+", "add", ptype, base_r.left, off, NULL);
            }
            else {
               // Range in opposite direction
               p.value = simp_call_builtin(
                  "-", "sub", ptype, base_r.left, off, NULL);
            }
         }
         break;

      case T_UARRAY:
         // The transformation must be computed at runtime
         {
            tree_t ref = tree_new(T_REF);
            tree_set_ref(ref, base_decl);
            tree_set_ident(ref, tree_ident(base_decl));
            tree_set_type(ref, ptype);

            tree_t base_left = simp_call_builtin(
               "LEFT", "uarray_left", ptype, ref, NULL);

            literal_t l;
            l.kind = L_INT;
            l.i = alias_r.kind;

            tree_t rkind_lit = tree_new(T_LITERAL);
            tree_set_literal(rkind_lit, l);
            tree_set_type(rkind_lit, ptype);

            // Call dircmp builtin which multiplies its third argument
            // by -1 if the direction of the first argument is not equal
            // to the direction of the second
            tree_t off_dir = simp_call_builtin(
               "NVC.BUILTIN.DIRCMP", "uarray_dircmp", ptype,
               ref, rkind_lit, off, NULL);

            p.value = simp_call_builtin(
               "+", "add", ptype, base_left, off_dir, NULL);
         }
         break;

      default:
         assert(false);
      }

      tree_add_param(new, p);

      return new;
   }

   literal_t indexes[tree_params(t)];
   bool can_fold = true;
   for (unsigned i = 0; i < tree_params(t); i++) {
      param_t p = tree_param(t, i);
      assert(p.kind == P_POS);
      can_fold = can_fold && folded_num(p.value, &indexes[i]);
   }

   if (!can_fold)
      return t;

   if (tree_params(t) > 1)
      return t;  // TODO: constant folding for multi-dimensional arrays

   switch (tree_kind(decl)) {
   case T_CONST_DECL:
      {
         tree_t v = tree_value(decl);
         assert(tree_kind(v) == T_AGGREGATE);
         assert(indexes[0].kind == L_INT);

         range_t bounds = type_dim(tree_type(decl), 0);
         int64_t left = assume_int(bounds.left);
         int64_t right = assume_int(bounds.right);

         if (indexes[0].i < left || indexes[0].i > right)
            simp_error(t, "array reference out of bounds");

         for (unsigned i = 0; i < tree_assocs(v); i++) {
            assoc_t a = tree_assoc(v, i);
            switch (a.kind) {
            case A_POS:
               if (a.pos + left == indexes[0].i)
                  return a.value;
               break;

            case A_OTHERS:
               return a.value;

            case A_RANGE:
               if ((indexes[0].i >= assume_int(a.range.left))
                   && (indexes[0].i <= assume_int(a.range.right)))
                  return a.value;
               break;

            case A_NAMED:
               if (assume_int(a.name) == indexes[0].i)
                  return a.value;
               break;
            }
         }

         assert(false);
      }
   default:
      return t;
   }
}

static tree_t simp_process(tree_t t)
{
   // Replace sensitivity list with a "wait on" statement
   if (tree_triggers(t) > 0) {
      tree_t p = tree_new(T_PROCESS);
      tree_set_ident(p, tree_ident(t));
      tree_set_loc(p, tree_loc(t));

      for (unsigned i = 0; i < tree_decls(t); i++)
         tree_add_decl(p, tree_decl(t, i));

      for (unsigned i = 0; i < tree_stmts(t); i++)
         tree_add_stmt(p, tree_stmt(t, i));

      tree_t w = tree_new(T_WAIT);
      tree_set_loc(w, tree_loc(t));
      tree_set_ident(w, tree_ident(p));
      for (unsigned i = 0; i < tree_triggers(t); i++)
         tree_add_trigger(w, tree_trigger(t, i));
      tree_add_stmt(p, w);

      return p;
   }
   else
      return t;
}

static tree_t simp_if(tree_t t)
{
   bool value_b;
   if (folded_bool(tree_value(t), &value_b)) {
      if (value_b) {
         // If statement always executes so replace with then part
         if (tree_stmts(t) == 1)
            return tree_stmt(t, 0);
         else {
            tree_t b = tree_new(T_BLOCK);
            tree_set_ident(b, tree_ident(t));
            for (unsigned i = 0; i < tree_stmts(t); i++)
               tree_add_stmt(b, tree_stmt(t, i));
            return b;
         }
      }
      else {
         // If statement never executes so replace with else part
         if (tree_else_stmts(t) == 1)
            return tree_else_stmt(t, 0);
         else if (tree_else_stmts(t) == 0)
            return NULL;   // Delete it
         else {
            tree_t b = tree_new(T_BLOCK);
            tree_set_ident(b, tree_ident(t));
            for (unsigned i = 0; i < tree_else_stmts(t); i++)
               tree_add_stmt(b, tree_else_stmt(t, i));
            return b;
         }
      }
   }
   else
      return t;
}

static tree_t simp_while(tree_t t)
{
   bool value_b;
   if (!tree_has_value(t))
      return t;
   else if (folded_bool(tree_value(t), &value_b) && !value_b) {
      // Condition is false so loop never executes
      return NULL;
   }
   else
      return t;
}

static tree_t simp_for(tree_t t)
{
   tree_t b = tree_new(T_BLOCK);
   tree_set_ident(b, tree_ident(t));

   for (unsigned i = 0; i < tree_decls(t); i++)
      tree_add_decl(b, tree_decl(t, i));

   tree_t decl = tree_decl(t, 0);

   tree_t var = tree_new(T_REF);
   tree_set_ident(var, tree_ident(decl));
   tree_set_type(var, tree_type(decl));
   tree_set_ref(var, decl);

   range_t r = tree_range(t);

   tree_t init = tree_new(T_VAR_ASSIGN);
   tree_set_ident(init, ident_uniq("init"));
   tree_set_target(init, var);
   tree_set_value(init, r.left);

   tree_t wh = tree_new(T_WHILE);
   tree_set_ident(wh, ident_uniq("loop"));

   for (unsigned i = 0; i < tree_stmts(t); i++)
      tree_add_stmt(wh, tree_stmt(t, i));

   tree_t eq = tree_new(T_FUNC_DECL);
   tree_set_ident(eq, ident_new("="));
   tree_add_attr_str(eq, ident_new("builtin"), "eq");

   tree_t cmp = simp_call_builtin("=", "eq", NULL, var, r.right, NULL);

   tree_t exit = tree_new(T_EXIT);
   tree_set_ident(exit, ident_uniq("for_exit"));
   tree_set_value(exit, cmp);

   tree_t next;
   if (r.kind == RANGE_DYN) {
      assert(tree_kind(r.left) == T_FCALL);
      param_t p = tree_param(r.left, 0);

      tree_t asc = simp_call_builtin("NVC.BUILTIN.ASCENDING", "uarray_asc",
                                     NULL, p.value, NULL);
      next = tree_new(T_IF);
      tree_set_value(next, asc);
      tree_set_ident(next, ident_uniq("for_next"));

      tree_t succ_call = simp_call_builtin("NVC.BUILTIN.SUCC", "succ",
                                           tree_type(decl), var, NULL);

      tree_t a1 = tree_new(T_VAR_ASSIGN);
      tree_set_ident(a1, ident_uniq("for_next_asc"));
      tree_set_target(a1, var);
      tree_set_value(a1, succ_call);

      tree_t pred_call = simp_call_builtin("NVC.BUILTIN.PRED", "pred",
                                           tree_type(decl), var, NULL);

      tree_t a2 = tree_new(T_VAR_ASSIGN);
      tree_set_ident(a2, ident_uniq("for_next_dsc"));
      tree_set_target(a2, var);
      tree_set_value(a2, pred_call);

      tree_add_stmt(next, a1);
      tree_add_else_stmt(next, a2);
   }
   else {
      tree_t call;
      switch (r.kind) {
      case RANGE_TO:
         call = simp_call_builtin("NVC.BUILTIN.SUCC", "succ",
                                  tree_type(decl), var, NULL);
         break;
      case RANGE_DOWNTO:
         call = simp_call_builtin("NVC.BUILTIN.PRED", "pred",
                                  tree_type(decl), var, NULL);
         break;
      default:
         assert(false);
      }

      next = tree_new(T_VAR_ASSIGN);
      tree_set_ident(next, ident_uniq("for_next"));
      tree_set_target(next, var);
      tree_set_value(next, call);
   }

   tree_add_stmt(wh, exit);
   tree_add_stmt(wh, next);

   tree_add_stmt(b, init);
   tree_add_stmt(b, wh);

   return b;
}

static void simp_build_wait(tree_t ref, void *context)
{
   tree_t wait = context;

   if (tree_kind(tree_ref(ref)) == T_SIGNAL_DECL)
      tree_add_trigger(wait, ref);
}

static tree_t simp_cassign(tree_t t)
{
   // Replace concurrent assignments with a process

   tree_t p = tree_new(T_PROCESS);
   tree_set_ident(p, tree_ident(t));

   tree_t w = tree_new(T_WAIT);
   tree_set_ident(w, ident_new("cassign"));

   tree_t s = tree_new(T_SIGNAL_ASSIGN);
   tree_set_loc(s, tree_loc(t));
   tree_set_target(s, tree_target(t));
   tree_set_ident(s, tree_ident(t));

   for (unsigned i = 0; i < tree_waveforms(t); i++) {
      tree_t wave = tree_waveform(t, i);
      tree_add_waveform(s, wave);
      tree_visit_only(wave, simp_build_wait, w, T_REF);
   }

   tree_add_stmt(p, s);
   tree_add_stmt(p, w);
   return p;
}

static tree_t simp_tree(tree_t t, void *context)
{
   switch (tree_kind(t)) {
   case T_PROCESS:
      return simp_process(t);
   case T_ARRAY_REF:
      return simp_array_ref(t);
   case T_ATTR_REF:
      return simp_attr_ref(t);
   case T_FCALL:
      return simp_fcall(t);
   case T_REF:
      return simp_ref(t);
   case T_IF:
      return simp_if(t);
   case T_WHILE:
      return simp_while(t);
   case T_FOR:
      return simp_for(t);
   case T_CASSIGN:
      return simp_cassign(t);
   case T_NULL:
      return NULL;   // Delete it
   default:
      return t;
   }
}

static void simp_intern_strings(void)
{
   // Intern some commponly used strings

   std_bool_i = ident_new("STD.STANDARD.BOOLEAN");
   builtin_i  = ident_new("builtin");
}

void simplify(tree_t top)
{
   static bool have_interned = false;
   if (!have_interned) {
      simp_intern_strings();
      have_interned = true;
   }

   tree_rewrite(top, simp_tree, NULL);
}

int simplify_errors(void)
{
   return errors;
}
