#!/bin/bash

trap 'echo "Interrupted"; kill 0; exit 130' INT

# echo "Starting LSQB original"
# ./auto_run.sh lsqb lsqb 1
# echo "Starting LSQB RPT"
# ./auto_run.sh lsqb lsqb 2
# echo "Starting LSQB Yan+"
# ./auto_run.sh lsqb lsqb 3

# echo "Starting JOB original"
# ./auto_run.sh job job_part1 1
# echo "Starting JOB RPT"
# ./auto_run.sh job job_part1 2
# echo "Starting JOB Yan+"
# ./auto_run.sh job job_part1 3
# echo "Starting JOB Yan+ No GYO"
# ./auto_run.sh job job_part1 4

# echo "Starting JOB RPT"
# ./auto_run.sh job job 2
# echo "Starting JOB original"
# ./auto_run.sh job job 1
# echo "Starting JOB Yan+"
# ./auto_run.sh job job 3
# echo "Starting JOB Yan+ No GYO"
# ./auto_run.sh job job 4

echo "Starting JOB yan+"
./auto_run.sh job job_test 3
echo "Starting JOB yan+"
./auto_run.sh job job_test 2
echo "Starting JOB yan+"
./auto_run.sh job job_test 1

