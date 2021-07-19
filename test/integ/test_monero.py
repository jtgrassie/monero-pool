#!/usr/bin/env python

from monero.wallet import Wallet
from monero.backends.jsonrpc import JSONRPCWallet
from monero.numbers import Decimal
from testlib.util import load_env, get_env_file

def test_pool_wallet_rpc():
    ENV=load_env(get_env_file())
    host=ENV['wallet_rpc_ip']
    port=ENV['wallet_rpc_port']
    w = Wallet(JSONRPCWallet(host=host, port=port))
    assert w.address() == ENV['test_pool_wallet_address']
    assert isinstance(w.balance(), Decimal)