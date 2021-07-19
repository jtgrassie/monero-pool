#!/usr/bin/env python

import socket
import json
import requests
from uuid import uuid4
from time import sleep
from pytest import skip
from os.path import exists
from testlib.util import load_env, get_env_file
from subprocess import Popen, PIPE

def test_miner_connect():
    ENV=load_env(get_env_file())
    port = ENV['test_pool_port']
    host = "127.0.0.1"
    pool_socket = socket.socket()
    pool_socket.connect((host, int(port)))
    json_object = {
        "id":1,
        "jsonrpc":"2.0",
        "method":"login",
        "params":{
            "login":ENV['test_pool_fee_wallet_address'],
            "pass":"d=300000",
            "agent": "monero-pool integration test",
            "rigid":"monero-pool_int_test",
            "algo":["rx/0"]
            }
        }
    json_string = json.dumps(json_object)
    json_string = json_string + "\n"
    raw_data = bytes(json_string, 'utf-8')
    pool_socket.sendall(raw_data)
    response_data = pool_socket.recv(4096)
    pool_socket.shutdown(socket.SHUT_RDWR)
    pool_socket.close()
    json_object = json.loads(str(response_data, 'utf-8'))
    key_check = ['id', 'jsonrpc', 'error', 'result']
    for key in key_check:
        assert key in json_object.keys()
    assert isinstance(json_object['result'], dict)
    assert isinstance(json_object['id'], int)
    assert json_object['jsonrpc'] == "2.0"
    assert json_object['error'] == None
    key_check = ['id', 'job', 'status']
    for key in key_check:
        assert key in json_object['result'].keys()
    assert isinstance(json_object['result']['job'], dict)
    key_check = ['blob', 'job_id', 'target', 'height', 'seed_hash', 'next_seed_hash']
    for key in key_check:
        assert key in json_object['result']['job'].keys()
    assert json_object['result']['status'] == "OK"

def test_actual_mining():
    ENV=load_env(get_env_file())
    port = ENV['test_pool_port']
    wallet = ENV['test_pool_fee_wallet_address']
    webui_port = ENV['test_pool_webui_port']
    proc = Popen("which xmrig", stdout=PIPE, shell=True)
    (xmrig_output, err) = proc.communicate()
    xmrig_cmd = str(xmrig_output, 'utf-8').strip()
    assert err == None
    assert len(xmrig_cmd) > 0
    assert exists(xmrig_cmd)
    xmrig_uuid = uuid4()
    full_cmd = "{} --log-file={}/xmrig-{}.log --algo rx/0 -u {} -o localhost:{} -p d=1000 --rig-id monero_pool_int_test".format(xmrig_cmd, ENV['test_build_dir'], xmrig_uuid, wallet, port)
    proc = Popen(full_cmd, stdout=PIPE, shell=True)
    pid = proc.pid
    pid_file_path = "{}/xmrig-{}.pid".format(ENV['test_build_dir'], xmrig_uuid)
    pid_fh = open(pid_file_path, 'w')
    pid_fh.write(str(pid))
    pid_fh.close()
    sleep(60)
    uri = "http://localhost:{}/stats".format(webui_port)
    cookies = {'wa': wallet}
    r = requests.get(uri, cookies=cookies)
    r.raise_for_status()
    proc = Popen("/usr/bin/kill {}".format(pid), stdout=PIPE, shell=True)
    assert r.status_code == 200
    assert r.headers['content-type'] == "application/json"
    json_object = r.json()
    assert json_object['worker_count'] > 0
    assert json_object['connected_miners'] > 0
    assert json_object['miner_hashrate'] > 0

def test_mine_block():
    ENV=load_env(get_env_file())
    if ENV['nettype'] == "mainnet":
        skip("Do not try mining a block on mainnet")
    port=ENV['test_pool_port']
    wallet=ENV['test_pool_fee_wallet_address']
    proc = Popen("which xmrig", stdout=PIPE, shell=True)
    (xmrig_output, err) = proc.communicate()
    xmrig_cmd = str(xmrig_output, 'utf-8').strip()
    assert err == None
    assert len(xmrig_cmd) > 0
    assert exists(xmrig_cmd)
    xmrig_uuid = uuid4()
    full_cmd = "{} --log-file={}/xmrig-{}.log --algo rx/0 -u {} -o localhost:{} -p d=300000 --rig-id monero_pool_int_test".format(xmrig_cmd, ENV['test_build_dir'], xmrig_uuid, wallet, port)
    proc = Popen(full_cmd, stdout=PIPE, shell=True)
    pid = proc.pid
    pid_file_path = "{}/xmrig-{}.pid".format(ENV['test_build_dir'], xmrig_uuid)
    pid_fh = open(pid_file_path, 'w')
    pid_fh.write(str(pid))
    pid_fh.close()
    webui_port = ENV['test_pool_webui_port']
    uri = "http://localhost:{}/stats".format(webui_port)
    r = requests.get(uri)
    r.raise_for_status()
    assert r.status_code == 200
    assert r.headers['content-type'] == "application/json"
    json_object = r.json()
    pool_blocks_found = json_object['pool_blocks_found']
    retry = 0
    max_retry = 60
    while True:
        r = requests.get(uri)
        r.raise_for_status()
        assert r.status_code == 200
        assert r.headers['content-type'] == "application/json"
        json_object = r.json()
        new_count = json_object['pool_blocks_found']
        if new_count > pool_blocks_found:
            break
        retry = retry + 1
        if retry > max_retry:
            proc = Popen("/usr/bin/kill {}".format(pid), stdout=PIPE, shell=True)
            assert False
        sleep(60)
    proc = Popen("/usr/bin/kill {}".format(pid), stdout=PIPE, shell=True)
    assert new_count > pool_blocks_found
