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

#include "bstack.h"
#include <string.h>
#include <stdlib.h>

struct bstack_t
{
    size_t c;
    size_t cc;
    size_t z;
    size_t n;
    size_t ni;
    recycle_fun rf;
    char * b;
};

void
bstack_new(bstack_t **out, size_t count, size_t size, recycle_fun recycle)
{
    bstack_t *q = (bstack_t*) calloc(1, sizeof(bstack_t));
    q->c = count;
    q->cc = 0;
    q->z = size;
    q->b = (char*) calloc(q->c, q->z);
    q->n = 0;
    q->ni = 0;
    q->rf = recycle;
    *out = q;
}

void
bstack_free(bstack_t *q)
{
    if (q->rf)
    {
        void *pb;
        bstack_reset(q);
        while ((pb = bstack_next(q)))
            q->rf(pb);
    }
    free(q->b);
    free(q);
}

void *
bstack_push(bstack_t *q, void *item)
{
    size_t idx = q->n++ % q->c;
    void *pb = q->b + (idx * q->z);
    if (q->rf && q->cc == q->c)
        q->rf(pb);
    if (item)
        memcpy(pb, item, q->z);
    else
        memset(pb, 0, q->z);
    if (q->cc < q->c)
        q->cc++;
    q->ni = q->cc;
    return pb;
}

void
bstack_drop(bstack_t *q)
{
    if (!q->cc)
        return;
    q->cc--;
    size_t idx = --q->n % q->c;
    void *pb = q->b + (idx * q->z);
    if (q->rf)
        q->rf(pb);
}

void *
bstack_top(bstack_t *q)
{
    if (!q->cc)
        return NULL;
    size_t idx = (q->n-1) % q->c;
    void *pb = q->b + (idx * q->z);
    return pb;
}

size_t
bstack_count(bstack_t *q)
{
    return q->cc;
}

void *
bstack_next(bstack_t *q)
{
    if (!q->ni)
        return NULL;
    q->ni--;
    size_t idx = (q->n - (q->cc - q->ni)) % q->c;
    void *pb = idx ? q->b + (idx * q->z) : q->b;
    return pb;
}

void
bstack_reset(bstack_t *q)
{
    q->ni = q->cc;
}

