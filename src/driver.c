#include "phase.h"

#include <assert.h>

static tree_t driver_get_decl(tree_t t)
{
   switch (tree_kind(t)) {
   case T_REF:
      return tree_ref(t);
   case T_ARRAY_REF:
   case T_ARRAY_SLICE:
      return driver_get_decl(tree_value(t));
   default:
      assert(false);
   }
}

static bool driver_is_const(tree_t t)
{
   switch (tree_kind(t)) {
   case T_LITERAL:
      return true;
   case T_REF:
      {
         tree_t decl = tree_ref(t);
         return (tree_kind(decl) == T_ENUM_LIT);
      }
   default:
      return false;
   }
}

static tree_t longest_static_prefix(tree_t t)
{
   // Longest static prefixes are in LRM 93 section 6.1

   switch (tree_kind(t)) {
   case T_REF:
      return NULL;
   case T_ARRAY_REF:
      {
         tree_t value = tree_value(t);
         if ((tree_kind(value) != T_REF)
             && (longest_static_prefix(value) == NULL))
            return NULL;

         const int nparams = tree_params(t);
         for (int i = 0; i < nparams; i++) {
            tree_t p = tree_param(t, i);
            if (!driver_is_const(tree_value(p)))
               return NULL;
         }

         return t;
      }
   default:
      assert(false);
   }
}

static void driver_signal_assign(tree_t t, tree_t proc)
{
   fmt_loc(stdout, tree_loc(t));

   tree_t target = tree_target(t);

   tree_t decl = driver_get_decl(target);

   tree_t driver = tree_new(T_DRIVER);
   tree_set_loc(driver, tree_loc(t));
   tree_set_ref(driver, decl);

   tree_t lsp = longest_static_prefix(target);
   if (lsp != NULL)
      tree_set_value(driver, target);

   tree_add_decl(proc, driver);
}

static void driver_visit_fn(tree_t t, void *arg)
{
   tree_t proc = arg;

   switch (tree_kind(t)) {
   case T_SIGNAL_ASSIGN:
      driver_signal_assign(t, proc);
      break;
   case T_PCALL:
      assert(false);
      break;
   default:
      break;
   }
}

void driver_extract(tree_t top)
{
   const int nstmts = tree_stmts(top);
   for (int i = 0; i < nstmts; i++) {
      tree_t p = tree_stmt(top, i);
      assert(tree_kind(p) == T_PROCESS);

      tree_visit(p, driver_visit_fn, p);
   }
}
