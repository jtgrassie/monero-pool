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

#ifndef XMR_H
#define XMR_H

#ifdef __cplusplus
extern "C" {
#endif

enum xmr_error
{
    XMR_NO_ERROR         = 0,
    XMR_PARSE_ERROR      = -1,
    XMR_VIN_COUNT_ERROR  = -2,
    XMR_VOUT_COUNT_ERROR = -3,
    XMR_VIN_TYPE_ERROR   = -4,
    XMR_TX_EXTRA_ERROR   = -5,
    XMR_MISMATCH_ERROR   = -6
};

int get_hashing_blob(const unsigned char *input, const size_t in_size,
        unsigned char **output, size_t *out_size);
int parse_address(const char *input, uint64_t *prefix,
        uint8_t *nettype, unsigned char *pub_spend);
int is_integrated(uint64_t prefix);
int get_block_hash(const unsigned char *input, const size_t in_size,
        unsigned char *output);
void get_hash(const unsigned char *input, const size_t in_size,
        unsigned char *output, int variant, uint64_t height);
void set_rx_main_seedhash(const unsigned char *seed_hash);
void get_rx_hash(const unsigned char *seed_hash, const unsigned char *input,
        const size_t in_size, unsigned char *output);
int validate_block_from_blob(const char *blob_hex,
        const unsigned char *sec_view,
        const unsigned char *pub_spend);

#ifdef __cplusplus
}
#endif

#endif
