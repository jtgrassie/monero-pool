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

#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <ctype.h>
#include <inttypes.h>

#include "util.h"

int
is_hex_string(const char *str)
{
    if (strlen(str) == 0)
        return -1;
    const char *cp = str;
    while (*cp)
    {
        if (!isxdigit(*cp++))
            return -2;
    }
    return 0;
}

static const unsigned char h2d[256] = {
    ['0'] = 0x00, ['1'] = 0x01, ['2'] = 0x02, ['3'] = 0x03, ['4'] = 0x04,
    ['5'] = 0x05, ['6'] = 0x06, ['7'] = 0x07, ['8'] = 0x08, ['9'] = 0x09,
    ['A'] = 0x0A, ['B'] = 0x0B, ['C'] = 0x0C, ['D'] = 0x0D, ['E'] = 0x0E,
    ['F'] = 0x0F, ['a'] = 0x0A, ['b'] = 0x0B, ['c'] = 0x0C, ['d'] = 0x0D,
    ['e'] = 0x0E, ['f'] = 0x0F
};

void
hex_to_bin(const char *hex, unsigned char *bin, const size_t bin_size)
{
    const char *ph = hex;
    unsigned char *end = bin + bin_size;
    while (*ph && bin < end)
    {
        *bin = h2d[(unsigned char)*ph++] << 4;
        *bin++ |= h2d[(unsigned char)*ph++];
    }
}

void
bin_to_hex(const unsigned char *bin, const size_t bin_size, char *hex)
{
    const char *hex_chars = "0123456789abcdef";
    char *ph = hex;
    const unsigned char *pb = bin;
    for (size_t i=0; i<bin_size; i++)
    {
        *ph++ = hex_chars[(*pb >> 4) & 0xF];
        *ph++ = hex_chars[(*pb++) & 0xF];
    }
}

void
reverse_bin(unsigned char *bin, const size_t len)
{
    size_t start = 0;
    size_t end = len-1;
    unsigned char temp;
    while (start < end)
    {
        temp = bin[start];
        bin[start] = bin[end];
        bin[end] = temp;
        start++;
        end--;
    }
}

char *
stecpy(char *dst, const char *src, const char *end)
{
    while (*src && dst < end)
        *dst++ = *src++;
    if (dst < end)
        *dst = '\0';
    return dst;
}

char *
trim(char *str)
{
    char *start = str;
    char *end = str + strlen(str);

    while(*start && isspace(*start))
        start++;

    while(end > start && isspace(*(end - 1)))
        end--;

    *end = '\0';
    return start;
}

uint64_t
read_varint(const unsigned char *b)
{
    uint64_t r = 0;
    unsigned s = 0, c = 7;
    for(; r |= (uint64_t)(*b & 0x7f)<<s, (*b++ & 0x80) && c--; s+=7);
    return r;
}

unsigned char *
write_varint(unsigned char *b, uint64_t v)
{
    unsigned c = 7;
    for (; *b = v & 0x7f, (v >>= 7) && c--; *b++ |= 0x80);
    if (v) *b |= 0x80;
    return ++b;
}

