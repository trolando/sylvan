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

#include <sylvan_cuddObj.hpp>

using namespace sylvan_cudd;

DdManager*
DD::manager() const
{
    return NULL;
}

DdNodePtr
DD::getNode() const
{ 
    return node; 
}

DdNodePtr
DD::getRegularNode() const 
{
    return Cudd_Regular(node); 
}

int
DD::nodeCount() const 
{ 
    return Cudd_DagSize(node); 
}

unsigned int
DD::NodeReadIndex() const
{
    return Cudd_NodeReadIndex(node); 
}

ABDD::ABDD()
{
    node = (DdNodePtr)sylvan::mtbdd_false;
    sylvan::mtbdd_protect((sylvan::MTBDD*)&node);
}

ABDD::~ABDD()
{
    sylvan::mtbdd_unprotect((sylvan::MTBDD*)&node);
}

ABDD::ABDD(Cudd const &manager, DdNodePtr ddNode)
{
    node = ddNode;
    sylvan::mtbdd_protect((sylvan::MTBDD*)&node);
    (void)manager;
}

ABDD::ABDD(const ABDD &from)
{
    node = from.node;
    sylvan::mtbdd_protect((sylvan::MTBDD*)&node);
}

ABDD::ABDD(DdNodePtr ddNode)
{
    node = ddNode;
    sylvan::mtbdd_protect((sylvan::MTBDD*)&node);
}

BDD
ABDD::Support() const
{
    return Cudd_Support(NULL, node);
}

int
ABDD::SupportSize() const
{
    return Cudd_SupportSize(NULL, node);
}

bool
ABDD::operator==(const ABDD &other) const
{
    return node == other.node; 
}

bool
ABDD::operator!=(const ABDD &other) const
{
    return node != other.node; 
}

void
ABDD::print(int nvars, int verbosity) const
{
    // print to stdout
    fflush(stdout);
    if ((sylvan::MTBDD)node == sylvan::mtbdd_false) printf("empty DD."); // TODO call defaultError
    Cudd_PrintDebug(NULL, node, nvars, verbosity);
}

bool
ABDD::IsOne() const
{
    return node == Cudd_ReadOne(NULL);
}

/*
bool
ABDD::IsCube() const
{
    return Cudd_CheckCube(NULL, node);
}
*/

double
ABDD::CountMinterm(int nvars) const
{
    return Cudd_CountMinterm(NULL, node, nvars);
}

double
ABDD::CountPath() const
{
    return Cudd_CountPath(node);
}

BDD::BDD() : ABDD() {}
BDD::BDD(Cudd const & manager, DdNodePtr ddNode) : ABDD(manager, ddNode) {}
BDD::BDD(DdNodePtr ddNode) : ABDD(ddNode) {}
BDD::BDD(const BDD& other) : ABDD(other) {}

BDD&
BDD::operator=(const BDD& right)
{
    node = right.node;
    return *this;
}

BDD
BDD::operator&(const BDD& other) const
{
    return BDD(Cudd_bddAnd(NULL, node, other.node));
}

BDD&
BDD::operator&=(const BDD& other)
{
    node = Cudd_bddAnd(NULL, node, other.node);
    return *this;
}

BDD
BDD::operator*(const BDD& other) const
{
    return BDD(Cudd_bddAnd(NULL, node, other.node));
}

BDD&
BDD::operator*=(const BDD& other)
{
    node = Cudd_bddAnd(NULL, node, other.node);
    return *this;
}

BDD
BDD::operator|(const BDD& other) const
{
    return BDD(Cudd_bddOr(NULL, node, other.node));
}

BDD&
BDD::operator|=(const BDD& other)
{
    node = Cudd_bddOr(NULL, node, other.node);
    return *this;
}

BDD
BDD::operator+(const BDD& other) const
{
    return BDD(Cudd_bddOr(NULL, node, other.node));
}

BDD&
BDD::operator+=(const BDD& other)
{
    node = Cudd_bddOr(NULL, node, other.node);
    return *this;
}

BDD
BDD::operator-(const BDD& other) const
{
    return BDD(Cudd_bddAnd(NULL, node, Cudd_Not(other.node)));
}

BDD&
BDD::operator-=(const BDD& other)
{
    node = Cudd_bddAnd(NULL, node, Cudd_Not(other.node));
    return *this;
}

BDD
BDD::operator^(const BDD& other) const
{
    return BDD(Cudd_bddXor(NULL, node, Cudd_Not(other.node)));
}

BDD&
BDD::operator^=(const BDD& other)
{
    node = Cudd_bddXor(NULL, node, Cudd_Not(other.node));
    return *this;
}

BDD
BDD::operator!() const
{
    return BDD(Cudd_Not(node));
}

BDD
BDD::operator~() const
{
    return BDD(Cudd_Not(node));
}

BDD
BDD::PickOneMinterm(std::vector<BDD> vars) const
{
    const int size = vars.size();
    DdNodePtr *V = new DdNodePtr[size];
    for (int i=0; i<size; i++) V[i] = vars[i].node;
    DdNodePtr result = Cudd_bddPickOneMinterm(NULL, node, V, size);
    delete[] V;
    return BDD(result);
}

BDD
BDD::SwapVariables(std::vector<BDD> x, std::vector<BDD> y) const
{
    const int size = x.size();
    DdNodePtr *X = new DdNodePtr[size];
    DdNodePtr *Y = new DdNodePtr[size];
    for (int i=0; i<size; i++) {
        X[i] = x[i].node;
        Y[i] = y[i].node;
    }
    DdNodePtr result = Cudd_bddSwapVariables(NULL, node, X, Y, size);
    delete[] X;
    delete[] Y;
    return BDD(result);
}

bool
BDD::operator<=(const BDD& other) const
{
    return Cudd_bddLeq(NULL, node, other.node);
}

bool
BDD::operator>=(const BDD& other) const
{
    return Cudd_bddGeq(NULL, node, other.node);
}

bool
BDD::operator<(const BDD& other) const
{
    return node != other.node && Cudd_bddLeq(NULL, node, other.node);
}

bool
BDD::operator>(const BDD& other) const
{
    return node != other.node && Cudd_bddGeq(NULL, node, other.node);
}

BDD
BDD::ExistAbstract(const BDD& cube, unsigned int limit) const
{
    assert(limit == 0);
    return BDD(Cudd_bddExistAbstract(NULL, node, cube.node));
    (void)limit;
}

BDD
BDD::UnivAbstract(const BDD& cube) const
{
    return BDD(Cudd_bddUnivAbstract(NULL, node, cube.node));
}

ADD::ADD() : ABDD() {}
ADD::ADD(Cudd const & manager, DdNodePtr ddNode) : ABDD(manager, ddNode) {}
ADD::ADD(DdNodePtr ddNode) : ABDD(ddNode) {}
ADD::ADD(const ADD& other) : ABDD(other) {}

ADD&
ADD::operator=(const ADD& right)
{
    node = right.node;
    return *this;
}

ADD
ADD::operator-() const
{
    return ADD(Cudd_addNegate(NULL, node));
}

ADD
ADD::operator~() const
{
    return ADD(Cudd_addCmpl(NULL, node));
}

ADD
ADD::operator*(const ADD& other) const
{
    return ADD(Cudd_addApply(NULL, Cudd_addTimes, node, other.node));
}

ADD&
ADD::operator*=(const ADD& other)
{
    node = Cudd_addApply(NULL, Cudd_addTimes, node, other.node);
    return *this;
}

ADD
ADD::operator+(const ADD& other) const
{
    return ADD(Cudd_addApply(NULL, Cudd_addPlus, node, other.node));
}

ADD&
ADD::operator+=(const ADD& other)
{
    node = Cudd_addApply(NULL, Cudd_addPlus, node, other.node);
    return *this;
}

ADD
ADD::operator-(const ADD& other) const
{
    return ADD(Cudd_addApply(NULL, Cudd_addMinus, node, other.node));
}

ADD&
ADD::operator-=(const ADD& other)
{
    node = Cudd_addApply(NULL, Cudd_addMinus, node, other.node);
    return *this;
}

ADD
ADD::operator&(const ADD& other) const
{
    return ADD(Cudd_addApply(NULL, Cudd_addTimes, node, other.node));
}

ADD&
ADD::operator&=(const ADD& other)
{
    node = Cudd_addApply(NULL, Cudd_addTimes, node, other.node);
    return *this;
}

ADD
ADD::Threshold(const ADD& g) const
{
    return ADD(Cudd_addApply(NULL, Cudd_addThreshold, node, g.node));
}

BDD
ADD::BddThreshold(CUDD_VALUE_TYPE value) const
{
    return BDD(Cudd_addBddThreshold(NULL, node, value));
}

BDD
ADD::BddStrictThreshold(CUDD_VALUE_TYPE value) const
{
    return BDD(Cudd_addBddStrictThreshold(NULL, node, value));
}

ADD
ADD::ExistAbstract(const ADD& cube) const
{
    return ADD(Cudd_addExistAbstract(NULL, node, cube.node));
}

ADD
ADD::UnivAbstract(const ADD& cube) const
{
    return ADD(Cudd_addUnivAbstract(NULL, node, cube.node));
}

ADD
ADD::operator|(const ADD& other) const
{
    return ADD(Cudd_addApply(NULL, Cudd_addOr, node, other.node));
}

ADD&
ADD::operator|=(const ADD& other)
{
    node = Cudd_addApply(NULL, Cudd_addOr, node, other.node);
    return *this;
}

ADD
ADD::Divide(const ADD& other) const
{
    return ADD(Cudd_addApply(NULL, Cudd_addDivide, node, other.node));
}

bool
ADD::IsZero() const
{
    return node == Cudd_ReadZero(NULL);
}

ADD
ADD::SwapVariables(std::vector<ADD> x, std::vector<ADD> y) const
{
    const int size = x.size();
    assert(size == (int)y.size());
    DdNodePtr *X = new DdNodePtr[size];
    DdNodePtr *Y = new DdNodePtr[size];
    for (int i=0; i<size; i++) {
        X[i] = x[i].node;
        Y[i] = y[i].node;
    }
    // add=bdd in this case..
    DdNodePtr result = Cudd_addSwapVariables(NULL, node, X, Y, size);
    delete[] X;
    delete[] Y;
    return ADD(result);
}

BDD
Cudd::Xeqy(std::vector<BDD> x, std::vector<BDD> y) const
{
    const int size = x.size();
    assert(size == (int)y.size());
    DdNodePtr *X = new DdNodePtr[size];
    DdNodePtr *Y = new DdNodePtr[size];
    for (int i=0; i<size; i++) {
        X[i] = x[i].node;
        Y[i] = y[i].node;
    }
    DdNodePtr result = Cudd_Xeqy(NULL, size, X, Y);
    delete[] X;
    delete[] Y;
    return BDD(result);
}

Cudd::Cudd(
      unsigned int numVars,
      unsigned int numVarsZ,
      unsigned int numSlots,
      unsigned int cacheSize,
      unsigned long maxMemory,
      PFC defaultHandler)
{
    Cudd_Init(numVars, numVarsZ, numSlots, cacheSize, maxMemory);
    (void)defaultHandler;
}

Cudd::~Cudd(void)
{
    sylvan::sylvan_quit();
    lace_exit();
}

DdManager*
Cudd::getManager(void) const
{
    return NULL;
}

void
Cudd::info() const
{
    Cudd_PrintInfo(NULL, stdout);
}

size_t
Cudd::ReadMemoryInUse() const
{
    return Cudd_ReadMemoryInUse(NULL);
}

BDD
Cudd::bddZero(void) const
{
    return Cudd_ReadLogicZero(NULL);
}

BDD
Cudd::bddOne(void) const
{
    return BDD(Cudd_ReadLogicOne(NULL));
    // NOTE: Cudd uses Cudd_ReadOne(NULL) which is really silly
}

ADD
Cudd::addZero(void) const
{
    return Cudd_ReadZero(NULL);
}

ADD
Cudd::addOne(void) const
{
    return Cudd_ReadOne(NULL);
}

ADD
Cudd::constant(double c) const
{
    return Cudd_addConst(NULL, c);
}

BDD
Cudd::bddVar(int index) const
{
    return BDD(Cudd_bddIthVar(NULL, index));
}

ADD
Cudd::addVar(int index) const
{
    return ADD(Cudd_addIthVar(NULL, index));
}

void
Cudd::DumpDot(const std::vector<BDD>& nodes, char const * const * inames, char const * const * onames, FILE * fp) const
{
    int n = nodes.size();
    DdNodePtr *F = new DdNodePtr[n];
    for (int i=0; i<n; i++) F[i] = nodes[i].node;
    Cudd_DumpDot(NULL, (int) n, F, inames, onames, fp);
    delete[] F;
}

void
Cudd::DumpDot(const std::vector<ADD>& nodes, char const * const * inames, char const * const * onames, FILE * fp) const
{
    int n = nodes.size();
    DdNodePtr *F = new DdNodePtr[n];
    for (int i=0; i<n; i++) F[i] = nodes[i].node;
    Cudd_DumpDot(NULL, (int) n, F, inames, onames, fp);
    delete[] F;
}
