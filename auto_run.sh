#!/bin/bash

trap 'echo "Interrupted"; kill 0; exit 130' INT

uNames=`uname -s`
osName=${uNames: 0: 4}
if [ "$osName" == "Darw" ] # Darwin
then
	COMMAND="ghead"
elif [ "$osName" == "Linu" ] # Linux
then
	COMMAND="head"
fi

SCRIPT=$(readlink -f $0)
SCRIPT_PATH=$(dirname "${SCRIPT}")

INPUT_DIR=$2
INPUT_DIR_PATH="${SCRIPT_PATH}/${INPUT_DIR}"

# graph, tpch, lsqb
DATABASE=$1

NUM_THREADS=${4:-72}

DUCK_NUM=${3:-1}

declare -A DUCK_MAP=(
  [1]="./duckdb_origin"
  [2]="./duckdb_RPT"
  [3]="./duckdb_YanPlus"
  [4]="./duckdb_YanPlus_NoGYO"
)

if [[ -z ${DUCK_MAP[$DUCK_NUM]} ]]; then
  echo "Usage: $0 <db> <dir> [threads] {1|2|3}" >&2
  exit 1
fi
DUCKDB_BIN=${DUCK_MAP[$DUCK_NUM]}

# Suffix function
function FileSuffix() {
    local filename="$1"
    if [ -n "$filename" ]; then
        echo "${filename##*.}"
    fi
}

function IsSuffix() {
    local filename="$1"
    if [ "$(FileSuffix ${filename})" = "sql" ]
    then
        return 0
    else 
        return 1
    fi
}

for file in $(ls ${INPUT_DIR_PATH})
do
    IsSuffix ${file}
    ret=$?
    if [ $ret -eq 0 ]
    then
        filename="${file%.*}"
        LOG_FILE="${INPUT_DIR_PATH}/log_${filename}_${DUCK_NUM}.txt"
        TIME_FILE="${INPUT_DIR_PATH}/time_${filename}_${DUCK_NUM}.txt"
        rm -f $LOG_FILE $TIME_FILE
        touch $LOG_FILE
        QUERY="${INPUT_DIR_PATH}/${file}"
        RAN=$RANDOM
        SUBMIT_QUERY="${INPUT_DIR_PATH}/query_${RAN}.sql"
        rm -f "${SUBMIT_QUERY}"
        touch "${SUBMIT_QUERY}"
        echo "COPY (" >> ${SUBMIT_QUERY}
        cat ${QUERY} >> ${SUBMIT_QUERY}
        echo ") TO '/dev/null' (DELIMITER ',');" >> ${SUBMIT_QUERY}
        echo "Start ${DUCKDB_BIN} Task at ${QUERY}"
        for ((current_task=1; current_task<=5; current_task++)); 
        do
            echo "Current Task: ${current_task}"
            timeout -s SIGKILL 15m ${DUCKDB_BIN} -c ".open ${DATABASE}_db" -c "SET threads TO ${NUM_THREADS};" -c ".timer off" -c ".read ${SUBMIT_QUERY}" -c ".timer on" -c ".read ${SUBMIT_QUERY}" 2>&1 | tee -a "${LOG_FILE}" | tail -n 1 | awk '{print $1}' >> "${TIME_FILE}"
        done
        awk '{s+=$1} END{if(NR) print "AVG", s/NR}' "$TIME_FILE" >> "$TIME_FILE"
        echo "End DuckDB Task..."
        rm -f "${SUBMIT_QUERY}"
    fi
done