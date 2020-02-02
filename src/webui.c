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

#define __STDC_FORMAT_MACROS
#include <inttypes.h>

#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include <pthread.h>

#include <event2/event.h>
#include <event2/buffer.h>
#include <event2/http.h>

#include "log.h"
#include "pool.h"
#include "webui.h"

extern unsigned char webui_html[];
extern unsigned int webui_html_len;

static pthread_t handle;
static struct event_base *webui_base;
static struct evhttp *webui_httpd;
static struct evhttp_bound_socket *webui_listener;


static void
send_json_stats(struct evhttp_request *req, void *arg)
{
    struct evbuffer *buf = evhttp_request_get_output_buffer(req);
    wui_context_t *context = (wui_context_t*) arg;
    struct evkeyvalq *hdrs_in = NULL;
    struct evkeyvalq *hdrs_out = NULL;
    uint64_t ph = context->pool_stats->pool_hashrate;
    uint64_t nh = context->pool_stats->network_hashrate;
    uint64_t nd = context->pool_stats->network_difficulty;
    uint64_t height = context->pool_stats->network_height;
    uint64_t ltf = context->pool_stats->last_template_fetched;
    uint64_t lbf = context->pool_stats->last_block_found;
    uint32_t pbf = context->pool_stats->pool_blocks_found;
    uint64_t rh = context->pool_stats->round_hashes;
    unsigned ss = context->allow_self_select;
    uint64_t mh = 0;
    double mb = 0.0;

    hdrs_in = evhttp_request_get_input_headers(req);
    const char *cookies = evhttp_find_header(hdrs_in, "Cookie");
    if (cookies)
    {
        char *wa = strstr(cookies, "wa=");
        if (wa)
        {
            wa += 3;
            mh = miner_hr(wa);
            uint64_t balance = miner_balance(wa);
            mb = (double) balance / 1000000000000.0;
        }
    }

    evbuffer_add_printf(buf, "{"
            "\"pool_hashrate\":%"PRIu64","
            "\"round_hashes\":%"PRIu64","
            "\"network_hashrate\":%"PRIu64","
            "\"network_difficulty\":%"PRIu64","
            "\"network_height\":%"PRIu64","
            "\"last_template_fetched\":%"PRIu64","
            "\"last_block_found\":%"PRIu64","
            "\"pool_blocks_found\":%d,"
            "\"payment_threshold\":%.2f,"
            "\"pool_fee\":%.3f,"
            "\"pool_port\":%d,"
            "\"pool_ssl_port\":%d,"
            "\"allow_self_select\":%u,"
            "\"connected_miners\":%d,"
            "\"miner_hashrate\":%"PRIu64","
            "\"miner_balance\":%.8f"
            "}", ph, rh, nh, nd, height, ltf, lbf, pbf,
            context->payment_threshold, context->pool_fee,
            context->pool_port, context->pool_ssl_port,
            ss, context->pool_stats->connected_miners,
            mh, mb);
    hdrs_out = evhttp_request_get_output_headers(req);
    evhttp_add_header(hdrs_out, "Content-Type", "application/json");
    evhttp_send_reply(req, HTTP_OK, "OK", buf);
}

static void
process_request(struct evhttp_request *req, void *arg)
{
    const char *url = evhttp_request_get_uri(req);
    struct evbuffer *buf = NULL;
    struct evkeyvalq *hdrs_out = NULL;

    if (strstr(url, "/stats") != NULL)
    {
        send_json_stats(req, arg);
        return;
    }

    buf = evhttp_request_get_output_buffer(req);
    evbuffer_add(buf, webui_html, webui_html_len);
    hdrs_out = evhttp_request_get_output_headers(req);
    evhttp_add_header(hdrs_out, "Content-Type", "text/html");
    evhttp_send_reply(req, HTTP_OK, "OK", buf);
}

static void *
thread_main(void *ctx)
{
    wui_context_t *context = (wui_context_t*) ctx;
    webui_listener = evhttp_bind_socket_with_handle(
            webui_httpd, "0.0.0.0", context->port);
    if(!webui_listener)
    {
        log_error("Failed to bind for port: %u", context->port);
        return 0;
    }
    evhttp_set_gencb(webui_httpd, process_request, ctx);
    event_base_dispatch(webui_base);
    event_base_free(webui_base);
    return 0;
}

int
start_web_ui(wui_context_t *context)
{
    log_debug("Starting Web UI");
    if (webui_base || handle)
    {
        log_error("Already running");
        return -1;
    }
    webui_base = event_base_new();
    if (!webui_base)
    {
        log_error("Failed to create httpd event base");
        return -1;
    }
    webui_httpd = evhttp_new(webui_base);
    if (!webui_httpd)
    {
        log_error("Failed to create evhttp event");
        return -1;
    }
    int rc = pthread_create(&handle, NULL, thread_main, context);
    if (!rc)
        pthread_detach(handle);
    return rc;
}

void
stop_web_ui(void)
{
    log_debug("Stopping Web UI");
    if (webui_listener && webui_httpd)
        evhttp_del_accept_socket(webui_httpd, webui_listener);
    if (webui_httpd)
        evhttp_free(webui_httpd);
    if (webui_base)
        event_base_loopbreak(webui_base);
}

