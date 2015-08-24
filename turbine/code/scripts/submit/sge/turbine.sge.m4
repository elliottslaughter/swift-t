changecom(`dnl')#!/bin/bash
# We use changecom to change the M4 comment to dnl, not hash

# Copyright 2013 University of Chicago and Argonne National Laboratory
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License

# TURBINE.SGE.M4
# Turbine SGE template.  This is automatically filled in
# by M4 in turbine-sge-run.zsh

# Created: esyscmd(`date')

# Define convenience macros
define(`getenv', `esyscmd(printf -- "$`$1' ")')
define(`getenv_nospace', `esyscmd(printf -- "$`$1'")')

# # $ -A getenv(PROJECT)
# $ -l h rt=getenv(WALLTIME)
#$ -N getenv(TURBINE_JOBNAME)
#$ -j y
# $ -cwd getenv(TURBINE_OUTPUT)
# # $ -o getenv(OUTPUT_FILE)
#$ -pe mpich getenv(PROCS)
#$ -V

VERBOSE=getenv(VERBOSE)
if (( ${VERBOSE} ))
then
 set -x
fi

echo "TURBINE-SGE"
date
echo

cd ${SGE_O_WORKDIR}

TURBINE_HOME=getenv(TURBINE_HOME)
COMMAND="getenv(COMMAND)"

export LD_LIBRARY_PATH=getenv_nospace(LD_LIBRARY_PATH):getenv(TURBINE_LD_LIBRARY_PATH)
source ${TURBINE_HOME}/scripts/turbine-config.sh

START=$( date +%s.%N )
${TURBINE_LAUNCHER} ${COMMAND}
STOP=$( date +%s.%N )
# Bash cannot do floating point arithmetic:
DURATION=$( awk -v START=${START} -v STOP=${STOP} \
            'BEGIN { printf "%.3f\n", STOP-START }' < /dev/null )
echo "MPIEXEC TIME: ${DURATION}"