/*
 * Copyright 2017 Johannes Kepler University Linz, Tom van Dijk
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

#include <sylvan.h>

#ifndef SYLVAN_CUDD_H
#define SYLVAN_CUDD_H

#ifdef __cplusplus
namespace sylvan_cudd {

extern "C" {
#endif

/**
 * Types
 */
typedef double CUDD_VALUE_TYPE;
typedef struct {} DdNode;
typedef DdNode *DdNodePtr;
typedef struct {} DdManager;
typedef struct {} DdGen;
// todo: apa

typedef enum {
    CUDD_REORDER_SAME,
    CUDD_REORDER_NONE,
    CUDD_REORDER_RANDOM,
    CUDD_REORDER_RANDOM_PIVOT,
    CUDD_REORDER_SIFT,
    CUDD_REORDER_SIFT_CONVERGE,
    CUDD_REORDER_SYMM_SIFT,
    CUDD_REORDER_SYMM_SIFT_CONV,
    CUDD_REORDER_WINDOW2,
    CUDD_REORDER_WINDOW3,
    CUDD_REORDER_WINDOW4,
    CUDD_REORDER_WINDOW2_CONV,
    CUDD_REORDER_WINDOW3_CONV,
    CUDD_REORDER_WINDOW4_CONV,
    CUDD_REORDER_GROUP_SIFT,
    CUDD_REORDER_GROUP_SIFT_CONV,
    CUDD_REORDER_ANNEALING,
    CUDD_REORDER_GENETIC,
    CUDD_REORDER_LINEAR,
    CUDD_REORDER_LINEAR_CONVERGE,
    CUDD_REORDER_LAZY_SIFT,
    CUDD_REORDER_EXACT
} Cudd_ReorderingType;

typedef enum {
    CUDD_PRE_GC_HOOK,
    CUDD_POST_GC_HOOK,
    CUDD_PRE_REORDERING_HOOK,
    CUDD_POST_REORDERING_HOOK
} Cudd_HookType;

typedef DdNodePtr (*DD_AOP)(DdManager*, DdNodePtr*, DdNodePtr*);
typedef DdNodePtr (*DD_MAOP)(DdManager*, DdNodePtr);

/**
 * Constants
 */

// logical true and false; same as mtbdd_true and mtbdd_false.
static const DdNodePtr CUDD_TRUE  = (DdNodePtr)0x8000000000000000LL;
static const DdNodePtr CUDD_FALSE = (DdNodePtr)0x0000000000000000LL;

// the variable index for leaves
static const uint32_t CUDD_CONST_INDEX = ((uint32_t)-1);

// CUDD_OUT_OF_MEM (mtbdd_invalid; actually Sylvan aborts when out of memory)
static const DdNodePtr CUDD_OUT_OF_MEM = (DdNodePtr)0xffffffffffffffffLL;

// not actually used...
static const size_t CUDD_CACHE_SLOTS = 262144;
static const size_t CUDD_UNIQUE_SLOTS = 262144; // only 1 table in Sylvan

/**
 * The following are macros in Cudd but not in this wrapper.
 */
DdNodePtr Cudd_Not(DdNodePtr node);
DdNodePtr Cudd_NotCond(DdNodePtr node, int c);
DdNodePtr Cudd_Regular(DdNodePtr node);
DdNodePtr Cudd_Complement(DdNodePtr node);
int Cudd_IsComplement(DdNodePtr node);
// Cudd_ReadIndex, Cudd_ReadPerm
// Cudd_ForeachCube, Cudd_ForeachPrime, Cudd_ForeachNode
// Cudd_zddForeachPath

/**
 * The following are function prototypes defined in Cudd.
 */

// DdNode * Cudd_addNewVar(DdManager *dd);
// DdNode * Cudd_addNewVarAtLevel(DdManager *dd, int level);
// DdNode * Cudd_bddNewVar(DdManager *dd);
// DdNode * Cudd_bddNewVarAtLevel(DdManager *dd, int level);
// int Cudd_bddIsVar(DdManager * dd, DdNode * f);
DdNode * Cudd_addIthVar(DdManager *dd, int i);
DdNode * Cudd_bddIthVar(DdManager *dd, int i);
// DdNode * Cudd_zddIthVar(DdManager *dd, int i);
// int Cudd_zddVarsFromBddVars(DdManager *dd, int multiplicity);
// unsigned int Cudd_ReadMaxIndex(void);
DdNode * Cudd_addConst(DdManager *dd, CUDD_VALUE_TYPE c);
// int Cudd_IsConstant(DdNode *node);
// int Cudd_IsNonConstant(DdNode *f);
DdNode * Cudd_T(DdNode *node);
DdNode * Cudd_E(DdNode *node);
CUDD_VALUE_TYPE Cudd_V(DdNode *node);
// unsigned long Cudd_ReadStartTime(DdManager *unique);
// unsigned long Cudd_ReadElapsedTime(DdManager *unique);
// void Cudd_SetStartTime(DdManager *unique, unsigned long st);
// void Cudd_ResetStartTime(DdManager *unique);
// unsigned long Cudd_ReadTimeLimit(DdManager *unique);
// unsigned long Cudd_SetTimeLimit(DdManager *unique, unsigned long tl);
// void Cudd_UpdateTimeLimit(DdManager * unique);
// void Cudd_IncreaseTimeLimit(DdManager * unique, unsigned long increase);
// void Cudd_UnsetTimeLimit(DdManager *unique);
// int Cudd_TimeLimited(DdManager *unique);
// void Cudd_RegisterTerminationCallback(DdManager *unique, DD_THFP callback, void * callback_arg);
// void Cudd_UnregisterTerminationCallback(DdManager *unique);
// DD_OOMFP Cudd_RegisterOutOfMemoryCallback(DdManager *unique, DD_OOMFP callback);
// void Cudd_UnregisterOutOfMemoryCallback(DdManager *unique);
// void Cudd_RegisterTimeoutHandler(DdManager *unique, DD_TOHFP handler, void *arg);
// DD_TOHFP Cudd_ReadTimeoutHandler(DdManager *unique, void **argp);
void Cudd_AutodynEnable(DdManager *unique, Cudd_ReorderingType method);
void Cudd_AutodynDisable(DdManager *unique);
// int Cudd_ReorderingStatus(DdManager *unique, Cudd_ReorderingType *method);
// void Cudd_AutodynEnableZdd(DdManager *unique, Cudd_ReorderingType method);
// void Cudd_AutodynDisableZdd(DdManager *unique);
// int Cudd_ReorderingStatusZdd(DdManager *unique, Cudd_ReorderingType *method);
// int Cudd_zddRealignmentEnabled(DdManager *unique);
// void Cudd_zddRealignEnable(DdManager *unique);
// void Cudd_zddRealignDisable(DdManager *unique);
// int Cudd_bddRealignmentEnabled(DdManager *unique);
// void Cudd_bddRealignEnable(DdManager *unique);
// void Cudd_bddRealignDisable(DdManager *unique);
DdNode * Cudd_ReadOne(DdManager *dd);
// DdNode * Cudd_ReadZddOne(DdManager *dd, int i);
DdNode * Cudd_ReadZero(DdManager *dd);
DdNode * Cudd_ReadLogicOne(DdManager *dd); // this is not a Cudd function
DdNode * Cudd_ReadLogicZero(DdManager *dd);
// DdNode * Cudd_ReadPlusInfinity(DdManager *dd);
// DdNode * Cudd_ReadMinusInfinity(DdManager *dd);
// DdNode * Cudd_ReadBackground(DdManager *dd);
// void Cudd_SetBackground(DdManager *dd, DdNode *bck);
// unsigned int Cudd_ReadCacheSlots(DdManager *dd);
// double Cudd_ReadCacheUsedSlots(DdManager * dd);
// double Cudd_ReadCacheLookUps(DdManager *dd);
// double Cudd_ReadCacheHits(DdManager *dd);
// double Cudd_ReadRecursiveCalls(DdManager * dd);
// unsigned int Cudd_ReadMinHit(DdManager *dd);
// void Cudd_SetMinHit(DdManager *dd, unsigned int hr);
// unsigned int Cudd_ReadLooseUpTo(DdManager *dd);
// void Cudd_SetLooseUpTo(DdManager *dd, unsigned int lut);
// unsigned int Cudd_ReadMaxCache(DdManager *dd);
// unsigned int Cudd_ReadMaxCacheHard(DdManager *dd);
// void Cudd_SetMaxCacheHard(DdManager *dd, unsigned int mc);
// int Cudd_ReadSize(DdManager *dd);
// int Cudd_ReadZddSize(DdManager *dd);
// unsigned int Cudd_ReadSlots(DdManager *dd);
// double Cudd_ReadUsedSlots(DdManager * dd);
// double Cudd_ExpectedUsedSlots(DdManager * dd);
// unsigned int Cudd_ReadKeys(DdManager *dd);
// unsigned int Cudd_ReadDead(DdManager *dd);
// unsigned int Cudd_ReadMinDead(DdManager *dd);
// unsigned int Cudd_ReadReorderings(DdManager *dd);
// unsigned int Cudd_ReadMaxReorderings(DdManager *dd);
// void Cudd_SetMaxReorderings(DdManager *dd, unsigned int mr);
// long Cudd_ReadReorderingTime(DdManager * dd);
// int Cudd_ReadGarbageCollections(DdManager * dd);
// long Cudd_ReadGarbageCollectionTime(DdManager * dd);
// double Cudd_ReadNodesFreed(DdManager * dd);
// double Cudd_ReadNodesDropped(DdManager * dd);
// double Cudd_ReadUniqueLookUps(DdManager * dd);
// double Cudd_ReadUniqueLinks(DdManager * dd);
// int Cudd_ReadSiftMaxVar(DdManager *dd);
// void Cudd_SetSiftMaxVar(DdManager *dd, int smv);
// int Cudd_ReadSiftMaxSwap(DdManager *dd);
// void Cudd_SetSiftMaxSwap(DdManager *dd, int sms);
// double Cudd_ReadMaxGrowth(DdManager *dd);
// void Cudd_SetMaxGrowth(DdManager *dd, double mg);
// double Cudd_ReadMaxGrowthAlternate(DdManager * dd);
// void Cudd_SetMaxGrowthAlternate(DdManager * dd, double mg);
// int Cudd_ReadReorderingCycle(DdManager * dd);
// void Cudd_SetReorderingCycle(DdManager * dd, int cycle);
unsigned int Cudd_NodeReadIndex(DdNode *node);
// int Cudd_ReadPerm(DdManager *dd, int i);
// int Cudd_ReadPermZdd(DdManager *dd, int i);
// int Cudd_ReadInvPerm(DdManager *dd, int i);
// int Cudd_ReadInvPermZdd(DdManager *dd, int i);
// DdNode * Cudd_ReadVars(DdManager *dd, int i);
// CUDD_VALUE_TYPE Cudd_ReadEpsilon(DdManager *dd);
// void Cudd_SetEpsilon(DdManager *dd, CUDD_VALUE_TYPE ep);
// Cudd_AggregationType Cudd_ReadGroupcheck(DdManager *dd);
// void Cudd_SetGroupcheck(DdManager *dd, Cudd_AggregationType gc);
// int Cudd_GarbageCollectionEnabled(DdManager *dd);
// void Cudd_EnableGarbageCollection(DdManager *dd);
// void Cudd_DisableGarbageCollection(DdManager *dd);
// int Cudd_DeadAreCounted(DdManager *dd);
// void Cudd_TurnOnCountDead(DdManager *dd);
// void Cudd_TurnOffCountDead(DdManager *dd);
// int Cudd_ReadRecomb(DdManager *dd);
// void Cudd_SetRecomb(DdManager *dd, int recomb);
// int Cudd_ReadSymmviolation(DdManager *dd);
// void Cudd_SetSymmviolation(DdManager *dd, int symmviolation);
// int Cudd_ReadArcviolation(DdManager *dd);
// void Cudd_SetArcviolation(DdManager *dd, int arcviolation);
// int Cudd_ReadPopulationSize(DdManager *dd);
// void Cudd_SetPopulationSize(DdManager *dd, int populationSize);
// int Cudd_ReadNumberXovers(DdManager *dd);
// void Cudd_SetNumberXovers(DdManager *dd, int numberXovers);
// unsigned int Cudd_ReadOrderRandomization(DdManager * dd);
// void Cudd_SetOrderRandomization(DdManager * dd, unsigned int factor);
size_t Cudd_ReadMemoryInUse(DdManager *dd);
int Cudd_PrintInfo(DdManager *dd, FILE *fp);
// long Cudd_ReadPeakNodeCount(DdManager *dd);
// int Cudd_ReadPeakLiveNodeCount(DdManager * dd);
// long Cudd_ReadNodeCount(DdManager *dd);
// long Cudd_zddReadNodeCount(DdManager *dd);
// int Cudd_AddHook(DdManager *dd, DD_HFP f, Cudd_HookType where);
// int Cudd_RemoveHook(DdManager *dd, DD_HFP f, Cudd_HookType where);
// int Cudd_IsInHook(DdManager * dd, DD_HFP f, Cudd_HookType where);
// int Cudd_StdPreReordHook(DdManager *dd, const char *str, void *data);
// int Cudd_StdPostReordHook(DdManager *dd, const char *str, void *data);
// int Cudd_EnableReorderingReporting(DdManager *dd);
// int Cudd_DisableReorderingReporting(DdManager *dd);
// int Cudd_ReorderingReporting(DdManager *dd);
// int Cudd_PrintGroupedOrder(DdManager * dd, const char *str, void *data);
// int Cudd_EnableOrderingMonitoring(DdManager *dd);
// int Cudd_DisableOrderingMonitoring(DdManager *dd);
// int Cudd_OrderingMonitoring(DdManager *dd);
// void Cudd_SetApplicationHook(DdManager *dd, void * value);
// void * Cudd_ReadApplicationHook(DdManager *dd);
// Cudd_ErrorType Cudd_ReadErrorCode(DdManager *dd);
// void Cudd_ClearErrorCode(DdManager *dd);
// DD_OOMFP Cudd_InstallOutOfMemoryHandler(DD_OOMFP newHandler);
// FILE * Cudd_ReadStdout(DdManager *dd);
// void Cudd_SetStdout(DdManager *dd, FILE *fp);
// FILE * Cudd_ReadStderr(DdManager *dd);
// void Cudd_SetStderr(DdManager *dd, FILE *fp);
// unsigned int Cudd_ReadNextReordering(DdManager *dd);
// void Cudd_SetNextReordering(DdManager *dd, unsigned int next);
// double Cudd_ReadSwapSteps(DdManager *dd);
// unsigned int Cudd_ReadMaxLive(DdManager *dd);
// void Cudd_SetMaxLive(DdManager *dd, unsigned int maxLive);
// size_t Cudd_ReadMaxMemory(DdManager *dd);
// size_t Cudd_SetMaxMemory(DdManager *dd, size_t maxMemory);
// int Cudd_bddBindVar(DdManager *dd, int index);
// int Cudd_bddUnbindVar(DdManager *dd, int index);
// int Cudd_bddVarIsBound(DdManager *dd, int index);
DdNode * Cudd_addExistAbstract(DdManager *manager, DdNode *f, DdNode *cube);
DdNode * Cudd_addUnivAbstract(DdManager *manager, DdNode *f, DdNode *cube);
// DdNode * Cudd_addOrAbstract(DdManager *manager, DdNode *f, DdNode *cube);
DdNode * Cudd_addApply(DdManager *dd, DD_AOP op, DdNode *f, DdNode *g);
DdNode * Cudd_addPlus(DdManager *dd, DdNode **f, DdNode **g);
DdNode * Cudd_addTimes(DdManager *dd, DdNode **f, DdNode **g);
DdNode * Cudd_addThreshold(DdManager *dd, DdNode **f, DdNode **g);
// DdNode * Cudd_addSetNZ(DdManager *dd, DdNode **f, DdNode **g);
DdNode * Cudd_addDivide(DdManager *dd, DdNode **f, DdNode **g);
DdNode * Cudd_addMinus(DdManager *dd, DdNode **f, DdNode **g);
// DdNode * Cudd_addMinimum(DdManager *dd, DdNode **f, DdNode **g);
// DdNode * Cudd_addMaximum(DdManager *dd, DdNode **f, DdNode **g);
// DdNode * Cudd_addOneZeroMaximum(DdManager *dd, DdNode **f, DdNode **g);
// DdNode * Cudd_addDiff(DdManager *dd, DdNode **f, DdNode **g);
// DdNode * Cudd_addAgreement(DdManager *dd, DdNode **f, DdNode **g);
DdNode * Cudd_addOr(DdManager *dd, DdNode **f, DdNode **g);
// DdNode * Cudd_addNand(DdManager *dd, DdNode **f, DdNode **g);
// DdNode * Cudd_addNor(DdManager *dd, DdNode **f, DdNode **g);
// DdNode * Cudd_addXor(DdManager *dd, DdNode **f, DdNode **g);
// DdNode * Cudd_addXnor(DdManager *dd, DdNode **f, DdNode **g);
// DdNode * Cudd_addMonadicApply(DdManager * dd, DD_MAOP op, DdNode * f);
// DdNode * Cudd_addLog(DdManager * dd, DdNode * f);
// DdNode * Cudd_addFindMax(DdManager *dd, DdNode *f);
// DdNode * Cudd_addFindMin(DdManager *dd, DdNode *f);
// DdNode * Cudd_addIthBit(DdManager *dd, DdNode *f, int bit);
// DdNode * Cudd_addScalarInverse(DdManager *dd, DdNode *f, DdNode *epsilon);
// DdNode * Cudd_addIte(DdManager *dd, DdNode *f, DdNode *g, DdNode *h);
// DdNode * Cudd_addIteConstant(DdManager *dd, DdNode *f, DdNode *g, DdNode *h);
// DdNode * Cudd_addEvalConst(DdManager *dd, DdNode *f, DdNode *g);
// int Cudd_addLeq(DdManager * dd, DdNode * f, DdNode * g);
DdNode * Cudd_addCmpl(DdManager *dd, DdNode *f);
DdNode * Cudd_addNegate(DdManager *dd, DdNode *f);
// DdNode * Cudd_addRoundOff(DdManager *dd, DdNode *f, int N);
// DdNode * Cudd_addWalsh(DdManager *dd, DdNode **x, DdNode **y, int n);
// DdNode * Cudd_addResidue(DdManager *dd, int n, int m, int options, int top);
// DdNode * Cudd_bddAndAbstract(DdManager *manager, DdNode *f, DdNode *g, DdNode *cube);
// DdNode * Cudd_bddAndAbstractLimit(DdManager *manager, DdNode *f, DdNode *g, DdNode *cube, unsigned int limit);
// int Cudd_ApaNumberOfDigits(int binaryDigits);
// DdApaNumber Cudd_NewApaNumber(int digits);
// void Cudd_FreeApaNumber(DdApaNumber number);
// void Cudd_ApaCopy(int digits, DdConstApaNumber source, DdApaNumber dest);
// DdApaDigit Cudd_ApaAdd(int digits, DdConstApaNumber a, DdConstApaNumber b, DdApaNumber sum);
// DdApaDigit Cudd_ApaSubtract(int digits, DdConstApaNumber a, DdConstApaNumber b, DdApaNumber diff);
// DdApaDigit Cudd_ApaShortDivision(int digits, DdConstApaNumber dividend, DdApaDigit divisor, DdApaNumber quotient);
// unsigned int Cudd_ApaIntDivision(int  digits, DdConstApaNumber dividend, unsigned int  divisor, DdApaNumber  quotient);
// void Cudd_ApaShiftRight(int digits, DdApaDigit in, DdConstApaNumber a, DdApaNumber b);
// void Cudd_ApaSetToLiteral(int digits, DdApaNumber number, DdApaDigit literal);
// void Cudd_ApaPowerOfTwo(int digits, DdApaNumber number, int power);
// int Cudd_ApaCompare(int digitsFirst, DdConstApaNumber first, int digitsSecond, DdConstApaNumber second);
// int Cudd_ApaCompareRatios(int digitsFirst, DdConstApaNumber firstNum, unsigned int firstDen, int digitsSecond, DdConstApaNumber secondNum, unsigned int secondDen);
// int Cudd_ApaPrintHex(FILE *fp, int digits, DdConstApaNumber number);
// int Cudd_ApaPrintDecimal(FILE *fp, int digits, DdConstApaNumber number);
// char * Cudd_ApaStringDecimal(int digits, DdConstApaNumber number);
// int Cudd_ApaPrintExponential(FILE * fp, int  digits, DdConstApaNumber number, int precision);
// DdApaNumber Cudd_ApaCountMinterm(DdManager const *manager, DdNode *node, int nvars, int *digits);
// int Cudd_ApaPrintMinterm(FILE *fp, DdManager const *dd, DdNode *node, int nvars);
// int Cudd_ApaPrintMintermExp(FILE * fp, DdManager const * dd, DdNode *node, int  nvars, int precision);
// int Cudd_ApaPrintDensity(FILE * fp, DdManager * dd, DdNode * node, int  nvars);
// DdNode * Cudd_UnderApprox(DdManager *dd, DdNode *f, int numVars, int threshold, int safe, double quality);
// DdNode * Cudd_OverApprox(DdManager *dd, DdNode *f, int numVars, int threshold, int safe, double quality);
// DdNode * Cudd_RemapUnderApprox(DdManager *dd, DdNode *f, int numVars, int threshold, double quality);
// DdNode * Cudd_RemapOverApprox(DdManager *dd, DdNode *f, int numVars, int threshold, double quality);
// DdNode * Cudd_BiasedUnderApprox(DdManager *dd, DdNode *f, DdNode *b, int numVars, int threshold, double quality1, double quality0);
// DdNode * Cudd_BiasedOverApprox(DdManager *dd, DdNode *f, DdNode *b, int numVars, int threshold, double quality1, double quality0);
DdNode * Cudd_bddExistAbstract(DdManager *manager, DdNode *f, DdNode *cube);
// DdNode * Cudd_bddExistAbstractLimit(DdManager * manager, DdNode * f, DdNode * cube, unsigned int limit);
// DdNode * Cudd_bddXorExistAbstract(DdManager *manager, DdNode *f, DdNode *g, DdNode *cube);
DdNode * Cudd_bddUnivAbstract(DdManager *manager, DdNode *f, DdNode *cube);
// DdNode * Cudd_bddBooleanDiff(DdManager *manager, DdNode *f, int x);
// int Cudd_bddVarIsDependent(DdManager *dd, DdNode *f, DdNode *var);
// double Cudd_bddCorrelation(DdManager *manager, DdNode *f, DdNode *g);
// double Cudd_bddCorrelationWeights(DdManager *manager, DdNode *f, DdNode *g, double *prob);
// DdNode * Cudd_bddIte(DdManager *dd, DdNode *f, DdNode *g, DdNode *h);
// DdNode * Cudd_bddIteLimit(DdManager *dd, DdNode *f, DdNode *g, DdNode *h, unsigned int limit);
// DdNode * Cudd_bddIteConstant(DdManager *dd, DdNode *f, DdNode *g, DdNode *h);
// DdNode * Cudd_bddIntersect(DdManager *dd, DdNode *f, DdNode *g);
DdNode * Cudd_bddAnd(DdManager *dd, DdNode *f, DdNode *g);
// DdNode * Cudd_bddAndLimit(DdManager *dd, DdNode *f, DdNode *g, unsigned int limit);
DdNode * Cudd_bddOr(DdManager *dd, DdNode *f, DdNode *g);
// DdNode * Cudd_bddOrLimit(DdManager *dd, DdNode *f, DdNode *g, unsigned int limit);
// DdNode * Cudd_bddNand(DdManager *dd, DdNode *f, DdNode *g);
// DdNode * Cudd_bddNor(DdManager *dd, DdNode *f, DdNode *g);
DdNode * Cudd_bddXor(DdManager *dd, DdNode *f, DdNode *g);
// DdNode * Cudd_bddXnor(DdManager *dd, DdNode *f, DdNode *g);
// DdNode * Cudd_bddXnorLimit(DdManager *dd, DdNode *f, DdNode *g, unsigned int limit);
int Cudd_bddLeq(DdManager *dd, DdNode *f, DdNode *g);
int Cudd_bddGeq(DdManager*, DdNodePtr f, DdNodePtr g); // this is not a Cudd function
DdNode * Cudd_addBddThreshold(DdManager *dd, DdNode *f, CUDD_VALUE_TYPE value);
DdNode * Cudd_addBddStrictThreshold(DdManager *dd, DdNode *f, CUDD_VALUE_TYPE value);
// DdNode * Cudd_addBddInterval(DdManager *dd, DdNode *f, CUDD_VALUE_TYPE lower, CUDD_VALUE_TYPE upper);
// DdNode * Cudd_addBddIthBit(DdManager *dd, DdNode *f, int bit);
// DdNode * Cudd_BddToAdd(DdManager *dd, DdNode *B);
// DdNode * Cudd_addBddPattern(DdManager *dd, DdNode *f);
// DdNode * Cudd_bddTransfer(DdManager *ddSource, DdManager *ddDestination, DdNode *f);
// int Cudd_DebugCheck(DdManager *table);
// int Cudd_CheckKeys(DdManager *table);
// DdNode * Cudd_bddClippingAnd(DdManager *dd, DdNode *f, DdNode *g, int maxDepth, int direction);
// DdNode * Cudd_bddClippingAndAbstract(DdManager *dd, DdNode *f, DdNode *g, DdNode *cube, int maxDepth, int direction);
// DdNode * Cudd_Cofactor(DdManager *dd, DdNode *f, DdNode *g);
// int Cudd_CheckCube(DdManager *dd, DdNode *g);
// int Cudd_VarsAreSymmetric(DdManager * dd, DdNode * f, int index1, int index2);
// DdNode * Cudd_bddCompose(DdManager *dd, DdNode *f, DdNode *g, int v);
// DdNode * Cudd_addCompose(DdManager *dd, DdNode *f, DdNode *g, int v);
// DdNode * Cudd_addPermute(DdManager *manager, DdNode *node, int *permut);
DdNode * Cudd_addSwapVariables(DdManager *dd, DdNode *f, DdNode **x, DdNode **y, int n);
// DdNode * Cudd_bddPermute(DdManager *manager, DdNode *node, int *permut);
// DdNode * Cudd_bddVarMap(DdManager *manager, DdNode *f);
// int Cudd_SetVarMap(DdManager *manager, DdNode **x, DdNode **y, int n);
DdNode * Cudd_bddSwapVariables(DdManager *dd, DdNode *f, DdNode **x, DdNode **y, int n);
// DdNode * Cudd_bddAdjPermuteX(DdManager *dd, DdNode *B, DdNode **x, int n);
// DdNode * Cudd_addVectorCompose(DdManager *dd, DdNode *f, DdNode **vector);
// DdNode * Cudd_addGeneralVectorCompose(DdManager *dd, DdNode *f, DdNode **vectorOn, DdNode **vectorOff);
// DdNode * Cudd_addNonSimCompose(DdManager *dd, DdNode *f, DdNode **vector);
// DdNode * Cudd_bddVectorCompose(DdManager *dd, DdNode *f, DdNode **vector);
// int Cudd_bddApproxConjDecomp(DdManager *dd, DdNode *f, DdNode ***conjuncts);
// int Cudd_bddApproxDisjDecomp(DdManager *dd, DdNode *f, DdNode ***disjuncts);
// int Cudd_bddIterConjDecomp(DdManager *dd, DdNode *f, DdNode ***conjuncts);
// int Cudd_bddIterDisjDecomp(DdManager *dd, DdNode *f, DdNode ***disjuncts);
// int Cudd_bddGenConjDecomp(DdManager *dd, DdNode *f, DdNode ***conjuncts);
// int Cudd_bddGenDisjDecomp(DdManager *dd, DdNode *f, DdNode ***disjuncts);
// int Cudd_bddVarConjDecomp(DdManager *dd, DdNode * f, DdNode ***conjuncts);
// int Cudd_bddVarDisjDecomp(DdManager *dd, DdNode * f, DdNode ***disjuncts);
// DdNode * Cudd_FindEssential(DdManager *dd, DdNode *f);
// int Cudd_bddIsVarEssential(DdManager *manager, DdNode *f, int id, int phase);
// DdTlcInfo * Cudd_FindTwoLiteralClauses(DdManager * dd, DdNode * f);
// int Cudd_PrintTwoLiteralClauses(DdManager * dd, DdNode * f, char **names, FILE *fp);
// int Cudd_ReadIthClause(DdTlcInfo * tlc, int i, unsigned *var1, unsigned *var2, int *phase1, int *phase2);
// void Cudd_tlcInfoFree(DdTlcInfo * t);
// int Cudd_DumpBlif(DdManager *dd, int n, DdNode **f, char const * const *inames, char const * const *onames, char *mname, FILE *fp, int mv);
// int Cudd_DumpBlifBody(DdManager *dd, int n, DdNode **f, char const * const *inames, char const * const *onames, FILE *fp, int mv);
int Cudd_DumpDot(DdManager *dd, int n, DdNode **f, char const * const *inames, char const * const *onames, FILE *fp);
// int Cudd_DumpDaVinci(DdManager *dd, int n, DdNode **f, char const * const *inames, char const * const *onames, FILE *fp);
// int Cudd_DumpDDcal(DdManager *dd, int n, DdNode **f, char const * const *inames, char const * const *onames, FILE *fp);
// int Cudd_DumpFactoredForm(DdManager *dd, int n, DdNode **f, char const * const *inames, char const * const *onames, FILE *fp);
// char * Cudd_FactoredFormString(DdManager *dd, DdNode *f, char const * const * inames);
// DdNode * Cudd_bddConstrain(DdManager *dd, DdNode *f, DdNode *c);
// DdNode * Cudd_bddRestrict(DdManager *dd, DdNode *f, DdNode *c);
// DdNode * Cudd_bddNPAnd(DdManager *dd, DdNode *f, DdNode *c);
// DdNode * Cudd_addConstrain(DdManager *dd, DdNode *f, DdNode *c);
// DdNode ** Cudd_bddConstrainDecomp(DdManager *dd, DdNode *f);
// DdNode * Cudd_addRestrict(DdManager *dd, DdNode *f, DdNode *c);
// DdNode ** Cudd_bddCharToVect(DdManager *dd, DdNode *f);
// DdNode * Cudd_bddLICompaction(DdManager *dd, DdNode *f, DdNode *c);
// DdNode * Cudd_bddSqueeze(DdManager *dd, DdNode *l, DdNode *u);
// DdNode * Cudd_bddInterpolate(DdManager * dd, DdNode * l, DdNode * u);
// DdNode * Cudd_bddMinimize(DdManager *dd, DdNode *f, DdNode *c);
// DdNode * Cudd_SubsetCompress(DdManager *dd, DdNode *f, int nvars, int threshold);
// DdNode * Cudd_SupersetCompress(DdManager *dd, DdNode *f, int nvars, int threshold);
// int Cudd_addHarwell(FILE *fp, DdManager *dd, DdNode **E, DdNode ***x, DdNode ***y, DdNode ***xn, DdNode ***yn_, int *nx, int *ny, int *m, int *n, int bx, int sx, int by, int sy, int pr);
DdManager * Cudd_Init(unsigned int numVars, unsigned int numVarsZ, unsigned int numSlots, unsigned int cacheSize, size_t maxMemory);
// void Cudd_Quit(DdManager *unique);
// int Cudd_PrintLinear(DdManager *table);
// int Cudd_ReadLinear(DdManager *table, int x, int y);
// DdNode * Cudd_bddLiteralSetIntersection(DdManager *dd, DdNode *f, DdNode *g);
// DdNode * Cudd_addMatrixMultiply(DdManager *dd, DdNode *A, DdNode *B, DdNode **z, int nz);
// DdNode * Cudd_addTimesPlus(DdManager *dd, DdNode *A, DdNode *B, DdNode **z, int nz);
// DdNode * Cudd_addTriangle(DdManager *dd, DdNode *f, DdNode *g, DdNode **z, int nz);
// DdNode * Cudd_addOuterSum(DdManager *dd, DdNode *M, DdNode *r, DdNode *c);
// DdNode * Cudd_PrioritySelect(DdManager *dd, DdNode *R, DdNode **x, DdNode **y, DdNode **z, DdNode *Pi, int n, DD_PRFP PiFunc);
// DdNode * Cudd_Xgty(DdManager *dd, int N, DdNode **z, DdNode **x, DdNode **y);
DdNode * Cudd_Xeqy(DdManager *dd, int N, DdNode **x, DdNode **y);
// DdNode * Cudd_addXeqy(DdManager *dd, int N, DdNode **x, DdNode **y);
// DdNode * Cudd_Dxygtdxz(DdManager *dd, int N, DdNode **x, DdNode **y, DdNode **z);
// DdNode * Cudd_Dxygtdyz(DdManager *dd, int N, DdNode **x, DdNode **y, DdNode **z);
// DdNode * Cudd_Inequality(DdManager * dd, int  N, int c, DdNode ** x, DdNode ** y);
// DdNode * Cudd_Disequality(DdManager * dd, int  N, int c, DdNode ** x, DdNode ** y);
// DdNode * Cudd_bddInterval(DdManager * dd, int  N, DdNode ** x, unsigned int lowerB, unsigned int upperB);
// DdNode * Cudd_CProjection(DdManager *dd, DdNode *R, DdNode *Y);
// DdNode * Cudd_addHamming(DdManager *dd, DdNode **xVars, DdNode **yVars, int nVars);
// int Cudd_MinHammingDist(DdManager *dd, DdNode *f, int *minterm, int upperBound);
// DdNode * Cudd_bddClosestCube(DdManager *dd, DdNode * f, DdNode *g, int *distance);
// int Cudd_addRead(FILE *fp, DdManager *dd, DdNode **E, DdNode ***x, DdNode ***y, DdNode ***xn, DdNode ***yn_, int *nx, int *ny, int *m, int *n, int bx, int sx, int by, int sy);
// int Cudd_bddRead(FILE *fp, DdManager *dd, DdNode **E, DdNode ***x, DdNode ***y, int *nx, int *ny, int *m, int *n, int bx, int sx, int by, int sy);
// void Cudd_Ref(DdNode *n);
// void Cudd_RecursiveDeref(DdManager *table, DdNode *n);
// void Cudd_IterDerefBdd(DdManager *table, DdNode *n);
// void Cudd_DelayedDerefBdd(DdManager * table, DdNode * n);
// void Cudd_RecursiveDerefZdd(DdManager *table, DdNode *n);
// void Cudd_Deref(DdNode *node);
// int Cudd_CheckZeroRef(DdManager *manager);
// int Cudd_ReduceHeap(DdManager *table, Cudd_ReorderingType heuristic, int minsize);
// int Cudd_ShuffleHeap(DdManager *table, int *permutation);
// DdNode * Cudd_Eval(DdManager *dd, DdNode *f, int *inputs);
// DdNode * Cudd_ShortestPath(DdManager *manager, DdNode *f, int *weight, int *support, int *length);
// DdNode * Cudd_LargestCube(DdManager *manager, DdNode *f, int *length);
// int Cudd_ShortestLength(DdManager *manager, DdNode *f, int *weight);
// DdNode * Cudd_Decreasing(DdManager *dd, DdNode *f, int i);
// DdNode * Cudd_Increasing(DdManager *dd, DdNode *f, int i);
// int Cudd_EquivDC(DdManager *dd, DdNode *F, DdNode *G, DdNode *D);
// int Cudd_bddLeqUnless(DdManager *dd, DdNode *f, DdNode *g, DdNode *D);
// int Cudd_EqualSupNorm(DdManager *dd, DdNode *f, DdNode *g, CUDD_VALUE_TYPE tolerance, int pr);
// DdNode * Cudd_bddMakePrime(DdManager *dd, DdNode *cube, DdNode *f);
// DdNode * Cudd_bddMaximallyExpand(DdManager *dd, DdNode *lb, DdNode *ub, DdNode *f);
// DdNode * Cudd_bddLargestPrimeUnate(DdManager *dd , DdNode *f, DdNode *phaseBdd);
// double * Cudd_CofMinterm(DdManager *dd, DdNode *node);
// DdNode * Cudd_SolveEqn(DdManager * bdd, DdNode *F, DdNode *Y, DdNode **G, int **yIndex, int n);
// DdNode * Cudd_VerifySol(DdManager * bdd, DdNode *F, DdNode **G, int *yIndex, int n);
// DdNode * Cudd_SplitSet(DdManager *manager, DdNode *S, DdNode **xVars, int n, double m);
// DdNode * Cudd_SubsetHeavyBranch(DdManager *dd, DdNode *f, int numVars, int threshold);
// DdNode * Cudd_SupersetHeavyBranch(DdManager *dd, DdNode *f, int numVars, int threshold);
// DdNode * Cudd_SubsetShortPaths(DdManager *dd, DdNode *f, int numVars, int threshold, int hardlimit);
// DdNode * Cudd_SupersetShortPaths(DdManager *dd, DdNode *f, int numVars, int threshold, int hardlimit);
// void Cudd_SymmProfile(DdManager *table, int lower, int upper);
// unsigned int Cudd_Prime(unsigned int p);
// int Cudd_Reserve(DdManager *manager, int amount);
// int Cudd_PrintMinterm(DdManager *manager, DdNode *node);
// int Cudd_bddPrintCover(DdManager *dd, DdNode *l, DdNode *u);
int Cudd_PrintDebug(DdManager *dd, DdNode *f, int n, int pr);
// int Cudd_PrintSummary(DdManager * dd, DdNode * f, int n, int mode);
int Cudd_DagSize(DdNode *node);
// int Cudd_EstimateCofactor(DdManager *dd, DdNode * node, int i, int phase);
// int Cudd_EstimateCofactorSimple(DdNode * node, int i);
// int Cudd_SharingSize(DdNode **nodeArray, int n);
double Cudd_CountMinterm(DdManager *manager, DdNode *node, int nvars);
// #ifdef EPD_H_
// int Cudd_EpdCountMinterm(DdManager const *manager, DdNode *node, int nvars, EpDouble *epd);
// #endif
// long double Cudd_LdblCountMinterm(DdManager const *manager, DdNode *node, int nvars);
// int Cudd_EpdPrintMinterm(DdManager const * dd, DdNode * node, int nvars);
double Cudd_CountPath(DdNode *node);
// double Cudd_CountPathsToNonZero(DdNode *node);
// int Cudd_SupportIndices(DdManager * dd, DdNode * f, int **indices);
DdNode * Cudd_Support(DdManager *dd, DdNode *f);
// int * Cudd_SupportIndex(DdManager *dd, DdNode *f);
int Cudd_SupportSize(DdManager *dd, DdNode *f);
// int Cudd_VectorSupportIndices(DdManager * dd, DdNode ** F, int n, int **indices);
// DdNode * Cudd_VectorSupport(DdManager *dd, DdNode **F, int n);
// int * Cudd_VectorSupportIndex(DdManager *dd, DdNode **F, int n);
// int Cudd_VectorSupportSize(DdManager *dd, DdNode **F, int n);
// int Cudd_ClassifySupport(DdManager *dd, DdNode *f, DdNode *g, DdNode **common, DdNode **onlyF, DdNode **onlyG);
// int Cudd_CountLeaves(DdNode *node);
// int Cudd_bddPickOneCube(DdManager *ddm, DdNode *node, char *string);
DdNode * Cudd_bddPickOneMinterm(DdManager *dd, DdNode *f, DdNode **vars, int n);
// DdNode ** Cudd_bddPickArbitraryMinterms(DdManager *dd, DdNode *f, DdNode **vars, int n, int k);
// DdNode * Cudd_SubsetWithMaskVars(DdManager *dd, DdNode *f, DdNode **vars, int nvars, DdNode **maskVars, int mvars);
// DdGen * Cudd_FirstCube(DdManager *dd, DdNode *f, int **cube, CUDD_VALUE_TYPE *value);
// int Cudd_NextCube(DdGen *gen, int **cube, CUDD_VALUE_TYPE *value);
// DdGen * Cudd_FirstPrime(DdManager *dd, DdNode *l, DdNode *u, int **cube);
// int Cudd_NextPrime(DdGen *gen, int **cube);
// DdNode * Cudd_bddComputeCube(DdManager *dd, DdNode **vars, int *phase, int n);
// DdNode * Cudd_addComputeCube(DdManager *dd, DdNode **vars, int *phase, int n);
// DdNode * Cudd_CubeArrayToBdd(DdManager *dd, int *array);
// int Cudd_BddToCubeArray(DdManager *dd, DdNode *cube, int *array);
// DdGen * Cudd_FirstNode(DdManager *dd, DdNode *f, DdNode **node);
// int Cudd_NextNode(DdGen *gen, DdNode **node);
// int Cudd_GenFree(DdGen *gen);
// int Cudd_IsGenEmpty(DdGen *gen);
// DdNode * Cudd_IndicesToCube(DdManager *dd, int *array, int n);
// void Cudd_PrintVersion(FILE *fp);
// double Cudd_AverageDistance(DdManager *dd);
// int32_t Cudd_Random(DdManager * dd);
// void Cudd_Srandom(DdManager * dd, int32_t seed);
// double Cudd_Density(DdManager *dd, DdNode *f, int nvars);
// void Cudd_OutOfMem(size_t size);
// void Cudd_OutOfMemSilent(size_t size);
// int Cudd_zddCount(DdManager *zdd, DdNode *P);
// double Cudd_zddCountDouble(DdManager *zdd, DdNode *P);
// DdNode * Cudd_zddProduct(DdManager *dd, DdNode *f, DdNode *g);
// DdNode * Cudd_zddUnateProduct(DdManager *dd, DdNode *f, DdNode *g);
// DdNode * Cudd_zddWeakDiv(DdManager *dd, DdNode *f, DdNode *g);
// DdNode * Cudd_zddDivide(DdManager *dd, DdNode *f, DdNode *g);
// DdNode * Cudd_zddWeakDivF(DdManager *dd, DdNode *f, DdNode *g);
// DdNode * Cudd_zddDivideF(DdManager *dd, DdNode *f, DdNode *g);
// DdNode * Cudd_zddComplement(DdManager *dd, DdNode *node);
// DdNode * Cudd_zddIsop(DdManager *dd, DdNode *L, DdNode *U, DdNode **zdd_I);
// DdNode * Cudd_bddIsop(DdManager *dd, DdNode *L, DdNode *U);
// DdNode * Cudd_MakeBddFromZddCover(DdManager *dd, DdNode *node);
// int Cudd_zddDagSize(DdNode *p_node);
// double Cudd_zddCountMinterm(DdManager *zdd, DdNode *node, int path);
// void Cudd_zddPrintSubtable(DdManager *table);
// DdNode * Cudd_zddPortFromBdd(DdManager *dd, DdNode *B);
// DdNode * Cudd_zddPortToBdd(DdManager *dd, DdNode *f);
// int Cudd_zddReduceHeap(DdManager *table, Cudd_ReorderingType heuristic, int minsize);
// int Cudd_zddShuffleHeap(DdManager *table, int *permutation);
// DdNode * Cudd_zddIte(DdManager *dd, DdNode *f, DdNode *g, DdNode *h);
// DdNode * Cudd_zddUnion(DdManager *dd, DdNode *P, DdNode *Q);
// DdNode * Cudd_zddIntersect(DdManager *dd, DdNode *P, DdNode *Q);
// DdNode * Cudd_zddDiff(DdManager *dd, DdNode *P, DdNode *Q);
// DdNode * Cudd_zddDiffConst(DdManager *zdd, DdNode *P, DdNode *Q);
// DdNode * Cudd_zddSubset1(DdManager *dd, DdNode *P, int var);
// DdNode * Cudd_zddSubset0(DdManager *dd, DdNode *P, int var);
// DdNode * Cudd_zddChange(DdManager *dd, DdNode *P, int var);
// void Cudd_zddSymmProfile(DdManager *table, int lower, int upper);
// int Cudd_zddPrintMinterm(DdManager *zdd, DdNode *node);
// int Cudd_zddPrintCover(DdManager *zdd, DdNode *node);
// int Cudd_zddPrintDebug(DdManager *zdd, DdNode *f, int n, int pr);
// DdGen * Cudd_zddFirstPath(DdManager *zdd, DdNode *f, int **path);
// int Cudd_zddNextPath(DdGen *gen, int **path);
// char * Cudd_zddCoverPathToString(DdManager *zdd, int *path, char *str);
// DdNode * Cudd_zddSupport(DdManager * dd, DdNode * f);
// int Cudd_zddDumpDot(DdManager *dd, int n, DdNode **f, char const * const *inames, char const * const *onames, FILE *fp);
// int Cudd_bddSetPiVar(DdManager *dd, int index);
// int Cudd_bddSetPsVar(DdManager *dd, int index);
// int Cudd_bddSetNsVar(DdManager *dd, int index);
// int Cudd_bddIsPiVar(DdManager *dd, int index);
// int Cudd_bddIsPsVar(DdManager *dd, int index);
// int Cudd_bddIsNsVar(DdManager *dd, int index);
// int Cudd_bddSetPairIndex(DdManager *dd, int index, int pairIndex);
// int Cudd_bddReadPairIndex(DdManager *dd, int index);
// int Cudd_bddSetVarToBeGrouped(DdManager *dd, int index);
// int Cudd_bddSetVarHardGroup(DdManager *dd, int index);
// int Cudd_bddResetVarToBeGrouped(DdManager *dd, int index);
// int Cudd_bddIsVarToBeGrouped(DdManager *dd, int index);
// int Cudd_bddSetVarToBeUngrouped(DdManager *dd, int index);
// int Cudd_bddIsVarToBeUngrouped(DdManager *dd, int index);
// int Cudd_bddIsVarHardGroup(DdManager *dd, int index);
// #ifdef MTR_H_
// MtrNode * Cudd_ReadTree(DdManager *dd);
// void Cudd_SetTree(DdManager *dd, MtrNode *tree);
// void Cudd_FreeTree(DdManager *dd);
// MtrNode * Cudd_ReadZddTree(DdManager *dd);
// void Cudd_SetZddTree(DdManager *dd, MtrNode *tree);
// void Cudd_FreeZddTree(DdManager *dd);
// MtrNode * Cudd_MakeTreeNode(DdManager *dd, unsigned int low, unsigned int size, unsigned int type);
// MtrNode * Cudd_MakeZddTreeNode(DdManager *dd, unsigned int low, unsigned int size, unsigned int type);
// #endif

typedef struct {} MtrNode; // dummy
#define MTR_DEFAULT 0x00000000
#define MTR_FIXED   0x00000004
MtrNode* Cudd_MakeTreeNode(DdManager*, unsigned int low, unsigned int size, unsigned int type);

#ifdef __cplusplus
}
}
#endif

#endif
