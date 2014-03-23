#include "phase.h"

#include <assert.h>

static tree_t driver_get_decl(tree_t t)
{
   switch (tree_kind(t)) {
   case T_REF:
      return tree_ref(t);
   default:
      assert(false);
   }
}

static tree_t driver_longest_static_prefix(tree_t t)
{
   // Longest static prefixes are in LRM 93 section 6.1

   switch (tree_kind(t)) {
   case T_REF:
      return NULL;
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

   tree_t lsp = driver_longest_static_prefix(target);
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
