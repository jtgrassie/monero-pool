/*
  Copyright (c) 2014-2018, The Monero Project

  All rights reserved.

  Redistribution and use in source and binary forms, with or without modification, are
  permitted provided that the following conditions are met:

  1. Redistributions of source code must retain the above copyright notice, this list of
     conditions and the following disclaimer.

  2. Redistributions in binary form must reproduce the above copyright notice, this list
     of conditions and the following disclaimer in the documentation and/or other
     materials provided with the distribution.

  3. Neither the name of the copyright holder nor the names of its contributors may be
     used to endorse or promote products derived from this software without specific
     prior written permission.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
  EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
  MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
  THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
  PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
  STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
  THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

  Parts of the project are originally copyright (c) 2012-2013 The Cryptonote developers
*/

#include <assert.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

#include "util.h"

void
hex_to_bin(const char *hex, char *bin, size_t bin_size)
{
    size_t len = strlen(hex);
    assert(len % 2 == 0);
    assert(bin_size >= len / 2);
    const char *ph = hex;
    while (*ph)
    {
        sscanf(ph, "%2hhx", bin++);    
        ph += 2;
    }
}

void
bin_to_hex(const char *bin, size_t bin_size, char *hex)
{
    const char *hex_chars = "0123456789abcdef";
    char *ph = hex;
    const char *pb = bin;
    for (size_t i=0; i<bin_size; i++)
    {
        *ph++ = hex_chars[(*pb >> 4) & 0xF];
        *ph++ = hex_chars[(*pb++) & 0xF];
    }
}

void
reverse_hex(char *hex, size_t len)
{
    assert(len % 2 == 0);
    size_t start = 0;
    size_t end = len-2;
    char temp[2];
    while (start < end)
    {
        memcpy(&temp[0], &hex[start], 2);
        memcpy(&hex[start], &hex[end], 2);
        memcpy(&hex[end], &temp[0], 2);
        start += 2;
        end -= 2;
    }
}

void
reverse_bin(char *bin, size_t len)
{
    size_t start = 0;
    size_t end = len-1;
    char temp;
    while (start < end)
    {
        temp = bin[start];
        bin[start] = bin[end];
        bin[end] = temp;
        start++;
        end--;
    }
}

