/*
 * Copyright 2011-2016 Formal Methods and Tools, University of Twente
 * Copyright 2016 Tom van Dijk, Johannes Kepler University Linz
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

#include <sylvan/obj.hpp>

using namespace sylvan;

/***
 * Implementation of class Bdd
 */

bool
Bdd::operator==(const Bdd& other) const
{
    return bdd == other.bdd;
}

bool
Bdd::operator!=(const Bdd& other) const
{
    return bdd != other.bdd;
}

Bdd&
Bdd::operator=(const Bdd& right)
{
    bdd = right.bdd;
    return *this;
}

bool
Bdd::operator<=(const Bdd& other) const
{
    return bdd_subset(this->bdd, other.bdd) == 1;
}

bool
Bdd::operator>=(const Bdd& other) const
{
    // TODO: better implementation, since we are not interested in the BDD result
    return other <= *this;
}

bool
Bdd::operator<(const Bdd& other) const
{
    return bdd != other.bdd && *this <= other;
}

bool
Bdd::operator>(const Bdd& other) const
{
    return bdd != other.bdd && *this >= other;
}

Bdd
Bdd::operator!() const
{
    return Bdd(bdd_not(bdd));
}

Bdd
Bdd::operator~() const
{
    return Bdd(bdd_not(bdd));
}

Bdd
Bdd::operator*(const Bdd& other) const
{
    return Bdd(bdd_and(bdd, other.bdd));
}

Bdd&
Bdd::operator*=(const Bdd& other)
{
    bdd = bdd_and(bdd, other.bdd);
    return *this;
}

Bdd
Bdd::operator&(const Bdd& other) const
{
    return Bdd(bdd_and(bdd, other.bdd));
}

Bdd&
Bdd::operator&=(const Bdd& other)
{
    bdd = bdd_and(bdd, other.bdd);
    return *this;
}

Bdd
Bdd::operator+(const Bdd& other) const
{
    return Bdd(bdd_or(bdd, other.bdd));
}

Bdd&
Bdd::operator+=(const Bdd& other)
{
    bdd = bdd_or(bdd, other.bdd);
    return *this;
}

Bdd
Bdd::operator|(const Bdd& other) const
{
    return Bdd(bdd_or(bdd, other.bdd));
}

Bdd&
Bdd::operator|=(const Bdd& other)
{
    bdd = bdd_or(bdd, other.bdd);
    return *this;
}

Bdd
Bdd::operator^(const Bdd& other) const
{
    return Bdd(bdd_xor(bdd, other.bdd));
}

Bdd&
Bdd::operator^=(const Bdd& other)
{
    bdd = bdd_xor(bdd, other.bdd);
    return *this;
}

Bdd
Bdd::operator-(const Bdd& other) const
{
    return Bdd(bdd_and(bdd, bdd_not(other.bdd)));
}

Bdd&
Bdd::operator-=(const Bdd& other)
{
    bdd = bdd_and(bdd, bdd_not(other.bdd));
    return *this;
}

Bdd
Bdd::AndAbstract(const Bdd &g, const BddSet &cube) const
{
    return bdd_and_exists(bdd, g.bdd, cube.set.bdd);
}

Bdd
Bdd::ExistAbstract(const BddSet &cube) const
{
    return bdd_exists(bdd, cube.set.bdd);
}

Bdd
Bdd::UnivAbstract(const BddSet &cube) const
{
    return bdd_forall(bdd, cube.set.bdd);
}

Bdd
Bdd::Ite(const Bdd &g, const Bdd &h) const
{
    return bdd_ite(bdd, g.bdd, h.bdd);
}

Bdd
Bdd::And(const Bdd &g) const
{
    return bdd_and(bdd, g.bdd);
}

Bdd
Bdd::Or(const Bdd &g) const
{
    return bdd_or(bdd, g.bdd);
}

Bdd
Bdd::Nand(const Bdd &g) const
{
    return bdd_nand(bdd, g.bdd);
}

Bdd
Bdd::Nor(const Bdd &g) const
{
    return bdd_nor(bdd, g.bdd);
}

Bdd
Bdd::Xor(const Bdd &g) const
{
    return bdd_xor(bdd, g.bdd);
}

Bdd
Bdd::Xnor(const Bdd &g) const
{
    return bdd_equiv(bdd, g.bdd);
}

bool
Bdd::Disjoint(const Bdd &g) const
{
    return bdd_disjoint(bdd, g.bdd) == 1;
}

bool
Bdd::Leq(const Bdd &g) const
{
    return bdd_subset(bdd, g.bdd) == 1;
}

Bdd
Bdd::RelPrev(const Bdd& relation, const BddSet& cube) const
{
    return bdd_relprev(relation.bdd, bdd, cube.set.bdd);
}

Bdd
Bdd::RelNext(const Bdd &relation, const BddSet &cube) const
{
    return bdd_relnext(bdd, relation.bdd, cube.set.bdd);
}

Bdd
Bdd::Closure() const
{
    return bdd_closure(bdd);
}

Bdd
Bdd::Constrain(const Bdd &c) const
{
    return bdd_constrain(bdd, c.bdd);
}

Bdd
Bdd::Restrict(const Bdd &c) const
{
    return bdd_restrict(bdd, c.bdd);
}

Bdd
Bdd::Compose(const BddMap &m) const
{
    return bdd_compose(bdd, m.bdd);
}

Bdd
Bdd::Permute(const std::vector<uint32_t>& from, const std::vector<uint32_t>& to) const
{
    /* Create a map */
    BddMap map;
    for (int i=from.size()-1; i>=0; i--) {
        map.put(from[i], Bdd::bddVar(to[i]));
    }

    return bdd_compose(bdd, map.bdd);
}

Bdd
Bdd::Support() const
{
    return mtbdd_support(bdd);
}

BDD
Bdd::GetBDD() const
{
    return bdd;
}

void
Bdd::PrintDot(FILE *out) const
{
    mtbdd_fprintdot(out, bdd);
}

void
Bdd::GetShaHash(char *string) const
{
    mtbdd_getsha(bdd, string);
}

std::string
Bdd::GetShaHash() const
{
    char buf[65];
    mtbdd_getsha(bdd, buf);
    return std::string(buf);
}

double
Bdd::SatCount(const BddSet &variables) const
{
    return bdd_satcount(bdd, variables.set.bdd);
}

double
Bdd::SatCount(size_t nvars) const
{
    // Note: the mtbdd_satcount can be called without initializing the MTBDD module.
    return mtbdd_satcount(bdd, nvars);
}

void
Bdd::PickOneCube(const BddSet &variables, uint8_t *values) const
{
    bdd_sat_one(bdd, variables.set.bdd, values);
}

std::vector<bool>
Bdd::PickOneCube(const BddSet &variables) const
{
    std::vector<bool> result = std::vector<bool>();

    BDD bdd = this->bdd;
    BDD vars = variables.set.bdd;

    if (bdd == mtbdd_false) return result;

    for (; !mtbdd_set_isempty(vars); vars = mtbdd_set_next(vars)) {
        uint32_t var = mtbdd_set_first(vars);
        if (bdd == mtbdd_true) {
            // pick 0
            result.push_back(false);
        } else {
            if (mtbdd_getvar(bdd) != var) {
                // pick 0
                result.push_back(false);
            } else {
                if (mtbdd_getlow(bdd) == mtbdd_false) {
                    // pick 1
                    result.push_back(true);
                    bdd = mtbdd_gethigh(bdd);
                } else {
                    // pick 0
                    result.push_back(false);
                    bdd = mtbdd_getlow(bdd);
                }
            }
        }
    }

    return result;
}

Bdd
Bdd::PickOneCube() const
{
    return Bdd(bdd_sat_one_bdd(bdd));
}

Bdd
Bdd::UnionCube(const BddSet &variables, uint8_t *values) const
{
    return bdd_union_cube(bdd, variables.set.bdd, values);
}

Bdd
Bdd::UnionCube(const BddSet &variables, std::vector<uint8_t> values) const
{
    uint8_t *data = values.data();
    return bdd_union_cube(bdd, variables.set.bdd, data);
}

/**
 * @brief Generate a cube representing a set of variables
 */
Bdd
Bdd::VectorCube(const std::vector<Bdd> variables)
{
    Bdd result = Bdd::bddOne();
    for (int i=variables.size()-1; i>=0; i--) {
        result *= variables[i];
    }
    return result;
}

/**
 * @brief Generate a cube representing a set of variables
 */
Bdd
Bdd::VariablesCube(std::vector<uint32_t> variables)
{
    BDD result = mtbdd_true;
    for (int i=variables.size()-1; i>=0; i--) {
        result = mtbdd_makenode(variables[i], mtbdd_false, result);
    }
    return result;
}

size_t
Bdd::NodeCount() const
{
    return mtbdd_nodecount(bdd);
}

Bdd
Bdd::bddOne()
{
    return mtbdd_true;
}

Bdd
Bdd::bddZero()
{
    return mtbdd_false;
}

Bdd
Bdd::bddVar(uint32_t index)
{
    return mtbdd_ithvar(index);
}

Bdd
Bdd::bddCube(const BddSet &variables, uint8_t *values)
{
    return bdd_cube(variables.set.bdd, values);
}

Bdd
Bdd::bddCube(const BddSet &variables, std::vector<uint8_t> values)
{
    uint8_t *data = values.data();
    return bdd_cube(variables.set.bdd, data);
}

bool
Bdd::isConstant() const
{
    return bdd == mtbdd_true || bdd == mtbdd_false;
}

bool
Bdd::isTerminal() const
{
    return bdd == mtbdd_true || bdd == mtbdd_false;
}

bool
Bdd::isOne() const
{
    return bdd == mtbdd_true;
}

bool
Bdd::isZero() const
{
    return bdd == mtbdd_false;
}

uint32_t
Bdd::TopVar() const
{
    return mtbdd_getvar(bdd);
}

Bdd
Bdd::Then() const
{
    return Bdd(mtbdd_gethigh(bdd));
}

Bdd
Bdd::Else() const
{
    return Bdd(mtbdd_getlow(bdd));
}

/***
 * Implementation of class BddMap
 */

BddMap::BddMap(uint32_t key_variable, const Bdd value)
{
    bdd = mtbdd_map_add(mtbdd_map_empty(), key_variable, value.bdd);
}


BddMap
BddMap::operator+(const Bdd& other) const
{
    return BddMap(mtbdd_map_update(bdd, other.bdd));
}

BddMap&
BddMap::operator+=(const Bdd& other)
{
    bdd = mtbdd_map_update(bdd, other.bdd);
    return *this;
}

BddMap
BddMap::operator-(const Bdd& other) const
{
    return BddMap(mtbdd_map_removeall(bdd, other.bdd));
}

BddMap&
BddMap::operator-=(const Bdd& other)
{
    bdd = mtbdd_map_removeall(bdd, other.bdd);
    return *this;
}

void
BddMap::put(uint32_t key, Bdd value)
{
    bdd = mtbdd_map_add(bdd, key, value.bdd);
}

void
BddMap::removeKey(uint32_t key)
{
    bdd = mtbdd_map_remove(bdd, key);
}

size_t
BddMap::size() const
{
    return mtbdd_map_count(bdd);
}

bool
BddMap::isEmpty() const
{
    return mtbdd_map_isempty(bdd);
}


/***
 * Implementation of class Mtbdd
 */

Mtbdd
Mtbdd::int64Terminal(int64_t value)
{
    return mtbdd_int64(value);
}

Mtbdd
Mtbdd::doubleTerminal(double value)
{
    return mtbdd_double(value);
}

Mtbdd
Mtbdd::fractionTerminal(int64_t nominator, uint64_t denominator)
{
    return mtbdd_fraction(nominator, denominator);
}

Mtbdd
Mtbdd::terminal(uint32_t type, uint64_t value)
{
    return mtbdd_makeleaf(type, value);
}

Mtbdd
Mtbdd::mtbddVar(uint32_t variable)
{
    return mtbdd_makenode(variable, mtbdd_false, mtbdd_true);
}

Mtbdd
Mtbdd::mtbddOne()
{
    return mtbdd_true;
}

Mtbdd
Mtbdd::mtbddZero()
{
    return mtbdd_false;
}

Mtbdd
Mtbdd::mtbddCube(const BddSet &variables, uint8_t *values, const Mtbdd &terminal)
{
    return mtbdd_cube(variables.set.bdd, values, terminal.mtbdd);
}

Mtbdd
Mtbdd::mtbddCube(const BddSet &variables, std::vector<uint8_t> values, const Mtbdd &terminal)
{
    uint8_t *data = values.data();
    return mtbdd_cube(variables.set.bdd, data, terminal.mtbdd);
}

bool
Mtbdd::isTerminal() const
{
    return mtbdd_isleaf(mtbdd);
}

bool
Mtbdd::isLeaf() const
{
    return mtbdd_isleaf(mtbdd);
}

bool
Mtbdd::isOne() const
{
    return mtbdd == mtbdd_true;
}

bool
Mtbdd::isZero() const
{
    return mtbdd == mtbdd_false;
}

uint32_t
Mtbdd::TopVar() const
{
    return mtbdd_getvar(mtbdd);
}

Mtbdd
Mtbdd::Then() const
{
    return mtbdd_isnode(mtbdd) ? mtbdd_gethigh(mtbdd) : mtbdd;
}

Mtbdd
Mtbdd::Else() const
{
    return mtbdd_isnode(mtbdd) ? mtbdd_getlow(mtbdd) : mtbdd;
}

Mtbdd
Mtbdd::Negate() const
{
    return mtbdd_negate(mtbdd);
}

Mtbdd
Mtbdd::Apply(const Mtbdd &other, mtbdd_apply_op op) const
{
    return mtbdd_apply(mtbdd, other.mtbdd, op);
}

Mtbdd
Mtbdd::UApply(mtbdd_uapply_op op, size_t param) const
{
    return mtbdd_uapply(mtbdd, op, param);
}

Mtbdd
Mtbdd::Abstract(const BddSet &variables, mtbdd_abstract_op op) const
{
    return mtbdd_abstract(mtbdd, variables.set.bdd, op);
}

Mtbdd
Mtbdd::Ite(const Mtbdd &g, const Mtbdd &h) const
{
    return mtbdd_ite(mtbdd, g.mtbdd, h.mtbdd);
}

Mtbdd
Mtbdd::Plus(const Mtbdd &other) const
{
    return mtbdd_plus(mtbdd, other.mtbdd);
}

Mtbdd
Mtbdd::Times(const Mtbdd &other) const
{
    return mtbdd_times(mtbdd, other.mtbdd);
}

Mtbdd
Mtbdd::Min(const Mtbdd &other) const
{
    return mtbdd_min(mtbdd, other.mtbdd);
}

Mtbdd
Mtbdd::Max(const Mtbdd &other) const
{
    return mtbdd_max(mtbdd, other.mtbdd);
}

Mtbdd
Mtbdd::AbstractPlus(const BddSet &variables) const
{
    return mtbdd_abstract_plus(mtbdd, variables.set.bdd);
}

Mtbdd
Mtbdd::AbstractTimes(const BddSet &variables) const
{
    return mtbdd_abstract_times(mtbdd, variables.set.bdd);
}

Mtbdd
Mtbdd::AbstractMin(const BddSet &variables) const
{
    return mtbdd_abstract_min(mtbdd, variables.set.bdd);
}

Mtbdd
Mtbdd::AbstractMax(const BddSet &variables) const
{
    return mtbdd_abstract_max(mtbdd, variables.set.bdd);
}

Mtbdd
Mtbdd::AndExists(const Mtbdd &other, const BddSet &variables) const
{
    return mtbdd_and_abstract_plus(mtbdd, other.mtbdd, variables.set.bdd);
}

bool
Mtbdd::operator==(const Mtbdd& other) const
{
    return mtbdd == other.mtbdd;
}

bool
Mtbdd::operator!=(const Mtbdd& other) const
{
    return mtbdd != other.mtbdd;
}

Mtbdd&
Mtbdd::operator=(const Mtbdd& right)
{
    mtbdd = right.mtbdd;
    return *this;
}

Mtbdd
Mtbdd::operator!() const
{
    return mtbdd_not(mtbdd);
}

Mtbdd
Mtbdd::operator~() const
{
    return mtbdd_not(mtbdd);
}

Mtbdd
Mtbdd::operator*(const Mtbdd& other) const
{
    return mtbdd_times(mtbdd, other.mtbdd);
}

Mtbdd&
Mtbdd::operator*=(const Mtbdd& other)
{
    mtbdd = mtbdd_times(mtbdd, other.mtbdd);
    return *this;
}

Mtbdd
Mtbdd::operator+(const Mtbdd& other) const
{
    return mtbdd_plus(mtbdd, other.mtbdd);
}

Mtbdd&
Mtbdd::operator+=(const Mtbdd& other)
{
    mtbdd = mtbdd_plus(mtbdd, other.mtbdd);
    return *this;
}

Mtbdd
Mtbdd::operator-(const Mtbdd& other) const
{
    return mtbdd_minus(mtbdd, other.mtbdd);
}

Mtbdd&
Mtbdd::operator-=(const Mtbdd& other)
{
    mtbdd = mtbdd_minus(mtbdd, other.mtbdd);
    return *this;
}

Mtbdd
Mtbdd::MtbddThreshold(double value) const
{
    return mtbdd_threshold_double(mtbdd, value);
}

Mtbdd
Mtbdd::MtbddStrictThreshold(double value) const
{
    return mtbdd_strict_threshold_double(mtbdd, value);
}

Bdd
Mtbdd::BddThreshold(double value) const
{
    return mtbdd_threshold_double(mtbdd, value);
}

Bdd
Mtbdd::BddStrictThreshold(double value) const
{
    return mtbdd_strict_threshold_double(mtbdd, value);
}

Mtbdd
Mtbdd::Support() const
{
    return mtbdd_support(mtbdd);
}

MTBDD
Mtbdd::GetMTBDD() const
{
    return mtbdd;
}

Mtbdd
Mtbdd::Compose(MtbddMap &m) const
{
    return mtbdd_compose(mtbdd, m.mtbdd);
}

Mtbdd
Mtbdd::Permute(const std::vector<uint32_t>& from, const std::vector<uint32_t>& to) const
{
    /* Create a map */
    MtbddMap map;
    for (int i=from.size()-1; i>=0; i--) {
        map.put(from[i], Bdd::bddVar(to[i]));
    }

    return mtbdd_compose(mtbdd, map.mtbdd);
}

double
Mtbdd::SatCount(size_t nvars) const
{
    return mtbdd_satcount(mtbdd, nvars);
}

double
Mtbdd::SatCount(const BddSet &variables) const
{
    return SatCount(mtbdd_set_count(variables.set.bdd));
}

size_t
Mtbdd::NodeCount() const
{
    return mtbdd_nodecount(mtbdd);
}


/***
 * Implementation of class MtbddMap
 */

MtbddMap::MtbddMap(uint32_t key_variable, Mtbdd value)
{
    mtbdd = mtbdd_map_add(mtbdd_map_empty(), key_variable, value.mtbdd);
}

MtbddMap
MtbddMap::operator+(const Mtbdd& other) const
{
    return MtbddMap(mtbdd_map_update(mtbdd, other.mtbdd));
}

MtbddMap&
MtbddMap::operator+=(const Mtbdd& other)
{
    mtbdd = mtbdd_map_update(mtbdd, other.mtbdd);
    return *this;
}

MtbddMap
MtbddMap::operator-(const Mtbdd& other) const
{
    return MtbddMap(mtbdd_map_removeall(mtbdd, other.mtbdd));
}

MtbddMap&
MtbddMap::operator-=(const Mtbdd& other)
{
    mtbdd = mtbdd_map_removeall(mtbdd, other.mtbdd);
    return *this;
}

void
MtbddMap::put(uint32_t key, Mtbdd value)
{
    mtbdd = mtbdd_map_add(mtbdd, key, value.mtbdd);
}

void
MtbddMap::removeKey(uint32_t key)
{
    mtbdd = mtbdd_map_remove(mtbdd, key);
}

size_t
MtbddMap::size()
{
    return mtbdd_map_count(mtbdd);
}

bool
MtbddMap::isEmpty()
{
    return mtbdd_map_isempty(mtbdd);
}


/***
 * Implementation of class Sylvan
 */

void
Sylvan::initPackage(size_t initialTableSize, size_t maxTableSize, size_t initialCacheSize, size_t maxCacheSize)
{
    mtbdd_set_sizes(initialTableSize, maxTableSize, initialCacheSize, maxCacheSize);
    sylvan_init_package();
}

void
Sylvan::initMtbdd()
{
    sylvan_init_mtbdd();
}

void
Sylvan::quitPackage()
{
    sylvan_quit();
}
