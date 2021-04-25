/*
Copyright (c) 2018, The Monero Project

All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
this list of conditions and the following disclaimer in the documentation
and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its contributors
may be used to endorse or promote products derived from this software without
specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "growbag.h"
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

struct gbag_t
{
    size_t z;
    size_t max;
    size_t ref;
    char * b;
    char * e;
    char * n;
    char * ni;
    char * no;
    gbag_recycle rc;
    gbag_moved mv;
};

void
gbag_new(gbag_t **out, size_t count, size_t size,
        gbag_recycle recycle, gbag_moved moved)
{
    gbag_t *gb = (gbag_t*) calloc(1, sizeof(gbag_t));
    gb->z = size;
    gb->max = count;
    gb->ref = 0;
    gb->b = (char*) calloc(gb->max, gb->z);
    gb->e = gb->b;
    gb->n = gb->b;
    gb->ni = gb->b;
    gb->no = (char*) calloc(1, size);
    gb->rc = recycle;
    gb->mv = moved;
    *out = gb;
}

void
gbag_free(gbag_t *gb)
{
    char *end = gb->b + (gb->max * gb->z);
    char *cur = gb->b;
    if (gb->rc)
    {
        while (cur < end)
        {
            gb->rc(cur);
            cur += gb->z;
        }
    }
    free(gb->b);
    free(gb->no);
    gb->max = 0;
    gb->ref = 0;
    gb->b = NULL;
    gb->e = NULL;
    gb->n = NULL;
    gb->ni = NULL;
    gb->rc = NULL;
    gb->mv = NULL;
    free(gb);
}

static inline int
gbag_occupied(gbag_t *gb, char *el)
{
    return *el || memcmp(el, gb->no, gb->z);
}

void *
gbag_get(gbag_t *gb)
{
    char *end = gb->b + (gb->max * gb->z);
    char *from = gb->n;
    size_t nc, oc, ocz;
    char *b = NULL;
    char *rv = NULL;
    if (gb->ref == gb->max)
        goto grow;
scan:
    while(gb->n < end)
    {
        if (!gbag_occupied(gb, gb->n))
        {
            gb->ref++;
            rv = gb->n;
            gb->n += gb->z;
            if (rv >= gb->e)
                gb->e = gb->n;
            return rv;
        }
        gb->n += gb->z;
    }
    if (from != gb->b)
    {
        end = from;
        gb->n = gb->b;
        from = gb->n;
        goto scan;
    }
    else
    {
grow:
        ocz = gb->max * gb->z;
        oc = gb->max;
        nc = gb->max << 1;
        b = (char*) realloc(gb->b, nc * gb->z);
        if (!b)
            return NULL;
        rv = b + ocz;
        memset(rv, 0, ocz);
        if (gb->mv && gb->b != b)
            gb->mv(b, oc);
        gb->max = nc;
        gb->ref++;
        gb->b = b;
        gb->n = rv + gb->z;
        gb->e = gb->n;
        return rv;
    }
    return NULL;
}

void
gbag_put(gbag_t *gb, void *item)
{
    if (gb->e > gb->b && (char*)item + gb->z == gb->e)
        gb->e -= gb->z;
    if (gb->rc)
        gb->rc(item);
    memset(item, 0, gb->z);
    gb->n = (char*)item;
    gb->ref--;
}

size_t
gbag_max(gbag_t *gb)
{
    return gb->max;
}

size_t
gbag_used(gbag_t *gb)
{
    return gb->ref;
}

void *
gbag_find(gbag_t *gb, const void *key, gbag_cmp cmp)
{
    return gbag_find_after(gb, key, cmp, NULL);
}

void *
gbag_find_after(gbag_t *gb, const void *key, gbag_cmp cmp, void *from)
{
    char *s = gb->b;
    char *e = gb->b + (gb->max * gb->z);
    if (from)
        s = ((char*)from) + gb->z;
    int c = (e-s)/gb->z;
    return bsearch(key, s, c, gb->z, cmp);
}

void *
gbag_first(gbag_t *gb)
{
    char *s = gb->b;
    char *e = gb->e;
    gb->ni = s;
    while (s < e)
    {
        if (gbag_occupied(gb, s))
            return s;
        s += gb->z;
        gb->ni = s;
    }
    return NULL;
}

void *
gbag_next(gbag_t *gb, void* from)
{
    if (from)
        gb->ni = ((char*)from) + gb->z;
    char *e = gb->e;
    char *s = gb->ni;
    while (s < e)
    {
        gb->ni += gb->z;
        if (gbag_occupied(gb, s))
            return s;
        s += gb->z;
    }
    return NULL;
}

