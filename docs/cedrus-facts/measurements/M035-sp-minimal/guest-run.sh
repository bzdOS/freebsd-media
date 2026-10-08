#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
set -u
(cd sp26-pcm && ../../ve_lab replay.lab > lab.txt 2>&1; echo $? > lab-exit.txt)
(cd sp36-pcm && ../../ve_lab replay.lab > lab.txt 2>&1; echo $? > lab-exit.txt)
(cd sp36-switch-pcm && ../../ve_lab replay.lab > lab.txt 2>&1; echo $? > lab-exit.txt)
(cd p-skip && ../../ve_lab replay.lab > lab.txt 2>&1; echo $? > lab-exit.txt)
(cd sp26-skip && ../../ve_lab replay.lab > lab.txt 2>&1; echo $? > lab-exit.txt)
(cd sp36-skip && ../../ve_lab replay.lab > lab.txt 2>&1; echo $? > lab-exit.txt)
(cd sp36-switch-skip && ../../ve_lab replay.lab > lab.txt 2>&1; echo $? > lab-exit.txt)
(cd p-inter-zero && ../../ve_lab replay.lab > lab.txt 2>&1; echo $? > lab-exit.txt)
(cd sp26-inter-zero && ../../ve_lab replay.lab > lab.txt 2>&1; echo $? > lab-exit.txt)
(cd sp36-inter-zero && ../../ve_lab replay.lab > lab.txt 2>&1; echo $? > lab-exit.txt)
(cd sp36-switch-inter-zero && ../../ve_lab replay.lab > lab.txt 2>&1; echo $? > lab-exit.txt)
(cd p-intra16-zero && ../../ve_lab replay.lab > lab.txt 2>&1; echo $? > lab-exit.txt)
(cd sp26-intra16-zero && ../../ve_lab replay.lab > lab.txt 2>&1; echo $? > lab-exit.txt)
(cd sp36-intra16-zero && ../../ve_lab replay.lab > lab.txt 2>&1; echo $? > lab-exit.txt)
(cd sp36-switch-intra16-zero && ../../ve_lab replay.lab > lab.txt 2>&1; echo $? > lab-exit.txt)
