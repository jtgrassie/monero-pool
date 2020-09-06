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

/* A bag of memory that exponentially grows */

#ifndef GBAG_H
#define GBAG_H

#include <stddef.h>

typedef struct gbag_t gbag_t;
typedef void (*gbag_recycle)(void*);
typedef void (*gbag_moved)(const void*,size_t);
typedef int (*gbag_cmp)(const void*,const void*);

void gbag_new(gbag_t **out, size_t count, size_t size,
        gbag_recycle recycle, gbag_moved moved);
void gbag_free(gbag_t *gb);

void * gbag_get(gbag_t *gb);
void gbag_put(gbag_t *gb, void *item);
size_t gbag_max(gbag_t *gb);
size_t gbag_used(gbag_t *gb);
void * gbag_find(gbag_t *gb, const void *key, gbag_cmp cmp);
void * gbag_find_after(gbag_t *gb, const void *key, gbag_cmp cmp, void* from);
void * gbag_first(gbag_t *gb);
void * gbag_next(gbag_t *gb, void* from);

#endif
