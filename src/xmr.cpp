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

#include "cryptonote_basic/cryptonote_basic.h"
#include "cryptonote_basic/cryptonote_format_utils.h"
#include "cryptonote_basic/blobdatatype.h"
#include "cryptonote_basic/difficulty.h"
#include "crypto/crypto.h"
#include "crypto/hash.h"
#include "common/base58.h"
#include "serialization/binary_utils.h"

#include "xmr.h"

using namespace cryptonote;

int get_hashing_blob(const char *input, const size_t in_size, char **output, size_t *out_size)
{
    block b = AUTO_VAL_INIT(b);
    blobdata bd = std::string(input, in_size);    
    if (!parse_and_validate_block_from_blob(bd, b))
    {
        printf("Failed to parse block\n");
        return -1;
    }

    blobdata blob = get_block_hashing_blob(b);
    *out_size = blob.length();
    *output = (char*) malloc(*out_size);
    memcpy(*output, blob.data(), *out_size);
    return 0;
}

int construct_block_blob(const char *block_data, uint64_t nonce, char **blob)
{
    struct block b = AUTO_VAL_INIT(b);
    blobdata bd = block_data;
    if (!parse_and_validate_block_from_blob(bd, b))
        return -1;
    b.nonce = nonce;
    bd = "";
    if (!block_to_blob(b, bd))
        return -2;
    *blob = (char*) malloc(bd.size());
    memcpy(*blob, bd.data(), bd.length());
    return 0;
}

int parse_address(const char *input, uint64_t *prefix)
{
    uint64_t tag;
    std::string decoded;
    bool rv = tools::base58::decode_addr(input, tag, decoded);
    if (rv)
    {
        *prefix = tag;
    }
    return rv ? 0 : -1;
}

void get_hash(const char *input, const size_t in_size, char **output)
{
    crypto::cn_slow_hash(input, in_size, reinterpret_cast<crypto::hash&>(*output), 1);
}

bool check_hash(const char* hash, uint64_t difficulty)
{
    return check_hash(reinterpret_cast<const crypto::hash&>(hash), difficulty);
}

