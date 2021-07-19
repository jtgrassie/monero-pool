#!/usr/bin/env python

import requests
from testlib.util import load_env, get_env_file

def test_webui_stats():
    ENV = load_env(get_env_file())
    port=ENV['test_pool_webui_port']
    uri = "http://127.0.0.1:{}/stats".format(port)
    r = requests.get(uri)
    r.raise_for_status()
    assert r.status_code == 200
    assert r.headers['content-type'] == "application/json"
    data = r.json()
    int_list = [
        'pool_hashrate',
        'round_hashes',
        'network_hashrate',
        'network_height',
        'last_template_fetched',
        'last_block_found',
        'pool_blocks_found',
        'pool_port',
        'pool_ssl_port',
        'allow_self_select',
        'connected_miners',
        'miner_hashrate',
        'worker_count'
    ]
    float_list = [
        'pool_fee',
        'miner_balance',
        'payment_threshold'
    ]
    for key in int_list:
        assert key in data.keys()
        assert isinstance(data[key], int)
    assert 'miner_hashrate_stats' in data.keys()
    assert isinstance(data['miner_hashrate_stats'], list)
    for key in float_list:
        assert key in data.keys()
        assert isinstance(data[key], float)
    assert data['miner_hashrate_stats'] == [0,0,0,0,0,0]

def test_webui_stats_with_wa():
    ENV = load_env(get_env_file())
    port=ENV['test_pool_webui_port']
    cookies = {'wa': ENV['test_pool_fee_wallet_address']}
    uri = "http://127.0.0.1:{}/stats".format(port)
    r = requests.get(uri, cookies=cookies)
    r.raise_for_status()
    assert r.headers['content-type'] == "application/json"
    assert r.status_code == 200

def test_webui_pool_page():
    ENV = load_env(get_env_file())
    port=ENV['test_pool_webui_port']
    uri = "http://127.0.0.1:{}/".format(port)
    r = requests.get(uri)
    r.raise_for_status
    assert r.headers['content-type'] == "text/html"
    assert r.status_code == 200

def test_webui_workers():
    ENV = load_env(get_env_file())
    port=ENV['test_pool_webui_port']
    uri = "http://127.0.0.1:{}/workers".format(port)
    r = requests.get(uri)
    r.raise_for_status
    assert r.headers['content-type'] == "application/json"
    assert r.status_code == 200
    data = r.json()
    assert isinstance(data, list)