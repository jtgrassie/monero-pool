#!/usr/bin/env python

from os.path import exists

def read_config():
    config_items = [
        "monerod_ip",
        "monerod_rpc_port",
        "wallet_rpc_ip",
        "wallet_rpc_port",
        "wallet_file",
        "wallet_password",
        "nettype",
        "test_pool_port",
        "test_pool_webui_port",
        "test_pool_wallet_address",
        "test_pool_fee_wallet_address",
        "test_build_dir"
    ]

    config_answers = {}

    for key in config_items:
        if key == "test_build_dir":
            print("this path should be outside of the git repo pathspace")
        config_answers[key] = input("{}: ".format(key))

    return config_answers

def write_dot_env(config_answers):
    fh = open(".env", 'w')
    for key in config_answers.keys():
        fh.write('{}="{}"\n'.format(key, config_answers[key]))
    fh.close()

if __name__ == "__main__":
    print("!!! PLEASE READ !!!")
    print("This expects you to have a running mainnet/stagenet/testnet monerod")
    print("and the monero-wallet-rpc and xmrig commands available in PATH")
    print("You will also need to create wallets for the enviroments to use")
    print("!!!!!!!!!!!!!!!!!!!")
    if exists(".env"):
        print("--- You have an existing environment ---")
        with open(".env", 'r') as fh:
            existing_env = fh.read()
        print(existing_env)
        print ("---")
        retry = 0 
        max_retry = 30
        while True:
            yn = input("keep? [(y)/n] ")
            if yn.lower() in ['', 'y']:
                exit(0)
            if yn.lower() in ['n']:
                break
            retry = retry + 1
            if retry > max_retry:
                print("I remind you, this is a yes or no question")
                exit(1)
    write_dot_env(read_config())

