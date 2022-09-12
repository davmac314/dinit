#!/usr/bin/env bash

make mconfig
make -C build
build/tools/mconfig-gen > src/includes/mconfig.h
