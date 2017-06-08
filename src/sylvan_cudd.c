/*
 * Copyright 2017 Tom van Dijk, Johannes Kepler University Linz
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

#include <sylvan_cudd.h>

static DdNodePtr one;
static DdNodePtr zero;

DdManager*
Cudd_Init(unsigned int numVars, unsigned int numVarsZ, unsigned int numSlots, unsigned int cacheSize, size_t maxMemory)
{
    // some sensible defaults for now
    lace_init(2, 0);
    lace_startup(0, 0, 0);
    if (maxMemory == 0) maxMemory = 1LL<<30; // 1 gigabyte
    sylvan_set_limits(maxMemory, 1, 16);
    sylvan_init_package();
    sylvan_init_mtbdd();
    one = (DdNodePtr)mtbdd_double(1.0);
    zero = (DdNodePtr)mtbdd_double(0.0);
    mtbdd_protect((MTBDD*)&one);
    mtbdd_protect((MTBDD*)&zero);
    (void)numVars;
    (void)numVarsZ;
    (void)numSlots;
    (void)cacheSize;
    return NULL;
}

void
Cudd_AutodynEnable(DdManager* dd, Cudd_ReorderingType method)
{
    (void)method;
    (void)dd;
}

void
Cudd_AutodynDisable(DdManager* dd)
{
    (void)dd;
}

DdNodePtr
Cudd_Not(DdNodePtr node)
{
    return (DdNodePtr)sylvan_not((MTBDD)node);
}

DdNodePtr
Cudd_NotCond(DdNodePtr node, int c)
{
    return c ? Cudd_Not(node) : node;
}

DdNodePtr
Cudd_Regular(DdNodePtr node)
{
    return (DdNodePtr)((MTBDD)node&~mtbdd_complement);
}

DdNodePtr
Cudd_Complement(DdNodePtr node)
{
    return (DdNodePtr)((MTBDD)node|mtbdd_complement);
}

int
Cudd_IsComplement(DdNodePtr node)
{
    return ((MTBDD)node&mtbdd_complement) ? 1 : 0;
}

unsigned int
Cudd_NodeReadIndex(DdNodePtr node)
{
    MTBDD dd = (MTBDD)node;
    if (mtbdd_isleaf(dd)) return CUDD_CONST_INDEX;
    else return mtbdd_getvar(dd);
}

DdNodePtr
Cudd_bddIthVar(DdManager* dd, int index)
{
    return (DdNodePtr)mtbdd_ithvar(index);
    (void)dd;
}

DdNodePtr
Cudd_addIthVar(DdManager* dd, int index)
{
    return (DdNodePtr)mtbdd_makenode(index, (MTBDD)zero, (MTBDD)one);
    (void)dd;
}

int
Cudd_IsConstant(DdNodePtr node);

int
Cudd_IsNonConstant(DdNodePtr f);

DdNodePtr
Cudd_T(DdNodePtr node)
{
    return (DdNodePtr)mtbdd_gethigh((MTBDD)node);
}

DdNodePtr
Cudd_E(DdNodePtr node)
{
    return (DdNodePtr)mtbdd_getlow((MTBDD)node);
}

CUDD_VALUE_TYPE
Cudd_V(DdNodePtr node)
{
    return mtbdd_getdouble((MTBDD)node);
}

DdNodePtr
Cudd_ReadOne(DdManager* dd)
{
    return one;
    (void)dd;
}

DdNodePtr
Cudd_ReadZero(DdManager* dd)
{
    return zero;
    (void)dd;
}

DdNodePtr
Cudd_ReadLogicOne(DdManager* dd)
{
    return (DdNodePtr)mtbdd_true;
    (void)dd;
}

DdNodePtr
Cudd_ReadLogicZero(DdManager* dd)
{
    return (DdNodePtr)mtbdd_false;
    (void)dd;
}

DdNodePtr
Cudd_addConst(DdManager* dd, CUDD_VALUE_TYPE c)
{
    return (DdNodePtr)mtbdd_double(c);
    (void)dd;
}

DdNodePtr
Cudd_addNegate(DdManager* dd, DdNodePtr f)
{
    LACE_ME;
    return (DdNodePtr)CALL(mtbdd_uapply, (MTBDD)f, TASK(mtbdd_op_negate), 0);
    (void)dd;
}

DdNodePtr
Cudd_addCmpl(DdManager* dd, DdNodePtr f)
{
    LACE_ME;
    return (DdNodePtr)CALL(mtbdd_uapply, (MTBDD)f, TASK(mtbdd_op_cmpl), 0);
    (void)dd;
}

DdNodePtr
Cudd_bddAnd(DdManager* dd, DdNodePtr f, DdNodePtr g)
{
    LACE_ME;
    return (DdNodePtr)CALL(sylvan_and, (size_t)f, (size_t)g, 0);
    (void)dd;
}

DdNodePtr
Cudd_bddOr(DdManager* dd, DdNodePtr f, DdNodePtr g)
{
    LACE_ME;
    return (DdNodePtr)sylvan_not(CALL(sylvan_and, sylvan_not((size_t)f), sylvan_not((size_t)g), 0));
    (void)dd;
}

DdNodePtr
Cudd_bddXor(DdManager* dd, DdNodePtr f, DdNodePtr g)
{
    LACE_ME;
    return (DdNodePtr)CALL(sylvan_xor, (size_t)f, (size_t)g, 0);
    (void)dd;
}

int
Cudd_bddLeq(DdManager* dd, DdNodePtr f, DdNodePtr g)
{
    /**
     * f <= g means: nothing in f that is not in g
     * so the intersection of f and ~g must be empty!
     */
    LACE_ME;
    MTBDD F = (MTBDD)f;
    MTBDD G = (MTBDD)g;
    MTBDD R = CALL(sylvan_ite, F, sylvan_not(G), sylvan_false, 0);
    return R == sylvan_false;
    (void)dd;
}

int
Cudd_bddGeq(DdManager* dd, DdNodePtr f, DdNodePtr g)
{
    return Cudd_bddLeq(NULL, g, f);
    (void)dd;
}

// DdNodePtr Cudd_addExistAbstract(DdManager* dd, DdNodePtr f, DdNodePtr cube);
// DdNodePtr Cudd_addUnivAbstract(DdManager* dd, DdNodePtr f, DdNodePtr cube);
// DdNodePtr Cudd_addOrAbstract(DdManager* dd, DdNodePtr f, DdNodePtr cube);

DdNodePtr
Cudd_Support(DdManager* dd, DdNodePtr f)
{
    LACE_ME;
    return (DdNodePtr)(size_t)CALL(mtbdd_support, (size_t)f);
    (void)dd;
}

int
Cudd_SupportSize(DdManager* dd, DdNodePtr f)
{
    LACE_ME;
    BDD supp = CALL(mtbdd_support, (size_t)f);
    return mtbdd_set_count(supp);
    (void)dd;
}

int*
Cudd_SupportIndex(DdManager* dd, DdNodePtr f);

/**
 * Get the number of nodes in the BDD
 * NOTE: not thread-safe!
 */
int
Cudd_DagSize(DdNodePtr node)
{
    return mtbdd_nodecount((MTBDD)node);
}

int
Cudd_CheckCube(DdManager* dd, DdNodePtr g);

double
Cudd_CountPath(DdNodePtr node)
{
    return -1; // not implemented yet...... FIXME
    //return mtbdd_pathcount((MTBDD)(size_t)node);
    (void)node;
}

double
Cudd_CountMinterm(DdManager* dd, DdNodePtr node, int nvars)
{
    LACE_ME;
    return CALL(mtbdd_satcount, (MTBDD)node, nvars);
    (void)dd;
}

// int Cudd_bddPickOneCube(DdManager* dd, DdNodePtr node, char *string);

DdNodePtr
Cudd_bddPickOneMinterm(DdManager* dd, DdNodePtr f, DdNodePtr *vars, int n)
{
    LACE_ME;
    uint32_t vars_a[n];
    for (int i=0; i<n; i++) {
        vars_a[i] = sylvan_var((MTBDD)vars[i]);
        // also check order
        if (i>0) assert(vars_a[i-1] < vars_a[i]);
    }
    MTBDD vars_dd = mtbdd_set_fromarray(vars_a, n);
    mtbdd_refs_push(vars_dd);
    uint8_t arr[n];
    MTBDD leaf = mtbdd_enum_all_first((MTBDD)f, vars_dd, arr, NULL);
    MTBDD result = mtbdd_cube(vars_dd, arr, leaf);
    mtbdd_refs_pop(1);
    return (DdNodePtr)result;
    (void)dd;
}

DdNodePtr
Cudd_bddExistAbstract(DdManager* dd, DdNodePtr f, DdNodePtr cube)
{
    LACE_ME;
    return (DdNodePtr)CALL(sylvan_exists, (MTBDD)f, (MTBDD)cube, 0);
    (void)dd;
}

DdNodePtr
Cudd_bddUnivAbstract(DdManager* dd, DdNodePtr f, DdNodePtr cube)
{
    LACE_ME;
    return (DdNodePtr)sylvan_not(CALL(sylvan_exists, sylvan_not((MTBDD)f), (MTBDD)cube, 0));
    (void)dd;
}

DdNodePtr
Cudd_bddSwapVariables(DdManager* dd, DdNodePtr f, DdNodePtr *x, DdNodePtr *y, int n)
{
    LACE_ME;
    // create map
    MTBDDMAP map = mtbdd_map_empty();
    mtbdd_refs_pushptr(&map);
    for (int i=0; i<n; i++) {
        const uint32_t var_x = sylvan_var((MTBDD)x[i]);
        const uint32_t var_y = sylvan_var((MTBDD)y[i]);
        map = mtbdd_map_add(map, var_x, (MTBDD)y[i]);
        map = mtbdd_map_add(map, var_y, (MTBDD)x[i]);
    }
    // perform swap
    DdNodePtr result = (DdNodePtr)CALL(mtbdd_compose, (MTBDD)f, map);
    mtbdd_refs_popptr(1);
    return result;
    (void)dd;
}

DdNodePtr
Cudd_addSwapVariables(DdManager* dd, DdNodePtr f, DdNodePtr *x, DdNodePtr *y, int n)
{
    LACE_ME;
    // create map
    MTBDDMAP map = mtbdd_map_empty();
    mtbdd_refs_pushptr(&map);
    for (int i=0; i<n; i++) {
        const uint32_t var_x = sylvan_var((MTBDD)x[i]);
        const uint32_t var_y = sylvan_var((MTBDD)y[i]);
        map = mtbdd_map_add(map, var_x, mtbdd_ithvar(var_y));
        map = mtbdd_map_add(map, var_y, mtbdd_ithvar(var_x));
    }
    // perform swap
    DdNodePtr result = (DdNodePtr)CALL(mtbdd_compose, (MTBDD)f, map);
    mtbdd_refs_popptr(1);
    return result;
    (void)dd;
}

TASK_3(MTBDD, addApplyWrapper, MTBDD*, f, MTBDD*, g, size_t, _ctx)
{
    DD_AOP op = (DD_AOP)_ctx;
    MTBDD res = (MTBDD)op(NULL, (DdNodePtr*)f, (DdNodePtr*)g);
    if (res == 0) return mtbdd_invalid;
    else return res;
}

DdNodePtr
Cudd_addApply(DdManager* dd, DD_AOP op, DdNodePtr f, DdNodePtr g)
{
    MTBDD F = (MTBDD)f;
    MTBDD G = (MTBDD)g;
    LACE_ME;
    MTBDD R = CALL(mtbdd_applyp, F, G, (size_t)op, TASK(addApplyWrapper), (size_t)op);
    return (DdNodePtr)R;
    (void)dd;
}

DdNodePtr
Cudd_addThreshold(DdManager* dd, DdNodePtr *f, DdNodePtr *g)
{
    MTBDD F = (MTBDD)*f;
    MTBDD G = (MTBDD)*g;
    LACE_ME;
    if (F == G /* or F == plus_infinity */) return *f;
    else if (mtbdd_isleaf(F) && mtbdd_isleaf(G)) {
        if (mtbdd_getdouble(F) >= mtbdd_getdouble(G)) return *f;
        else return Cudd_ReadZero(NULL);
    }
    return NULL;
    (void)dd;
}

DdNodePtr
Cudd_addTimes(DdManager* dd, DdNodePtr *f, DdNodePtr *g)
{
    MTBDD F = (MTBDD)*f;
    MTBDD G = (MTBDD)*g;
    LACE_ME;
    if (F == (MTBDD)zero || G == (MTBDD)zero) return (DdNodePtr)zero;
    else if (F == (MTBDD)one) return (DdNodePtr)G;
    else if (G == (MTBDD)one) return (DdNodePtr)F;
    else if (mtbdd_isleaf(F) && mtbdd_isleaf(G)) {
        double value = mtbdd_getdouble(F) * mtbdd_getdouble(G);
        return (DdNodePtr)mtbdd_double(value);
    } else if (F > G) {
        *f = (DdNodePtr)G;
        *g = (DdNodePtr)F;
    }
    return NULL;
    (void)dd;
}

DdNodePtr
Cudd_addPlus(DdManager* dd, DdNodePtr *f, DdNodePtr *g)
{
    MTBDD F = (MTBDD)*f;
    MTBDD G = (MTBDD)*g;
    LACE_ME;
    if (F == (MTBDD)zero) return (DdNodePtr)G;
    else if (G == (MTBDD)zero) return (DdNodePtr)F;
    else if (mtbdd_isleaf(F) && mtbdd_isleaf(G)) {
        double value = mtbdd_getdouble(F) + mtbdd_getdouble(G);
        return (DdNodePtr)mtbdd_double(value);
    } else if (F > G) {
        *f = (DdNodePtr)G;
        *g = (DdNodePtr)F;
    }
    return NULL;
    (void)dd;
}

DdNodePtr
Cudd_addMinus(DdManager* dd, DdNodePtr *f, DdNodePtr *g)
{
    MTBDD F = (MTBDD)*f;
    MTBDD G = (MTBDD)*g;
    LACE_ME;
    if (F == G) return zero;
    else if (F == (MTBDD)zero) return (DdNodePtr)CALL(mtbdd_uapply, G, TASK(mtbdd_op_negate), 0);
    else if (G == (MTBDD)zero) return (DdNodePtr)F;
    else if (mtbdd_isleaf(F) && mtbdd_isleaf(G)) {
        double value = mtbdd_getdouble(F) - mtbdd_getdouble(G);
        return (DdNodePtr)mtbdd_double(value);
    } else return NULL;
    (void)dd;
}

DdNodePtr
Cudd_addOr(DdManager* dd, DdNodePtr *f, DdNodePtr *g)
{
    MTBDD F = (MTBDD)*f;
    MTBDD G = (MTBDD)*g;
    LACE_ME;
    if (F == (MTBDD)one || G == (MTBDD)one) return (DdNodePtr)one;
    else if (mtbdd_isleaf(F)) return (DdNodePtr)G;
    else if (mtbdd_isleaf(G)) return (DdNodePtr)F;
    else if (F == G) return (DdNodePtr)F;
    else if (F > G) {
        *f = (DdNodePtr)G;
        *g = (DdNodePtr)F;
    }
    return NULL;
    (void)dd;
}

DdNodePtr
Cudd_addDivide(DdManager* dd, DdNodePtr *f, DdNodePtr *g)
{
    MTBDD F = (MTBDD)*f;
    MTBDD G = (MTBDD)*g;
    LACE_ME;
    if (F == (MTBDD)zero) return zero;
    else if (G == (MTBDD)one) return (DdNodePtr)F;
    else if (mtbdd_isleaf(F) && mtbdd_isleaf(G)) {
        double value = mtbdd_getdouble(F) / mtbdd_getdouble(G);
        return (DdNodePtr)mtbdd_double(value);
    }
    return NULL;
    (void)dd;
}

DdNodePtr
Cudd_addBddThreshold(DdManager* dd, DdNodePtr f, CUDD_VALUE_TYPE value)
{
    LACE_ME;
    return (DdNodePtr)CALL(mtbdd_threshold_double, (MTBDD)f, value);
    (void)dd;
}

DdNodePtr
Cudd_addBddStrictThreshold(DdManager* dd, DdNodePtr f, CUDD_VALUE_TYPE value)
{
    LACE_ME;
    return (DdNodePtr)CALL(mtbdd_strict_threshold_double, (MTBDD)f, value);
    (void)dd;
}

DdNodePtr
Cudd_addExistAbstract(DdManager* dd, DdNodePtr f, DdNodePtr cube)
{
    LACE_ME;
    return (DdNodePtr)CALL(mtbdd_abstract, (MTBDD)f, (MTBDD)cube, TASK(mtbdd_abstract_op_plus));
    (void)dd;
}

DdNodePtr
Cudd_addUnivAbstract(DdManager* dd, DdNodePtr f, DdNodePtr cube)
{
    LACE_ME;
    return (DdNodePtr)CALL(mtbdd_abstract, (MTBDD)f, (MTBDD)cube, TASK(mtbdd_abstract_op_times));
    (void)dd;
}

int
Cudd_PrintInfo(DdManager* dd, FILE *fp)
{
    fprintf(fp, "CuDD implemented by Sylvan\n");
    return 1;
    (void)dd;
}

size_t
Cudd_ReadMemoryInUse(DdManager* dd)
{
    return 0; // TODO FIXME
    (void)dd;
}

int
Cudd_PrintDebug(DdManager* dd, DdNodePtr f, int n, int pr)
{
    MTBDD F = (MTBDD)f;

    if (F == mtbdd_invalid) printf(": is mtbdd_invalid\n");
    if (F == mtbdd_false) printf(": is mtbdd_false\n");
    if (F == mtbdd_true) printf(": is mtbdd_true\n");
    fflush(stdout);

    // Todo:
    // if pr >= 1
    // - print number of nodes
    // - print number of leaves
    // - print number of minterms
    // if pr > 2 .... 

    return 1;
    (void)n;
    (void)pr;
    (void)dd;
}

/**
 * Compute BDD of X == Y, matching variables in x with variables in y,
 * where x[0] < y[0] < x[1] < y[1] in the variable ordering (if you want speed)
 */
DdNodePtr
Cudd_Xeqy(DdManager* dd, int N, DdNodePtr *x, DdNodePtr *y) 
{
    LACE_ME;
    MTBDD u, v, w;
    u = v = w = mtbdd_true;
    mtbdd_refs_pushptr(&u);
    mtbdd_refs_pushptr(&v);
    mtbdd_refs_pushptr(&w);
    for (int i=0; i<N; i++) {
        MTBDD var_x = (MTBDD)x[N-i-1];
        MTBDD var_y = (MTBDD)y[N-i-1];
        v = CALL(sylvan_and, var_y, u, 0);
        w = CALL(sylvan_and, sylvan_not(var_y), u, 0);
        u = CALL(sylvan_ite, var_x, v, w, 0);
    }
    mtbdd_refs_popptr(3);
    return (DdNodePtr)u;
    (void)dd;
}

MtrNode*
Cudd_MakeTreeNode(DdManager* dd, unsigned int low, unsigned int size, unsigned int type)
{
    (void)low;
    (void)size;
    (void)type;
    return NULL;
    (void)dd;
}

int
Cudd_DumpDot(DdManager* dd, int n, DdNodePtr *f, char const * const *inames, char const * const *onames, FILE *fp)
{
    // TODO support multiple
    for (int i=0; i<n; i++) {
        mtbdd_fprintdot(fp, (MTBDD)f[i]);
    }
        
    // ignore inames,onames for now
    (void)inames;
    (void)onames;
    return 0;
    (void)dd;
}
