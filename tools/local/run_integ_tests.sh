#!/bin/bash

function print_mining_warning {
    echo '!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!'
    echo '!!! WARNING WARNING WARNING WARNING !!!'
    echo '!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!'
    echo
    echo "WARN This will launch xmrig and attempt to mine a block"
    echo "WARN This will considerably tax your build system"
    echo "WARN If you are on a cloud REVIEW your EULA running any"
    echo "WARN miner maybe seen as a violation of the EULA"
    echo "WARN and be grounds to terminate your account"
    echo
    echo "If you want to speed up the blockfind, connect your own HR to pool"
    echo "with a fixed difficulty of 300000 a hashrate of ~1000"
    echo "should find a block within the testing window"
}

if [ ! -f ./.env ]; then
    echo "please run ./create_test_env.sh"
    exit 1
fi

source ./.env

activate_script="./testenv/bin/activate"
if [ ! -f $activate_script ]; then
    echo "please run ./create_test_env.sh"
    exit 1
fi

source $activate_script

git_root=`git rev-parse --show-toplevel`

cd $git_root/test/integ

if [ $nettype = 'stagenet' ]; then
    print_mining_warning
fi

if [ $nettype = 'testnet' ]; then
    print_mining_warning
fi

if [ -z "$1" ]; then
    pytest 
else
    pytest $1
fi

# kill any stray xmrig processes that have not respected our kills
xmrigs=`ps aux | grep xmrig | grep -v grep | grep $test_build_dir`
if [ -z "$xmrigs" ]; then
    echo "killing xmrigs"
    echo $xmrigs
fi
for pid in `ps aux | grep xmrig | grep -v grep | grep $test_build_dir | awk '{print $2}'`; do
    kill $pid
done
