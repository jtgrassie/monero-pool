/*
Copyright (c) 2014-2019, The Monero Project

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

Parts of the project are originally copyright (c) 2012-2013 The Cryptonote
developers.
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
#include "serialization/binary_utils.h"
#include "ringct/rctSigs.h"
#include "common/base58.h"
#include "common/util.h"
#include "string_tools.h"

#include "xmr.h"

using namespace epee::string_tools;
using namespace cryptonote;
using namespace crypto;

int get_hashing_blob(const unsigned char *input, const size_t in_size,
        unsigned char **output, size_t *out_size)
{
    block b = AUTO_VAL_INIT(b);
    blobdata bd = std::string((const char*)input, in_size);
    if (!parse_and_validate_block_from_blob(bd, b))
    {
        return XMR_PARSE_ERROR;
    }

    blobdata blob = get_block_hashing_blob(b);
    *out_size = blob.length();
    *output = (unsigned char*) malloc(*out_size);
    memcpy(*output, blob.data(), *out_size);
    return XMR_NO_ERROR;
}

int parse_address(const char *input, uint64_t *prefix,
        unsigned char *pub_spend)
{
    uint64_t tag;
    std::string decoded;
    bool rv = tools::base58::decode_addr(input, tag, decoded);
    if (rv)
    {
        *prefix = tag;
        if (pub_spend != NULL)
        {
            account_public_address address;
            ::serialization::parse_binary(decoded, address);
            public_key S = address.m_spend_public_key;
            memcpy(pub_spend, &S, 32);
        }
    }
    return rv ? XMR_NO_ERROR : XMR_PARSE_ERROR;
}

void get_hash(const unsigned char *input, const size_t in_size,
        unsigned char *output, int variant, uint64_t height)
{
    cn_slow_hash(input, in_size,
            reinterpret_cast<hash&>(*output), variant, height);
}

void get_rx_hash(const unsigned char *input, const size_t in_size,
        unsigned char *output, const unsigned char *seed_hash,
        const uint64_t height)
{
#ifdef HAVE_RX
    static unsigned max_concurrency = tools::get_max_concurrency();
    uint64_t seed_height;
    if (rx_needhash(height, &seed_height))
    {
        rx_seedhash(seed_height, (const char*)seed_hash, max_concurrency);
    }
    rx_slow_hash((const char*)input, in_size, (char*)output, max_concurrency);
#endif
}

int validate_block_from_blob(const char *blob_hex,
        const unsigned char *sec_view,
        const unsigned char *pub_spend)
{
    /*
      The only validation needed is that the data parses to a block and the
      miner tx only pays out to the pool.
    */
    block b = AUTO_VAL_INIT(b);
    blobdata bd;
    secret_key v;
    public_key S;
    memcpy(&unwrap(v), sec_view, 32);
    memcpy(&S, pub_spend, 32);

    if (!parse_hexstr_to_binbuff(blob_hex, bd))
        return XMR_PARSE_ERROR;

    if (!parse_and_validate_block_from_blob(bd, b))
        return XMR_PARSE_ERROR;

    transaction tx = b.miner_tx;

    /*
      Ensure we have only one in, one out and in is gen.
    */
    if (tx.vin.size() != 1)
        return XMR_VIN_COUNT_ERROR;

    if (tx.vout.size() != 1)
        return XMR_VOUT_COUNT_ERROR;

    if (tx.vin[0].type() != typeid(txin_gen))
        return XMR_VIN_TYPE_ERROR;

    /*
      Ensure that the miner tx single output key is destined for the pool
      wallet.

      Don't bother checking any additional pub keys in tx extra. The daemon
      created miner tx only has one public key in extra. If we can't derive
      from the first (which should be only) found, reject.
    */
    std::vector<tx_extra_field> tx_extra_fields;
    parse_tx_extra(tx.extra, tx_extra_fields);
    tx_extra_pub_key pub_key_field;
    if (!find_tx_extra_field_by_type(tx_extra_fields, pub_key_field, 0))
        return XMR_TX_EXTRA_ERROR;
    public_key R = pub_key_field.pub_key;
    public_key P = boost::get<txout_to_key>(tx.vout[0].target).key;
    key_derivation derivation;
    generate_key_derivation(R, v, derivation);
    public_key derived;
    derive_subaddress_public_key(P, derivation, 0, derived);
    if (derived != S)
        return XMR_MISMATCH_ERROR;

    return XMR_NO_ERROR;
}

