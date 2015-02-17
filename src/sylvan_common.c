/*
 * Copyright 2011-2015 Formal Methods and Tools, University of Twente
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <sylvan_common.h>

/**
 * Static global variables
 */

int workers;
llmsset_t nodes;

/**
 * Retrieve nodes
 */

llmsset_t
__sylvan_get_internal_data()
{
    return nodes;
}

/**
 * Calculate table usage (in parallel)
 */

TASK_2(size_t, sylvan_table_usage_par, size_t, start, size_t, end)
{
    if (end - start <= 128) {
        return llmsset_get_filled_partial(nodes, start, end);
    } else {
        size_t part = (end-start)/2;
        if (part < 128) part = 128;
        SPAWN(sylvan_table_usage_par, start, start+part);
        size_t end2 = start+2*part;
        if (end2 > end) end2 = end;
        size_t res = CALL(sylvan_table_usage_par, start+part, end2);;
        res += SYNC(sylvan_table_usage_par);
        return res;
    }
}

VOID_TASK_IMPL_2(sylvan_table_usage, size_t*, filled, size_t*, total)
{
    size_t tot = llmsset_get_size(nodes);
    if (filled != NULL) *filled = CALL(sylvan_table_usage_par, 0, tot);
    if (total != NULL) *total = tot;
}

/**
 * Thread-local insert index for LLMSset
 */
DECLARE_THREAD_LOCAL(insert_index, uint64_t*);

uint64_t*
initialize_insert_index()
{
    LOCALIZE_THREAD_LOCAL(insert_index, uint64_t*);
    insert_index = (uint64_t*)malloc(LINE_SIZE);
    LACE_ME;
    size_t my_id = LACE_WORKER_ID;
    *insert_index = llmsset_get_insertindex_multi(nodes, my_id, workers);
    SET_THREAD_LOCAL(insert_index, insert_index);
    return insert_index;
}
