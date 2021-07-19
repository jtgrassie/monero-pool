#!/bin/bash

activate_script="./testenv/bin/activate"
if [ -f $activate_script ]; then
    echo "testenv venv already exists rm -rf ./testenv/ to rebuild"
else
    echo "building new python venv `pwd`/testenv"
    python -m venv ./testenv
fi

if [ -f $activate_script ]; then
    source $activate_script
else
    echo "python venv testenv cannot find activate script"
    exit 1
fi

echo installing requirements into venv
pip install -r requirements.txt

python create_dot_env.py

if [[ $? -ne 0 ]] ; then
    echo "Something went wrong!!"
    exit 1
fi

./run_integ_tests.sh test_env.py

if [[ $? -ne 0 ]] ; then
    echo "Something went wrong!!"
    exit 1
else
    echo "Environtment setup complete you can now run launch.sh"
    echo "or launch_and_test.sh"
fi