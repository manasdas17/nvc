#include "phase.h"
#include "common.h"

#include <assert.h>
#include <string.h>

typedef struct driver_list driver_list_t;

struct driver_list {
   driver_list_t *next;
   tree_t         decl;
   uint8_t        netmask[0];
};

#define MASK_ADD(d, n) \
   d->netmask[n / 8] |= (1 << (n % 8))

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

static void driver_expr(tree_t t, driver_list_t *driver,
                        int nnets, int start, int width, int stride)
{
   printf("driver_expr %s width=%d stride=%d\n", tree_kind_str(tree_kind(t)),
          width, stride);

   assert(width <= stride);

   switch (tree_kind(t)) {
   case T_REF:
      for (int i = start; i < nnets; i += stride) {
         for (int j = i; j < i + width; j++)
            MASK_ADD(driver, j);
      }
      break;

   case T_ARRAY_REF:
      {
         const int nparams = tree_params(t);
         assert(nparams == 1);

         tree_t value = tree_value(t);

         const int elem_width = type_width(type_elem(tree_type(value)));

         tree_t idx = tree_value(tree_param(t, 0));
         if (driver_is_const(idx)) {
            const int64_t idx_val = assume_int(idx);
            driver_expr(value, driver, nnets, start + idx_val,
                        elem_width, stride);
         }
         else
            assert(false);
      }
      break;

   default:
      assert(false);
   }
}

static void driver_target(tree_t t, driver_list_t **drivers)
{
   tree_t decl = driver_get_decl(t);

   driver_list_t *d;
   for (d = *drivers; (d != NULL) && (d->decl != decl); d = d->next)
      ;

   const int nnets = tree_nets(decl);

   if (d == NULL) {
      const int maskb = idiv_roundup(nnets, 8);

      d = xmalloc(sizeof(driver_list_t) + maskb);
      d->next = *drivers;
      d->decl = decl;
      memset(d->netmask, '\0', maskb);

      *drivers = d;
   }

   driver_expr(t, d, nnets, 0, nnets, nnets);

#if 0
   tree_t target = tree_target(t);

   tree_t decl = driver_get_decl(target);

   tree_t driver = tree_new(T_DRIVER);
   tree_set_loc(driver, tree_loc(t));
   tree_set_ref(driver, decl);

   tree_t lsp = longest_static_prefix(target);
   if (lsp != NULL)
      tree_set_value(driver, target);

   tree_add_decl(proc, driver);
#endif
}

static void driver_visit_fn(tree_t t, void *arg)
{
   driver_list_t **drivers = arg;

   switch (tree_kind(t)) {
   case T_SIGNAL_ASSIGN:
      driver_target(tree_target(t), drivers);
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
      driver_list_t *drivers = NULL;

      tree_t p = tree_stmt(top, i);
      assert(tree_kind(p) == T_PROCESS);

      tree_visit(p, driver_visit_fn, &drivers);

#if 1
      printf("\n--- Process %s drivers ---\n", istr(tree_ident(p)));

      for (driver_list_t *it = drivers; it != NULL; it = it->next) {
         printf("%-20s ", istr(tree_ident(it->decl)));

         const int maskb = idiv_roundup(tree_nets(it->decl), 8);
         for (int i = maskb - 1; i >= 0; i--)
            printf("%02x", it->netmask[i]);

         printf("\n");
      }
#endif
   }
}
