#!/bin/bash

trap 'echo "Interrupted"; kill 0; exit 130' INT

echo "Starting JOB original"
./auto_run.sh job job 1
echo "Starting JOB PT"
./auto_run.sh job job 2
echo "Starting JOB Yan+"
./auto_run.sh job job 3

echo "Starting LSQB original"
./auto_run.sh lsqb lsqb 1
echo "Starting LSQB PT"
./auto_run.sh lsqb lsqb 2
echo "Starting LSQB Yan+"
./auto_run.sh lsqb lsqb 3