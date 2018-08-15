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

#include <sys/types.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <microhttpd.h>
#include <pthread.h>

#include <lmdb.h>

#include "log.h"
#include "pool.h"
#include "webui.h"

#define TAG_MAX 17
#define PAGE_MAX 4096

extern unsigned char webui_html[];
extern unsigned int webui_html_len;

static struct MHD_Daemon *mhd_daemon;
static char page_buffer[PAGE_MAX];
static char MINERS_CONNECTED[] = "{{MINERS_CONNECTED}}";
static char LAST_BLOCK_FOUND[] = "{{LAST_BLOCK_FOUND}}";
static char NETWORK_HASHRATE[] = "{{NETWORK_HASHRATE}}";
static char POOL_HASHRATE[] = "{{POOL_HASHRATE}}";
static char MINER_HASHRATE[] = "{{MINER_HASHRATE}}";
static char POOL_BLOCKS_FOUND[] = "{{POOL_BLOCKS_FOUND}}";
static char PAYMENT_THRESHOLD[] = "{{PAYMENT_THRESHOLD}}";
static char POOL_FEE[] = "{{POOL_FEE}}";
static char MINER_BALANCE_DUE[] = "{{MINER_BALANCE_DUE}}";


static void
format_hashrate(uint64_t hashrate, char* output, size_t len)
{
    if (hashrate < 1000)
        snprintf(output, len, "%d H/s", (int) hashrate);
    else if (hashrate < 1000000)
        snprintf(output, len, "%.2f KH/s", (double) hashrate / 1000.0);
    else if (hashrate < 1000000000000)
        snprintf(output, len, "%.2f MH/s", (double) hashrate / 1000000.0);
    else
        snprintf(output, len, "%.2f GH/s", (double) hashrate / 1000000000000.0);
}

int
answer_to_connection (void *cls, struct MHD_Connection *connection,
        const char *url,
        const char *method, const char *version,
        const char *upload_data,
        size_t *upload_data_size, void **con_cls)
{
    static char temp[TAG_MAX];
    struct MHD_Response *response;
    int ret;
    wui_context_t *context = (wui_context_t*) cls;

    memset(page_buffer, 0, PAGE_MAX);
    memcpy(page_buffer, webui_html, webui_html_len);

    char *p = strstr(page_buffer, MINERS_CONNECTED);
    memset(p, ' ', strlen(MINERS_CONNECTED));
    sprintf(temp, "%d", context->pool_stats->connected_miners);
    memcpy(p, temp, strlen(temp));

    time_t now = time(NULL);
    double diff = difftime(now, context->pool_stats->last_block_found);
    if (context->pool_stats->last_block_found == 0)
        snprintf(temp, TAG_MAX, "None yet");
    else if (diff < 60)
        snprintf(temp, TAG_MAX, "%d seconds ago", (int) diff);
    else if (diff < 3600)
        snprintf(temp, TAG_MAX, "%d minutes ago", (int) diff / 60);
    else if (diff < 86400)
        snprintf(temp, TAG_MAX, "%d hours ago", (int) diff / 3600);
    else
        snprintf(temp, TAG_MAX, "%d days ago", (int) diff / 86400);
    p = strstr(page_buffer, LAST_BLOCK_FOUND);
    memset(p, ' ', strlen(LAST_BLOCK_FOUND));
    memcpy(p, temp, strlen(temp));

    uint64_t nh = context->pool_stats->network_hashrate;
    format_hashrate(nh, temp, TAG_MAX);
    p = strstr(page_buffer, NETWORK_HASHRATE);
    memset(p, ' ', strlen(NETWORK_HASHRATE));
    memcpy(p, temp, strlen(temp));

    uint64_t ph = context->pool_stats->pool_hashrate;
    format_hashrate(ph, temp, TAG_MAX);
    p = strstr(page_buffer, POOL_HASHRATE);
    memset(p, ' ', strlen(POOL_HASHRATE));
    memcpy(p, temp, strlen(temp));

    sprintf(temp, "%d", context->pool_stats->pool_blocks_found);
    p = strstr(page_buffer, POOL_BLOCKS_FOUND);
    memset(p, ' ', strlen(POOL_BLOCKS_FOUND));
    memcpy(p, temp, strlen(temp));

    p = strstr(page_buffer, PAYMENT_THRESHOLD);
    memset(p, ' ', strlen(PAYMENT_THRESHOLD));
    sprintf(temp, "%.2f", context->payment_threshold);
    memcpy(p, temp, strlen(temp));

    p = strstr(page_buffer, POOL_FEE);
    memset(p, ' ', strlen(POOL_FEE));
    sprintf(temp, "%.2f", context->pool_fee);
    memcpy(p, temp, strlen(temp));

    const char *wa = MHD_lookup_connection_value(connection, MHD_COOKIE_KIND, "wa");
    if (wa != NULL)
    {
        uint64_t mh = miner_hr(wa);
        format_hashrate(mh, temp, TAG_MAX);
        p = strstr(page_buffer, MINER_HASHRATE);
        memset(p, ' ', strlen(MINER_HASHRATE));
        memcpy(p, temp, strlen(temp));

        uint64_t balance = miner_balance(wa);
        p = strstr(page_buffer, MINER_BALANCE_DUE);
        memset(p, ' ', strlen(MINER_BALANCE_DUE));
        sprintf(temp, "%.8f", (double) balance / 1000000000000.0);
        memcpy(p, temp, strlen(temp));
    }

    response = MHD_create_response_from_buffer(strlen(page_buffer),
            (void*) page_buffer, MHD_RESPMEM_PERSISTENT);
    ret = MHD_queue_response(connection, MHD_HTTP_OK, response);
    MHD_destroy_response(response);
    return ret;
}

int
start_web_ui(wui_context_t *context)
{
    log_debug("Starting Web UI");
    mhd_daemon = MHD_start_daemon(MHD_USE_SELECT_INTERNALLY, context->port, NULL, NULL,
            &answer_to_connection, (void*) context, MHD_OPTION_END);
    return mhd_daemon != NULL ? 0 : -1;
}

void
stop_web_ui()
{
    log_debug("Stopping Web UI");
    if (mhd_daemon != NULL)
        MHD_stop_daemon(mhd_daemon);
}

