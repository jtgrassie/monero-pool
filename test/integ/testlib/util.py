#!/usr/bin/env python

from os.path import exists
from subprocess import Popen, PIPE

def load_env(env_file):
    content = ""
    env_dict = {}
    assert exists(env_file)
    with open(env_file, 'r') as fh:
        content = fh.read()
    for line in content.splitlines():
        k, v = line.split("=")
        v = v.replace('"','')
        env_dict[k] = v
    return env_dict

def get_env_file():
    proc = Popen("git rev-parse --show-toplevel", stdout=PIPE, shell=True)
    (git_root, err) = proc.communicate()
    assert err == None
    git_root = str(git_root, 'utf-8')
    git_root = git_root.strip('\n')
    assert exists(git_root)
    env_file = "{}/tools/local/.env".format(git_root)
    assert exists(env_file)
    return env_file