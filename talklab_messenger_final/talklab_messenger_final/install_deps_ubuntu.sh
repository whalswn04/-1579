#!/usr/bin/env bash
set -euo pipefail
sudo apt update
sudo apt install -y build-essential cmake libboost-dev libssl-dev libsqlite3-dev nodejs npm
