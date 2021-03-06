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

#include "util.h"
#include "fbuf.h"
#include "ident.h"

#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

struct clist {
   char         value;
   struct trie  *down;
   struct clist *left;
   struct clist *right;
};

struct trie {
   char         value;
   uint8_t      write_gen;
   uint16_t     depth;
   uint32_t     write_index;
   struct trie  *up;
   struct clist *children;
};

struct ident_rd_ctx {
   fbuf_t  *file;
   size_t   cache_sz;
   size_t   cache_alloc;
   ident_t *cache;
};

struct ident_wr_ctx {
   fbuf_t   *file;
   uint32_t  next_index;
   uint8_t   generation;
};

static struct trie root = {
   .value       = '\0',
   .write_gen   = 0,
   .write_index = 0,
   .depth       = 1,
   .up          = NULL,
   .children    = NULL
};

static struct trie *alloc_node(char ch, struct trie *prev)
{
   struct trie *t = xmalloc(sizeof(struct trie));
   t->value     = ch;
   t->depth     = prev->depth + 1;
   t->up        = prev;
   t->children  = NULL;
   t->write_gen = 0;

   struct clist *c = xmalloc(sizeof(struct clist));
   c->value    = ch;
   c->down     = t;
   c->left     = NULL;
   c->right    = NULL;

   struct clist *it, **where;
   for (it = prev->children, where = &(prev->children);
        it != NULL;
        where = (ch < it->value ? &(it->left) : &(it->right)),
           it = *where)
      ;

   *where = c;

   return t;
}

static void build_trie(const char *str, struct trie *prev, struct trie **end)
{
   assert(*str != '\0');
   assert(prev != NULL);

   struct trie *t = alloc_node(*str, prev);

   if (*(++str) == '\0')
      *end = t;
   else
      build_trie(str, t, end);
}

static struct clist *search_node(struct trie *t, char ch)
{
   struct clist *it;
   for (it = t->children;
        (it != NULL) && (it->value != ch);
        it = (ch < it->value ? it->left : it->right))
      ;

   return it;
}

static bool search_trie(const char **str, struct trie *t, struct trie **end)
{
   assert(**str != '\0');
   assert(t != NULL);

   struct clist *it = search_node(t, **str);

   if (it == NULL) {
      *end = t;
      return false;
   }
   else {
      (*str)++;

      if (**str == '\0') {
         *end = it->down;
         return true;
      }
      else
         return search_trie(str, it->down, end);
   }
}

ident_t ident_new(const char *str)
{
   assert(str != NULL);
   assert(*str != '\0');

   struct trie *result;
   if (!search_trie(&str, &root, &result))
      build_trie(str, result, &result);

   return result;
}

bool ident_interned(const char *str)
{
   assert(str != NULL);
   assert(*str != '\0');

   struct trie *result;
   return search_trie(&str, &root, &result);
}

const char *istr(ident_t ident)
{
   assert(ident != NULL);

   char *p = get_fmt_buf(ident->depth) + ident->depth - 1;
   *p = '\0';

   struct trie *it;
   for (it = ident; it->value != '\0'; it = it->up) {
      assert(it != NULL);
      *(--p) = it->value;
   }

   return p;
}

ident_wr_ctx_t ident_write_begin(fbuf_t *f)
{
   static uint8_t ident_wr_gen = 1;
   assert(ident_wr_gen > 0);

   struct ident_wr_ctx *ctx = xmalloc(sizeof(struct ident_wr_ctx));
   ctx->file       = f;
   ctx->generation = ident_wr_gen++;
   ctx->next_index = 0;

   return ctx;
}

void ident_write_end(ident_wr_ctx_t ctx)
{
   free(ctx);
}

void ident_write(ident_t ident, ident_wr_ctx_t ctx)
{
   if (ident == NULL) {
      write_u32(UINT32_MAX, ctx->file);
      write_u8(0, ctx->file);
   }
   else if (ident->write_gen == ctx->generation)
      write_u32(ident->write_index, ctx->file);
   else {
      write_u32(UINT32_MAX, ctx->file);
      write_raw(istr(ident), ident->depth, ctx->file);

      ident->write_gen   = ctx->generation;
      ident->write_index = ctx->next_index++;

      assert(ctx->next_index != UINT32_MAX);
   }
}

ident_rd_ctx_t ident_read_begin(fbuf_t *f)
{
   struct ident_rd_ctx *ctx = xmalloc(sizeof(struct ident_rd_ctx));
   ctx->file        = f;
   ctx->cache_alloc = 256;
   ctx->cache_sz    = 0;
   ctx->cache       = xmalloc(ctx->cache_alloc * sizeof(ident_t));

   return ctx;
}

void ident_read_end(ident_rd_ctx_t ctx)
{
   free(ctx->cache);
   free(ctx);
}

ident_t ident_read(ident_rd_ctx_t ctx)
{
   const uint32_t index = read_u32(ctx->file);
   if (index == UINT32_MAX) {
      if (ctx->cache_sz == ctx->cache_alloc) {
         ctx->cache_alloc *= 2;
         ctx->cache = xrealloc(ctx->cache, ctx->cache_alloc * sizeof(ident_t));
      }

      struct trie *p = &root;
      char ch;
      while ((ch = read_u8(ctx->file)) != '\0') {
         struct clist *it = search_node(p, ch);
         if (it != NULL)
            p = it->down;
         else
            p = alloc_node(ch, p);
      }

      if (p == &root)
         return NULL;
      else {
         ctx->cache[ctx->cache_sz++] = p;
         return p;
      }
   }
   else {
      assert(index < ctx->cache_sz);
      return ctx->cache[index];
   }
}

ident_t ident_uniq(const char *prefix)
{
   static int counter = 0;

   const char *start = prefix;
   struct trie *end;
   if (search_trie(&start, &root, &end)) {
      const size_t len = strlen(prefix) + 16;
      char buf[len];
      snprintf(buf, len, "%s%d", prefix, counter++);

      return ident_new(buf);
   }
   else {
      struct trie *result;
      build_trie(start, end, &result);
      return result;
   }
}

ident_t ident_prefix(ident_t a, ident_t b, char sep)
{
   if (a == NULL)
      return b;
   else if (b == NULL)
      return a;

   struct trie *result;

   if (sep != '\0') {
      // Append separator
      const char sep_str[] = { sep, '\0' };
      const char *p_sep_str = sep_str;
      if (!search_trie(&p_sep_str, a, &result))
         build_trie(p_sep_str, result, &result);
   }
   else
      result = a;

   // Append b
   const char *bstr = istr(b);
   if (!search_trie(&bstr, result, &result))
      build_trie(bstr, result, &result);

   return result;
}

ident_t ident_strip(ident_t a, ident_t b)
{
   assert(a != NULL);
   assert(b != NULL);

   while (a->value == b->value && b->value != '\0') {
      a = a->up;
      b = b->up;
   }

   return (b->value == '\0' ? a : NULL);
}

char ident_char(ident_t i, unsigned n)
{
   assert(i != NULL);

   if (n == 0)
      return i->value;
   else
      return ident_char(i->up, n - 1);
}

ident_t ident_until(ident_t i, char c)
{
   assert(i != NULL);

   ident_t r = i;
   while (i->value != '\0') {
      if (i->value == c)
         r = i->up;
      i = i->up;
   }

   return r;
}

ident_t ident_runtil(ident_t i, char c)
{
   assert(i != NULL);

   while (i->value != '\0') {
      if (i->value == c)
         return i->up;
      i = i->up;
   }

   return i;
}

ident_t ident_rfrom(ident_t i, char c)
{
   assert(i != NULL);

   char buf[i->depth + 1];
   char *p = buf + i->depth;
   *p-- = '\0';

   while (i->value != '\0') {
      if (i->value == c)
         return ident_new(p + 1);
      *p-- = i->value;
      i = i->up;
   }

   return (i->value == '\0') ? NULL : i;
}

bool icmp(ident_t i, const char *s)
{
   assert(i != NULL);

   struct trie *result;
   if (!search_trie(&s, &root, &result))
      return false;
   else
      return result == i;
}

static bool ident_glob_walk(const struct trie *i, const char *g,
                            const char *const end)
{
   if (i->value == '\0')
      return (g < end);
   else if (g < end)
      return false;
   else if (*g == '*')
      return ident_glob_walk(i->up, g, end)
         || ident_glob_walk(i->up, g - 1, end);
   else if (i->value == *g)
      return ident_glob_walk(i->up, g - 1, end);
   else
      return false;
}

bool ident_glob(ident_t i, const char *glob, int length)
{
   assert(i != NULL);

   if (length < 0)
      length = strlen(glob);

   return ident_glob_walk(i, glob + length - 1, glob);
}

void ident_list_add(ident_list_t **list, ident_t i)
{
   ident_list_t *c = xmalloc(sizeof(ident_list_t));
   c->ident = i;
   c->next  = *list;

   *list = c;
}

void ident_list_free(ident_list_t *list)
{
   ident_list_t *it = list;
   while (it != NULL) {
      ident_list_t *next = it->next;
      free(it);
      it = next;
   }
}
