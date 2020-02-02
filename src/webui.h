/*
Copyright (c) 2014-2020, The Monero Project

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

#ifndef WEBUI_H
#define WEBUI_H

typedef struct pool_stats_t
{
    uint64_t network_difficulty;
    uint64_t network_hashrate;
    uint64_t network_height;
    uint32_t connected_miners;
    uint64_t pool_hashrate;
    uint64_t round_hashes;
    uint32_t pool_blocks_found;
    time_t last_block_found;
    time_t last_template_fetched;
} pool_stats_t;

typedef struct wui_context_t
{
    uint32_t port;
    pool_stats_t *pool_stats;
    double pool_fee;
    double payment_threshold;
    uint32_t pool_port;
    uint32_t pool_ssl_port;
    unsigned allow_self_select;
} wui_context_t;

int start_web_ui(wui_context_t *context);
void stop_web_ui(void);

#endif
