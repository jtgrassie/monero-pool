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

#define __STDC_FORMAT_MACROS
#include <inttypes.h>

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
#define JSON_MAX 512

extern unsigned char webui_html[];
extern unsigned int webui_html_len;

static struct MHD_Daemon *mhd_daemon;

int
send_json_stats (void *cls, struct MHD_Connection *connection)
{
    struct MHD_Response *response;
    int ret;
    wui_context_t *context = (wui_context_t*) cls;
    char json[JSON_MAX];
    uint64_t ph = context->pool_stats->pool_hashrate;
    uint64_t nh = context->pool_stats->network_hashrate;
    uint64_t height = context->pool_stats->network_height;
    uint64_t ltf = context->pool_stats->last_template_fetched;
    uint64_t lbf = context->pool_stats->last_block_found;
    uint32_t pbf = context->pool_stats->pool_blocks_found;
    uint64_t mh = 0;
    double mb = 0.0;
    const char *wa = MHD_lookup_connection_value(connection,
            MHD_COOKIE_KIND, "wa");
    if (wa != NULL)
    {
        mh = miner_hr(wa);
        uint64_t balance = miner_balance(wa);
        mb = (double) balance / 1000000000000.0;
    }
    snprintf(json, JSON_MAX, "{"
            "\"pool_hashrate\":%"PRIu64","
            "\"network_hashrate\":%"PRIu64","
            "\"network_height\":%"PRIu64","
            "\"last_template_fetched\":%"PRIu64","
            "\"last_block_found\":%"PRIu64","
            "\"pool_blocks_found\":%d,"
            "\"payment_threshold\":%.2f,"
            "\"pool_fee\":%.3f,"
            "\"pool_port\":%d,"
            "\"connected_miners\":%d,"
            "\"miner_hashrate\":%"PRIu64","
            "\"miner_balance\":%.8f"
            "}", ph, nh, height, ltf, lbf, pbf,
            context->payment_threshold, context->pool_fee,
            context->pool_port, context->pool_stats->connected_miners,
            mh, mb);
    response = MHD_create_response_from_buffer(strlen(json),
            (void*) json, MHD_RESPMEM_MUST_COPY);
    ret = MHD_queue_response(connection, MHD_HTTP_OK, response);
    MHD_destroy_response(response);
    return ret;
}

int
answer_to_connection (void *cls, struct MHD_Connection *connection,
        const char *url,
        const char *method, const char *version,
        const char *upload_data,
        size_t *upload_data_size, void **con_cls)
{
    if (strstr(url, "/stats") != NULL)
        return send_json_stats(cls, connection);

    struct MHD_Response *response;
    response = MHD_create_response_from_buffer(webui_html_len,
            (void*) webui_html, MHD_RESPMEM_PERSISTENT);
    int ret = MHD_queue_response(connection, MHD_HTTP_OK, response);
    MHD_destroy_response(response);
    return ret;
}

int
start_web_ui(wui_context_t *context)
{
    log_debug("Starting Web UI");
    mhd_daemon = MHD_start_daemon(MHD_USE_SELECT_INTERNALLY,
            context->port, NULL, NULL,
            &answer_to_connection, (void*) context, MHD_OPTION_END);
    return mhd_daemon != NULL ? 0 : -1;
}

void
stop_web_ui(void)
{
    log_debug("Stopping Web UI");
    if (mhd_daemon != NULL)
        MHD_stop_daemon(mhd_daemon);
}

