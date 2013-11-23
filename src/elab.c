//
//  Copyright (C) 2011-2013  Nick Gasson
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
#include "common.h"
#include "rt/cover.h"

#include <ctype.h>
#include <assert.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
#include <inttypes.h>

#define FUNC_REPLACE_MAX 32

typedef struct {
   tree_t   out;
   ident_t  path;    // Current 'PATH_NAME
   ident_t  inst;    // Current 'INSTANCE_NAME
   netid_t *next_net;
} elab_ctx_t;

typedef struct {
   tree_t formal;
   tree_t actual;
} rewrite_params_t;

typedef struct map_list {
   struct map_list *next;
   tree_t formal;
   tree_t actual;
   tree_t signal;
   tree_t name;
} map_list_t;

typedef struct copy_list {
   struct copy_list *next;
   tree_t tree;
} copy_list_t;

static void elab_arch(tree_t t, const elab_ctx_t *ctx);
static void elab_block(tree_t t, const elab_ctx_t *ctx);
static void elab_stmts(tree_t t, const elab_ctx_t *ctx);
static void elab_funcs(tree_t arch, tree_t ent);

static int     errors = 0;
static ident_t inst_name_i;
static ident_t fst_dir_i;
static ident_t scope_pop_i;

static ident_t hpathf(ident_t path, char sep, const char *fmt, ...)
{
   va_list ap;
   char buf[256];
   va_start(ap, fmt);
   vsnprintf(buf, sizeof(buf), fmt, ap);
   va_end(ap);

   // LRM specifies instance path is lowercase
   char *p = buf;
   while (*p != '\0') {
      *p = tolower((uint8_t)*p);
      ++p;
   }

   return ident_prefix(path, ident_new(buf), sep);
}

static const char *simple_name(const char *full)
{
   // Strip off any library or entity prefix from the parameter
   const char *start = full;
   for (const char *p = full; *p != '\0'; p++) {
      if (*p == '.' || *p == '-')
         start = p + 1;
   }

   return start;
}

struct arch_search_params {
   lib_t    lib;
   ident_t  name;
   tree_t  *arch;
};

static void find_arch(ident_t name, int kind, void *context)
{
   struct arch_search_params *params = context;

   ident_t prefix = ident_until(name, '-');

   if ((kind == T_ARCH) && (prefix == params->name)) {
      tree_t t = lib_get_check_stale(params->lib, name);
      assert(t != NULL);

      if (*(params->arch) == NULL)
         *(params->arch) = t;
      else {
         lib_mtime_t old_mtime = lib_mtime(params->lib,
                                           tree_ident2(*(params->arch)));
         lib_mtime_t new_mtime = lib_mtime(params->lib, tree_ident2(t));

         if (new_mtime == old_mtime) {
            // Analysed at the same time: compare line number
            // Note this assumes both architectures are from the same
            // file but this shouldn't be a problem with high-resolution
            // timestamps
            uint16_t new_line = tree_loc(t)->first_line;
            uint16_t old_line = tree_loc(*(params->arch))->first_line;

            if (new_line > old_line)
               *(params->arch) = t;
         }
         else if (new_mtime > old_mtime)
            *(params->arch) = t;
      }
   }
}

static tree_t pick_arch(const loc_t *loc, ident_t name)
{
   // When an explicit architecture name is not given select the most
   // recently analysed architecture of this entity

   ident_t lib_name = ident_until(name, '.');
   lib_t lib = lib_find(istr(lib_name), true, true);

   tree_t arch = lib_get_check_stale(lib, name);
   if ((arch == NULL) || (tree_kind(arch) != T_ARCH)) {
      arch = NULL;
      struct arch_search_params params = { lib, name, &arch };
      lib_walk_index(lib, find_arch, &params);

      if (arch == NULL)
         fatal_at(loc, "no suitable architecture for %s", istr(name));
   }

   return arch;
}

static tree_t fixup_entity_refs(tree_t t, void *context)
{
   // Rewrite references to an entity to point at the selected architecture
   // so attributes like 'PATH_NAME are correct

   if (tree_kind(t) != T_REF)
      return t;

   tree_t arch = context;

   if (tree_ref(t) == tree_ref(arch))
      tree_set_ref(t, arch);

   return t;
}

static tree_t rewrite_refs(tree_t t, void *context)
{
   rewrite_params_t *params = context;

   if (tree_kind(t) != T_REF)
      return t;

   tree_t decl = tree_ref(t);

   tree_kind_t decl_kind = tree_kind(decl);
   if (decl_kind != tree_kind(params->formal))
      return t;

   if (decl != params->formal)
      return t;

   // Do not rewrite references if they appear as formal names
   if (tree_attr_int(t, ident_new("formal"), 0))
      return t;

   // Skip assignments to OPEN ports
   if (params->actual == NULL)
      return t;

   switch (tree_kind(params->actual)) {
   case T_SIGNAL_DECL:
   case T_ENUM_LIT:
      tree_set_ref(t, params->actual);
      tree_set_type(t, tree_type(params->actual));
      break;
   case T_LITERAL:
   case T_AGGREGATE:
   case T_REF:
   case T_ARRAY_SLICE:
   case T_ARRAY_REF:
   case T_FCALL:
      return params->actual;
   default:
      fatal_at(tree_loc(params->actual), "cannot handle tree kind %s "
               "in rewrite_refs", tree_kind_str(tree_kind(params->actual)));
   }

   return t;
}

static tree_t elab_port_to_signal(tree_t arch, tree_t port, tree_t actual)
{
   assert(tree_kind(port) == T_PORT_DECL);

   ident_t name = tree_ident(port);

   const int ndecls = tree_decls(arch);
   for (int i = 0; i < ndecls; i++) {
      tree_t d = tree_decl(arch, i);
      if (tree_ident(d) == name)
         return d;
   }

   type_t port_type   = tree_type(port);
   type_t actual_type = tree_type(actual);

   type_t type = (type_kind(port_type) == T_UARRAY) ? actual_type : port_type;

   port_mode_t mode = tree_subkind(port);

   tree_t s = tree_new(T_SIGNAL_DECL);
   tree_set_ident(s, tree_ident(port));
   tree_set_type(s, type);
   tree_add_attr_int(s, fst_dir_i, mode);

   if ((mode == PORT_OUT) || (mode == PORT_INOUT)) {
      if (tree_has_value(port))
         tree_set_value(s, tree_value(port));
   }

   tree_add_decl(arch, s);
   return s;
}

static void elab_copy_context(tree_t dest, tree_t src)
{
   const int nctx = tree_contexts(src);
   for (int i = 0; i < nctx; i++)
      tree_add_context(dest, tree_context(src, i));
}

static tree_t elab_signal_port(tree_t arch, tree_t formal, tree_t param,
                               map_list_t **maps)
{
   assert(tree_kind(param) == T_PARAM);

   tree_t actual = tree_value(param);

   // NULL name means associate the whole port
   tree_t name = NULL;
   if (tree_subkind(param) == P_NAMED) {
      tree_t n = tree_name(param);
      if (tree_kind(n) != T_REF)
         name = n;
   }

   switch (tree_kind(actual)) {
   case T_ARRAY_REF:
   case T_REF:
   case T_ARRAY_SLICE:
      {
         // Replace the formal port with a signal and connect its nets to
         // those of the actual

         tree_t ref = actual;
         while (tree_kind(ref) != T_REF)
            ref = tree_value(ref);

         tree_t decl = tree_ref(ref);
         if (tree_kind(decl) == T_SIGNAL_DECL) {
            tree_t s = elab_port_to_signal(arch, formal, actual);

            map_list_t *m = xmalloc(sizeof(map_list_t));
            m->next   = *maps;
            m->formal = formal;
            m->actual = actual;
            m->signal = s;
            m->name   = name;

            *maps = m;

            return s;
         }
         else
            return actual;
      }

   case T_LITERAL:
   case T_AGGREGATE:
      return actual;

   case T_OPEN:
      return NULL;

   default:
      fatal_at(tree_loc(actual), "tree %s not supported as actual",
               tree_kind_str(tree_kind(actual)));
   }
}

static ident_t elab_formal_name(tree_t t)
{
   tree_kind_t kind;
   while ((kind = tree_kind(t)) != T_REF) {
      switch (kind) {
      case T_ARRAY_REF:
      case T_ARRAY_SLICE:
         t = tree_value(t);
         break;

      default:
         fatal_at(tree_loc(t), "sorry, this kind of formal is not supported %s",
                  tree_kind_str(kind));
      }
   }

   return tree_ident(t);
}

static map_list_t *elab_map(tree_t t, tree_t arch,
                            tree_formals_t tree_Fs, tree_formal_t tree_F,
                            tree_actuals_t tree_As, tree_actual_t tree_A)
{
   tree_t unit = tree_ref(arch);
   assert(tree_kind(unit) == T_ENTITY);

   const int nformals = tree_Fs(unit);
   const int nactuals = tree_As(t);

   bool have_formals[nformals];
   for (int i = 0; i < nformals; i++)
      have_formals[i] = false;

   map_list_t *maps = NULL;

   for (int i = 0; i < nactuals; i++) {
      tree_t p = tree_A(t, i);
      tree_t formal = NULL;

      switch (tree_subkind(p)) {
      case P_POS:
         {
            const int pos = tree_pos(p);
            formal = tree_F(unit, pos);
            have_formals[pos] = true;
         }
         break;
      case P_NAMED:
         {
            ident_t name = elab_formal_name(tree_name(p));
            for (int j = 0; j < nformals; j++) {
               tree_t port = tree_F(unit, j);
               if (tree_ident(port) == name) {
                  formal = port;
                  have_formals[j] = true;
                  break;
               }
            }
         }
         break;
      default:
         assert(false);
      }
      assert(formal != NULL);

      rewrite_params_t params = {
         .formal = formal,
         .actual = NULL
      };

      switch (tree_class(formal)) {
      case C_SIGNAL:
         params.actual = elab_signal_port(arch, formal, p, &maps);
         break;

      case C_CONSTANT:
         params.actual = tree_value(p);
         break;

      default:
         assert(false);
      }

      tree_rewrite(arch, rewrite_refs, &params);
   }

   // Assign default values
   for (unsigned i = 0; i < nformals; i++) {
      if (!have_formals[i]) {
         tree_t f = tree_F(unit, i);
         if (tree_has_value(f)) {
            rewrite_params_t params = {
               .formal = f,
               .actual = tree_value(f)
            };
            tree_rewrite(arch, rewrite_refs, &params);
         }
      }
   }

   return maps;
}

static netid_t elab_get_net(tree_t expr, int n)
{
   switch (tree_kind(expr)) {
   case T_REF:
      return tree_net(tree_ref(expr), n);

   case T_ARRAY_REF:
      {
         assert(tree_params(expr) == 1);

         tree_t value = tree_value(expr);
         type_t array_type = tree_type(value);
         tree_t index = tree_value(tree_param(expr, 0));

         range_t type_r  = type_dim(array_type, 0);

         const int64_t type_left = assume_int(type_r.left);
         const int64_t index_val = assume_int(index);
         const int64_t type_off =
            (type_r.kind == RANGE_TO)
            ? index_val - type_left
            : type_left - index_val;

         const int stride = type_width(type_elem(array_type));

         return elab_get_net(value, n + (type_off * stride));
      }

   case T_ARRAY_SLICE:
      {
         tree_t value = tree_value(expr);
         type_t array_type = tree_type(value);

         range_t type_r  = type_dim(array_type, 0);
         range_t slice_r = tree_range(expr);

         assert(type_r.kind == slice_r.kind);

         const int64_t slice_left = assume_int(slice_r.left);
         const int64_t type_left = assume_int(type_r.left);
         const int64_t type_off =
            (type_r.kind == RANGE_TO)
            ? slice_left - type_left
            : type_left - slice_left;

         const int stride = type_width(type_elem(array_type));

         return elab_get_net(value, n + (type_off * stride));
      }

   default:
      assert(false);
   }
}

static void elab_map_nets(map_list_t *maps)
{
   for (; maps != NULL; maps = maps->next) {
      if (maps->name == NULL) {
         // Associate the whole port
         const int awidth = type_width(tree_type(maps->actual));
         type_t ftype = tree_type(maps->signal);
         if (type_kind(ftype) != T_UARRAY) {
            const int fwidth = type_width(ftype);
            if (fwidth != awidth) {
               error_at(tree_loc(maps->actual), "actual width %d does not "
                        "match formal width %d", awidth, fwidth);
               ++errors;
               continue;
            }
         }

         for (int i = 0; i < awidth; i++)
            tree_add_net(maps->signal, elab_get_net(maps->actual, i));
      }
      else {
         // Associate a sub-element or slice of the port
         switch (tree_kind(maps->name)) {
         case T_ARRAY_REF:
            {
               type_t array_type = tree_type(maps->formal);

               type_t elem_type = type_elem(array_type);
               const int width = type_width(elem_type);

               assert(tree_params(maps->name) == 1);

               int64_t low, high;
               range_bounds(type_dim(array_type, 0), &low, &high);

               tree_t index = tree_value(tree_param(maps->name, 0));
               int64_t index_val = assume_int(index) - low;

               for (int i = 0; i < width; i++)
                  tree_change_net(maps->signal, index_val + i,
                                  elab_get_net(maps->actual, i));
            }
            break;

         case T_ARRAY_SLICE:
            {
               type_t array_type = tree_type(maps->formal);

               type_t elem_type = type_elem(array_type);
               const int elem_width = type_width(elem_type);

               range_t slice = tree_range(maps->name);

               int64_t low, high;
               range_bounds(slice, &low, &high);

               const int64_t left = assume_int(slice.left);

               for (int64_t i = low; i <= high; i++) {
                  for (int j = 0; j < elem_width; j++) {
                     const int64_t off =
                        (slice.kind == RANGE_TO) ? i - left : left - i;
                     tree_change_net(
                        maps->signal, (i * elem_width) + j,
                        elab_get_net(maps->actual, off * elem_width + j));
                  }
               }
            }
            break;

         default:
            fatal_at(tree_loc(maps->formal), "sorry, tree kind %s not "
                     "supported as a formal",
                     tree_kind_str(tree_kind(maps->formal)));
         }
      }
   }
}

static bool elab_should_copy(tree_t t)
{
   switch (tree_kind(t)) {
   case T_CONST_DECL:
   case T_SIGNAL_DECL:
   case T_GENVAR:
   case T_VAR_DECL:
   case T_PORT_DECL:
      return true;

   default:
      return false;
   }
}

static void elab_build_copy_list(tree_t t, void *context)
{
   copy_list_t **list = context;

   if (elab_should_copy(t)) {
      copy_list_t *new = xmalloc(sizeof(copy_list_t));
      new->tree = t;
      new->next = *list;

      *list = new;
   }
}

static bool elab_copy_trees(tree_t t, void *context)
{
   copy_list_t *list = context;

   if (elab_should_copy(t)) {
      for (; list != NULL; list = list->next) {
         if (list->tree == t)
            return true;
      }
   }

   return false;
}

static tree_t elab_copy(tree_t t)
{
   copy_list_t *copy_list = NULL;
   tree_visit(t, elab_build_copy_list, &copy_list);

   // For achitectures, also make a copy of the entity ports
   if (tree_kind(t) == T_ARCH)
      tree_visit(tree_ref(t), elab_build_copy_list, &copy_list);

   tree_t copy = tree_copy(t, elab_copy_trees, copy_list);

   while (copy_list != NULL) {
      copy_list_t *tmp = copy_list->next;
      free(copy_list);
      copy_list = tmp;
   }

   return copy;
}

static bool elab_compatible_map(tree_t comp, tree_t entity, char *what,
                                tree_formals_t tree_Fs, tree_formal_t tree_F)
{
   // TODO: for now they must exactly match up

   const int comp_nf   = (*tree_Fs)(comp);
   const int entity_nf = (*tree_Fs)(entity);

   for (int i = 0; i < comp_nf; i++) {
      tree_t comp_f = (*tree_F)(comp, i);

      bool found = false;
      for (int j = 0; j < entity_nf; j++) {
         tree_t entity_f = (*tree_F)(entity, j);

         if (tree_ident(comp_f) != tree_ident(entity_f))
            continue;

         found = true;

         type_t entity_type = tree_type(entity_f);
         type_t comp_type   = tree_type(comp_f);

         if (!type_eq(entity_type, comp_type)) {
            error_at(tree_loc(comp_f), "type of %s %s in component "
                     "declaration %s is %s which does not match type %s in "
                     "entity %s", what, istr(tree_ident(comp_f)),
                     istr(tree_ident(comp)), type_pp(comp_type),
                     type_pp(entity_type), istr(tree_ident(entity)));
            ++errors;
            return false;
         }
      }

      if (!found) {
         error_at(tree_loc(comp_f), "%s %s not found in entity %s",
                  what, istr(tree_ident(comp_f)), istr(tree_ident(entity)));
         ++errors;
         return false;
      }
   }

   return true;
}

static tree_t elab_default_binding(tree_t t)
{
   // Default binding indication is described in LRM 93 section 5.2.2

   tree_t comp = tree_ref(t);

   ident_t full_i = tree_ident(comp);
   ident_t comp_i = ident_rfrom(full_i, '.');
   ident_t lib_i = ident_until(full_i, '.');

   ident_t entity_i;
   if (lib_i == full_i)
      entity_i = ident_prefix(lib_name(lib_work()), full_i, '.');
   else
      entity_i = ident_prefix(lib_i, comp_i, '.');

   tree_t arch = elab_copy(pick_arch(tree_loc(comp), entity_i));

   // Check entity is compatible with component declaration

   tree_t entity = tree_ref(arch);

   if (!elab_compatible_map(comp, entity, "generic",
                            tree_generics, tree_generic))
      return NULL;

   if (!elab_compatible_map(comp, entity, "port", tree_ports, tree_port))
      return NULL;

   return arch;
}

static void elab_instance(tree_t t, const elab_ctx_t *ctx)
{
   tree_t arch = NULL;
   switch (tree_class(t)) {
   case C_ENTITY:
      arch = elab_copy(pick_arch(tree_loc(t), tree_ident2(t)));
      break;

   case C_COMPONENT:
      arch = elab_default_binding(t);
      break;

   case C_CONFIGURATION:
      fatal_at(tree_loc(t), "sorry, configurations is not supported yet");
      break;

   default:
      assert(false);
   }

   if (arch == NULL)
      return;

   map_list_t *maps = elab_map(t, arch, tree_ports, tree_port,
                               tree_params, tree_param);

   (void)elab_map(t, arch, tree_generics, tree_generic,
                  tree_genmaps, tree_genmap);

   tree_t entity = tree_ref(arch);

   elab_copy_context(ctx->out, entity);

   ident_t ninst = hpathf(ctx->inst, '@', "%s(%s)",
                          simple_name(istr(tree_ident2(arch))),
                          simple_name(istr(tree_ident(arch))));

   elab_funcs(arch, entity);
   simplify(arch);

   elab_map_nets(maps);

   while (maps != NULL) {
      map_list_t *tmp = maps->next;
      free(maps);
      maps = tmp;
   }

   elab_ctx_t new_ctx = {
      .out      = ctx->out,
      .path     = ctx->path,
      .inst     = ninst,
      .next_net = ctx->next_net
   };
   elab_arch(arch, &new_ctx);
}

static void elab_signal_nets(tree_t decl, const elab_ctx_t *ctx)
{
   // Assign net IDs to each sub-element of a signal declaration

   if (tree_nets(decl) != 0) {
      // Nets have already been assigned e.g. from a port map
   }
   else {
      const int width = type_width(tree_type(decl));
      for (int i = 0; i < width; i++)
         tree_add_net(decl, (*ctx->next_net)++);
   }
}

static void elab_decls(tree_t t, const elab_ctx_t *ctx)
{
   tree_add_attr_str(t, inst_name_i,
                     ident_prefix(ctx->inst, ident_new(":"), '\0'));

   const int ndecls = tree_decls(t);
   for (int i = 0; i < ndecls; i++) {
      tree_t d = tree_decl(t, i);
      const char *label = simple_name(istr(tree_ident(d)));
      ident_t ninst = hpathf(ctx->inst, ':', "%s", label);
      ident_t npath = hpathf(ctx->path, ':', "%s", label);

      switch (tree_kind(d)) {
      case T_SIGNAL_DECL:
         elab_signal_nets(d, ctx);
         // Fall-through
      case T_FUNC_BODY:
      case T_PROC_BODY:
      case T_ALIAS:
      case T_FILE_DECL:
      case T_VAR_DECL:
         tree_set_ident(d, npath);
         tree_add_decl(ctx->out, d);
         tree_add_attr_str(d, inst_name_i, ninst);
         break;
      case T_FUNC_DECL:
      case T_PROC_DECL:
         tree_set_ident(d, npath);
         break;
      case T_CONST_DECL:
         tree_set_ident(d, npath);
         tree_add_attr_str(d, inst_name_i, ninst);
         tree_add_decl(ctx->out, d);
         break;
      default:
         break;
      }
   }
}

static void elab_push_scope(tree_t t, const elab_ctx_t *ctx)
{
   tree_t h = tree_new(T_HIER);
   tree_set_loc(h, tree_loc(t));
   tree_set_subkind(h, tree_kind(t));

   if (tree_kind(t) == T_PACKAGE) {
      const char *name = istr(tree_ident(t));
      char lower[strlen(name) + 1], *p;
      for (p = lower; *name != '\0'; p++, name++)
         *p = tolower(*name);
      *p = '\0';

      tree_set_ident(h, ident_new(lower));
   }
   else
      tree_set_ident(h, ident_new(strrchr(istr(ctx->path), ':') + 1));

   if ((tree_kind(t) == T_ARCH) && (tree_decls(ctx->out) > 0)) {
      // Convert an identifier like WORK.FOO-RTL to foo(rtl)
      const char *id = istr(tree_ident(t));
      while ((*id != '\0') && (*id++ != '.'))
         ;

      const size_t len = strlen(id);
      char str[len + 2];
      char *p = str;
      for (; *id != '\0'; p++, id++) {
         if (*id == '-')
            *p = '(';
         else
            *p = tolower(*id);
      }

      *p++ = ')';
      *p++ = '\0';

      tree_set_ident2(h, ident_new(str));
   }

   tree_add_decl(ctx->out, h);
}

static void elab_pop_scope(const elab_ctx_t *ctx)
{
   tree_t last_decl = tree_decl(ctx->out, tree_decls(ctx->out) - 1);
   tree_add_attr_int(last_decl, scope_pop_i,
                     tree_attr_int(last_decl, scope_pop_i, 0) + 1);
}

static void elab_for_generate(tree_t t, elab_ctx_t *ctx)
{
   int64_t low, high;
   range_bounds(tree_range(t), &low, &high);

   for (int64_t i = low; i <= high; i++) {
      tree_t copy = elab_copy(t);

      tree_t genvar = tree_ref(copy);
      rewrite_params_t params = {
         .formal = genvar,
         .actual = get_int_lit(genvar, i)
      };
      tree_rewrite(copy, rewrite_refs, &params);

      //const char *label = istr(tree_ident(copy));
      ident_t npath = hpathf(ctx->path, '\0', "[%"PRIi64"]", i);
      ident_t ninst = hpathf(ctx->inst, '\0', "[%"PRIi64"]", i);

      elab_ctx_t new_ctx = {
         .out      = ctx->out,
         .path     = npath,
         .inst     = ninst,
         .next_net = ctx->next_net
      };

      elab_push_scope(copy, &new_ctx);
      elab_decls(copy, &new_ctx);
      elab_stmts(copy, &new_ctx);
      elab_pop_scope(&new_ctx);
   }
}

static void elab_process(tree_t t, const elab_ctx_t *ctx)
{
   // Rename local functions in this process to avoid collisions in the
   // global LLVM namespace

   const int ndecls = tree_decls(t);
   for (int i = 0; i < ndecls; i++) {
      tree_t d = tree_decl(t, i);
      switch (tree_kind(d)) {
      case T_FUNC_DECL:
      case T_FUNC_BODY:
      case T_PROC_DECL:
      case T_PROC_BODY:
         tree_set_ident(d, ident_prefix(ctx->path, tree_ident(d), '_'));
         break;
      default:
         break;
      }
   }
}

static void elab_stmts(tree_t t, const elab_ctx_t *ctx)
{
   const int nstmts = tree_stmts(t);
   for (int i = 0; i < nstmts; i++) {
      tree_t s = tree_stmt(t, i);
      const char *label = istr(tree_ident(s));
      ident_t npath = hpathf(ctx->path, ':', "%s", label);
      ident_t ninst = hpathf(ctx->inst, ':', "%s", label);

      elab_ctx_t new_ctx = {
         .out      = ctx->out,
         .path     = npath,
         .inst     = ninst,
         .next_net = ctx->next_net
      };

      switch (tree_kind(s)) {
      case T_INSTANCE:
         elab_instance(s, &new_ctx);
         break;
      case T_BLOCK:
         elab_block(s, &new_ctx);
         break;
      case T_FOR_GENERATE:
         elab_for_generate(s, &new_ctx);
         break;
      case T_IF_GENERATE:
         fatal("IF-GENERATE statement was not constant folded");
      case T_PROCESS:
         elab_process(s, &new_ctx);
         // Fall-through
      default:
         tree_add_stmt(ctx->out, s);
      }

      tree_set_ident(s, npath);
   }
}

static void elab_block(tree_t t, const elab_ctx_t *ctx)
{
   elab_push_scope(t, ctx);
   elab_decls(t, ctx);
   elab_stmts(t, ctx);
   elab_pop_scope(ctx);
}

static void elab_arch(tree_t t, const elab_ctx_t *ctx)
{
   elab_copy_context(ctx->out, t);
   elab_push_scope(t, ctx);
   elab_decls(t, ctx);
   elab_stmts(t, ctx);
   elab_pop_scope(ctx);

   tree_rewrite(t, fixup_entity_refs, t);

   tree_set_ident(t, ident_prefix(ctx->path, ident_new(":"), '\0'));
}

static void elab_entity(tree_t t, const elab_ctx_t *ctx)
{
   if (tree_ports(t) > 0 || tree_generics(t) > 0) {
      // LRM 93 section 12.1 says implementation may allow this but
      // is not required to
      fatal("top-level entity may not have generics or ports");
   }

   tree_t arch = pick_arch(NULL, tree_ident(t));
   const char *name = simple_name(istr(tree_ident(t)));
   ident_t ninst = hpathf(ctx->inst, ':', ":%s(%s)", name,
                          simple_name(istr(tree_ident(arch))));
   ident_t npath = hpathf(ctx->path, ':', ":%s", name);

   elab_copy_context(ctx->out, t);

   elab_funcs(arch, t);
   simplify(arch);

   elab_ctx_t new_ctx = {
      .out      = ctx->out,
      .path     = npath,
      .inst     = ninst,
      .next_net = ctx->next_net
   };
   elab_arch(arch, &new_ctx);
}

static tree_t rewrite_funcs(tree_t t, void *context)
{
   if (tree_kind(t) != T_FCALL)
      return t;

   tree_t decl = tree_ref(t);

   ident_t name = tree_ident(decl);
   for (tree_t *rlist = context; *rlist != NULL; ++rlist) {
      const bool match =
         (tree_ident(*rlist) == name)
         && (type_eq(tree_type(*rlist), tree_type(decl)));

      if (match) {
         tree_set_ref(t, *rlist);
         break;
      }
   }

   return t;
}

static lib_t elab_link_lib(ident_t name)
{
   lib_t lib = lib_find(istr(ident_until(name, '.')), true, true);
   if (lib == NULL)
      fatal("cannot link library %s", istr(name));
   return lib;
}

static void elab_funcs(tree_t arch, tree_t ent)
{
   // Look through all the package bodies required by the context and
   // rewrite references to function declarations with references to
   // the function body.  This allows these functions to be folded by
   // the simplify phase

   int nreplace = 0;
   tree_t rlist[FUNC_REPLACE_MAX];

   const int ncontext_arch = tree_contexts(arch);
   const int ncontext_ent  = tree_contexts(ent);
   for (int i = 0; i < ncontext_arch + ncontext_ent; i++) {
      tree_t ctx = (i < ncontext_arch)
         ? tree_context(arch, i)
         : tree_context(ent, i - ncontext_arch);

      ident_t name = tree_ident(ctx);
      lib_t lib = elab_link_lib(name);

      ident_t body_i = ident_prefix(name, ident_new("body"), '-');
      tree_t body = lib_get_check_stale(lib, body_i);
      if (body == NULL)
         continue;

      const int ndecls = tree_decls(body);
      for (int j = 0; j < ndecls; j++) {
         tree_t decl = tree_decl(body, j);
         if (tree_kind(decl) != T_FUNC_BODY)
            continue;

         if (nreplace + 1 == FUNC_REPLACE_MAX) {
            tree_rewrite(arch, rewrite_funcs, rlist);
            nreplace = 0;
         }

         rlist[nreplace++] = decl;
         rlist[nreplace] = NULL;
      }
   }

   if (nreplace > 0)
      tree_rewrite(arch, rewrite_funcs, rlist);
}

static tree_t rewrite_package_signals(tree_t t, void *ctx)
{
   tree_t decl = ctx;

   if (tree_kind(t) != T_REF)
      return t;

   tree_t ref = tree_ref(t);

   if (tree_kind(ref) != T_SIGNAL_DECL)
      return t;

   if (tree_ident(ref) != tree_ident(decl))
      return t;

   tree_set_ref(t, decl);
   return t;
}

static void elab_package_signals(tree_t unit, const elab_ctx_t *ctx)
{
   bool have_signals = false;
   const int ndecls = tree_decls(unit);
   for (int i = 0; i < ndecls; i++) {
      tree_t d = tree_decl(unit, i);
      if (tree_kind(d) != T_SIGNAL_DECL)
         continue;
      else if (tree_nets(d) > 0)
         continue;

      if (!have_signals) {
         elab_push_scope(unit, ctx);
         have_signals = true;
      }

      elab_signal_nets(d, ctx);

      tree_rewrite(ctx->out, rewrite_package_signals, d);

      tree_add_decl(ctx->out, d);

      ident_t i = ident_new(package_signal_path_name(tree_ident(d)));
      tree_set_ident(d, i);
      tree_add_attr_str(d, inst_name_i, i);
   }

   if (have_signals)
      elab_pop_scope(ctx);
}

static void elab_context_signals(const elab_ctx_t *ctx)
{
   const int nctx = tree_contexts(ctx->out);
   for (int i = 0; i < nctx; i++) {
      tree_t c = tree_context(ctx->out, i);

      ident_t name = tree_ident(c);
      lib_t lib = elab_link_lib(name);

      tree_t pack = lib_get_check_stale(lib, name);
      if (tree_kind(pack) != T_PACKAGE)
         continue;

      elab_package_signals(pack, ctx);

      ident_t body_i = ident_prefix(name, ident_new("body"), '-');
      tree_t body = lib_get_check_stale(lib, body_i);
      if (body == NULL)
         continue;

      elab_package_signals(body, ctx);
   }
}

tree_t elab(tree_t top)
{
   inst_name_i  = ident_new("INSTANCE_NAME");
   fst_dir_i    = ident_new("fst_dir");
   scope_pop_i  = ident_new("scope_pop");

   tree_t e = tree_new(T_ELAB);
   tree_set_ident(e, ident_prefix(tree_ident(top),
                                  ident_new("elab"), '.'));

   netid_t next_net = 0;
   elab_ctx_t ctx = {
      .out      = e,
      .path     = NULL,
      .inst     = NULL,
      .next_net = &next_net
   };

   switch (tree_kind(top)) {
   case T_ENTITY:
      elab_entity(top, &ctx);
      break;
   default:
      fatal("%s is not a suitable top-level unit", istr(tree_ident(top)));
   }

   elab_context_signals(&ctx);

   if (errors > 0)
      return NULL;

   tree_add_attr_int(e, ident_new("nnets"), next_net);

   if (opt_get_int("cover"))
      cover_tag(e);

   bounds_check(e);

   if (bounds_errors() == 0) {
      lib_put(lib_work(), e);
      return e;
   }
   else
      return NULL;
}
