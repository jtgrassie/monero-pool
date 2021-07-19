#!/bin/bash

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

existing_pids=`ls $test_build_dir/*.pid`
for i in $existing_pids; do
    check_pid=`cat $i`
    process_count=`ps aux | grep $check_pid | grep -v grep | wc -l`
    if [ $process_count -gt 0 ]; then
        echo "Found running processes, please run kill_all_test_env_processes.sh"
        exit 1
    fi
done

test_branch=$1
build_type=$2

if [ -z "$1" ]; then
    test_branch="master"
fi

if [ -z "$2" ]; then
    build_type="debug"
fi
git_root=`git rev-parse --show-toplevel`
if [ -d $test_build_dir ]; then
    echo testbuild directory exists, cleaning, and creating
    rm -rf $test_build_dir
    mkdir $test_build_dir
else
    echo creating testbuild directory
    mkdir $test_build_dir
fi

mkdir $test_build_dir/data-dir

if [ ! -d $test_build_dir ]; then
    echo "$test_build_dir directory doesn't exist when it should"
    exit 1
fi

git clone $git_root $test_build_dir/monero-pool
cd $test_build_dir/monero-pool
git checkout $test_branch
if [ $build_type = 'release' ]; then
    make release
else
    make
fi

if [ $? -ne 0 ]; then
    echo "Build Failed!!"
    exit 1
fi

echo "Creating test pool.conf file"
echo > $test_build_dir/pool.conf
echo pool-list = 0.0.0.0 >> $test_build_dir/pool.conf
echo pool-port = $test_pool_port >> $test_build_dir/pool.conf
echo pool-ssl-port = $test_pool_port >> $test_build_dir/pool.conf
echo pool-syn-backlog = 16 >> $test_build_dir/pool.conf
echo webui-list = 0.0.0.0 >> $test_build_dir/pool.conf
echo webui-port = $test_pool_webui_port >> $test_build_dir/pool.conf
echo rpc-host = $monerod_ip >> $test_build_dir/pool.conf
echo rpc-port = $monerod_rpc_port >> $test_build_dir/pool.conf
echo wallet-rpc-host = $wallet_rpc_ip >> $test_build_dir/pool.conf
echo wallet-rpc-port = $wallet_rpc_port >> $test_build_dir/pool.conf
echo rpc-timeout = 15 >> $test_build_dir/pool.conf
echo idle-timeout = 150 >> $test_build_dir/pool.conf
echo pool-wallet = $test_pool_wallet_address >> $test_build_dir/pool.conf
echo pool-fee-wallet = $test_pool_fee_wallet_address >> $test_build_dir/pool.conf
echo pool-start-diff = 1000 >> $test_build_dir/pool.conf
echo pool-fixed-diff = 0 >> $test_build_dir/pool.conf
echo pool-nicehash-diff = 280000 >> $test_build_dir/pool.conf
echo pool-fee = 0.01 >> $test_build_dir/pool.conf
echo payment-threshold = 0.33 >> $test_build_dir/pool.conf
echo share-mul = 2.0 >> $test_build_dir/pool.conf
echo retarget-time = 30 >> $test_build_dir/pool.conf
echo retarget-ratio = 0.55 >> $test_build_dir/pool.conf
echo log-level = 5 >> $test_build_dir/pool.conf
echo log-file = $test_build_dir/test_pool.log >> $test_build_dir/pool.conf
echo data-dir = $test_build_dir/data-dir >> $test_build_dir/pool.conf
echo pid-file = $test_build_dir/test_pool.pid >> $test_build_dir/pool.conf
echo forked = 1 >> $test_build_dir/pool.conf
echo processes = 1 >> $test_build_dir/pool.conf
echo cull-shares = 10 >> $test_build_dir/pool.conf

echo "creating monero-wallet-rpc config file"

echo > $test_build_dir/monero_wallet_rpc.conf 
if [ -z "$nettype" ]; then
    echo "nettype not set"
    exit 1
fi
if [ $nettype == "stagenet" ]; then
    echo stagenet=1 >> $test_build_dir/monero_wallet_rpc.conf
elif [ $nettype == "testnet" ]; then
    echo testnet=1 >> $test_build_dir/monero_wallet_rpc.conf
fi
echo log-file=$test_build_dir/test_monero_wallet_rpc.log >> $test_build_dir/monero_wallet_rpc.conf
echo disable-rpc-login=1 >> $test_build_dir/monero_wallet_rpc.conf
echo daemon-address=$monerod_ip:$monerod_rpc_port >> $test_build_dir/monero_wallet_rpc.conf
echo trusted-daemon=1 >> $test_build_dir/monero_wallet_rpc.conf
echo pidfile=$test_build_dir/test_monero_wallet_rpc.pid >> $test_build_dir/monero_wallet_rpc.conf
echo wallet-file=$wallet_file >> $test_build_dir/monero_wallet_rpc.conf
echo password=$wallet_password >> $test_build_dir/monero_wallet_rpc.conf
echo rpc-bind-port=$wallet_rpc_port >> $test_build_dir/monero_wallet_rpc.conf
echo rpc-bind-ip=$wallet_rpc_ip >> $test_build_dir/monero_wallet_rpc.conf 
echo detach=1 >> $test_build_dir/monero_wallet_rpc.conf

echo "Launching monero-wallet-rpc"

monero-wallet-rpc --config-file $test_build_dir/monero_wallet_rpc.conf

rpc_pid=`cat $test_build_dir/test_monero_wallet_rpc.pid`
rpc_process=`ps aux | grep $rpc_pid | grep -v grep | wc -l`

if [ $rpc_process -ne 1 ]; then
    echo "failed to start monero-wallet-rpc"
    echo "Please review $test_build_dir/test_monero_wallet_rpc.log"
    exit 1
fi

sleep 5

max_retry=30
retry=0
while true; do
    curl -s http://$wallet_rpc_ip:$wallet_rpc_port/json_rpc -d '{"jsonrpc":"2.0","id":"0","method":"get_version"}' -H 'Content-Type: application/json' > /dev/null
    if [ $? -eq 0 ]; then
        break
    fi
    sleep 5
    retry=$[$retry + 1]
    if [ $retry -gt $max_retry ]; then
        echo "monero-wallet-rpc api did not start responding";
        echo "Please review $test_build_dir/test_monero_wallet_rpc.log"
        exit 1
    fi
done

cd build/$build_type
./monero-pool -c $test_build_dir/pool.conf

mp_pid=`cat $test_build_dir/test_pool.pid`
mp_process=`ps aux | grep $mp_pid | grep -v grep | wc -l`
echo $mp_process
if [ $mp_process -ne 1 ]; then
    echo "failed to start monero-pool"
    echo "please review $test_build_dir/test_pool.log"
    kill $rpc_pid
    exit 1
fi   

retry=0
while true; do
	curl -s http://127.0.0.1:$test_pool_webui_port/stats > /dev/null
	if [ $? -eq 0 ]; then
		break
	fi
	sleep 5
	retry=$[retry + 1]
	if [ $retry -gt $max_retry ]; then
		echo "monero-pool webui services not responding"
		echo "please review $test_build_dir/test_pool.log"
		kill $rpc_pid
		kill $mp_pid
		exit 1
	fi
done

echo "Pool launch success"
echo "Pool pid $mp_pid wallet_pid $rpc_pid"
