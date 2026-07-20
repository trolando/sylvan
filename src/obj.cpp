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

namespace {

using bdd_binary_op = int (*)(BDD*, BDD, BDD);
using bdd_unary_set_op = int (*)(BDD*, BDD, BDDSET);
using bdd_binary_set_op = int (*)(BDD*, BDD, BDD, BDDSET);

BDD
apply_binary(bdd_binary_op op, BDD a, BDD b)
{
    BDD result = mtbdd_invalid;
    mtbdd_protect(&result);
    int status = op(&result, a, b);
    mtbdd_unprotect(&result);
    return status == SYLVAN_OK ? result : mtbdd_invalid;
}

BDD
apply_ite(BDD a, BDD b, BDD c)
{
    BDD result = mtbdd_invalid;
    mtbdd_protect(&result);
    int status = bdd_ite(&result, a, b, c);
    mtbdd_unprotect(&result);
    return status == SYLVAN_OK ? result : mtbdd_invalid;
}

BDD
apply_unary_set(bdd_unary_set_op op, BDD dd, BDDSET vars)
{
    BDD result = mtbdd_invalid;
    mtbdd_protect(&result);
    int status = op(&result, dd, vars);
    mtbdd_unprotect(&result);
    return status == SYLVAN_OK ? result : mtbdd_invalid;
}

BDD
apply_binary_set(bdd_binary_set_op op, BDD a, BDD b, BDDSET vars)
{
    BDD result = mtbdd_invalid;
    mtbdd_protect(&result);
    int status = op(&result, a, b, vars);
    mtbdd_unprotect(&result);
    return status == SYLVAN_OK ? result : mtbdd_invalid;
}

}

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
    return bdd_subseteq(this->bdd, other.bdd) == 1;
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
    return Bdd(apply_binary(bdd_and, bdd, other.bdd));
}

Bdd&
Bdd::operator*=(const Bdd& other)
{
    bdd = apply_binary(bdd_and, bdd, other.bdd);
    return *this;
}

Bdd
Bdd::operator&(const Bdd& other) const
{
    return Bdd(apply_binary(bdd_and, bdd, other.bdd));
}

Bdd&
Bdd::operator&=(const Bdd& other)
{
    bdd = apply_binary(bdd_and, bdd, other.bdd);
    return *this;
}

Bdd
Bdd::operator+(const Bdd& other) const
{
    return Bdd(apply_binary(bdd_or, bdd, other.bdd));
}

Bdd&
Bdd::operator+=(const Bdd& other)
{
    bdd = apply_binary(bdd_or, bdd, other.bdd);
    return *this;
}

Bdd
Bdd::operator|(const Bdd& other) const
{
    return Bdd(apply_binary(bdd_or, bdd, other.bdd));
}

Bdd&
Bdd::operator|=(const Bdd& other)
{
    bdd = apply_binary(bdd_or, bdd, other.bdd);
    return *this;
}

Bdd
Bdd::operator^(const Bdd& other) const
{
    return Bdd(apply_binary(bdd_xor, bdd, other.bdd));
}

Bdd&
Bdd::operator^=(const Bdd& other)
{
    bdd = apply_binary(bdd_xor, bdd, other.bdd);
    return *this;
}

Bdd
Bdd::operator-(const Bdd& other) const
{
    return Bdd(apply_binary(bdd_diff, bdd, other.bdd));
}

Bdd&
Bdd::operator-=(const Bdd& other)
{
    bdd = apply_binary(bdd_diff, bdd, other.bdd);
    return *this;
}

Bdd
Bdd::AndAbstract(const Bdd &g, const BddSet &cube) const
{
    return apply_binary_set(bdd_and_exists, bdd, g.bdd, cube.set.bdd);
}

Bdd
Bdd::ExistAbstract(const BddSet &cube) const
{
    return apply_unary_set(bdd_exists, bdd, cube.set.bdd);
}

Bdd
Bdd::UnivAbstract(const BddSet &cube) const
{
    return apply_unary_set(bdd_forall, bdd, cube.set.bdd);
}

Bdd
Bdd::Ite(const Bdd &g, const Bdd &h) const
{
    return apply_ite(bdd, g.bdd, h.bdd);
}

Bdd
Bdd::And(const Bdd &g) const
{
    return apply_binary(bdd_and, bdd, g.bdd);
}

Bdd
Bdd::Or(const Bdd &g) const
{
    return apply_binary(bdd_or, bdd, g.bdd);
}

Bdd
Bdd::Nand(const Bdd &g) const
{
    return apply_binary(bdd_nand, bdd, g.bdd);
}

Bdd
Bdd::Nor(const Bdd &g) const
{
    return apply_binary(bdd_nor, bdd, g.bdd);
}

Bdd
Bdd::Xor(const Bdd &g) const
{
    return apply_binary(bdd_xor, bdd, g.bdd);
}

Bdd
Bdd::Xnor(const Bdd &g) const
{
    return apply_binary(bdd_xnor, bdd, g.bdd);
}

bool
Bdd::Disjoint(const Bdd &g) const
{
    return bdd_disjoint(bdd, g.bdd) == 1;
}

bool
Bdd::Leq(const Bdd &g) const
{
    return bdd_subseteq(bdd, g.bdd) == 1;
}

Bdd
Bdd::RelPrev(const Bdd& relation, const BddSet& cube) const
{
    return bdd_rel_prev(relation.bdd, bdd, cube.set.bdd);
}

Bdd
Bdd::RelNext(const Bdd &relation, const BddSet &cube) const
{
    return bdd_rel_next(bdd, relation.bdd, cube.set.bdd);
}

Bdd
Bdd::Closure() const
{
    return bdd_transitive_closure(bdd);
}

Bdd
Bdd::Constrain(const Bdd &c) const
{
    return apply_binary(bdd_constrain, bdd, c.bdd);
}

Bdd
Bdd::Restrict(const Bdd &c) const
{
    return apply_binary(bdd_restrict, bdd, c.bdd);
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
    for (size_t i=from.size(); i>0; i--) {
        map.put(from[i-1], Bdd::bddVar(to[i-1]));
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
    mtbdd_fprint_dot(out, bdd);
}

void
Bdd::GetShaHash(char *string) const
{
    mtbdd_sha256(bdd, string);
}

std::string
Bdd::GetShaHash() const
{
    char buf[65];
    mtbdd_sha256(bdd, buf);
    return std::string(buf);
}

double
Bdd::SatCount(const BddSet &variables) const
{
    return bdd_sat_count(bdd, variables.set.bdd);
}

double
Bdd::SatCount(size_t nvars) const
{
    // Note: the mtbdd_sat_count can be called without initializing the MTBDD module.
    return mtbdd_sat_count(bdd, nvars);
}

void
Bdd::PickOneCube(const BddSet &variables, uint8_t *values) const
{
    bdd_pick_cube_values(bdd, variables.set.bdd, values);
}

std::vector<bool>
Bdd::PickOneCube(const BddSet &variables) const
{
    std::vector<bool> result = std::vector<bool>();

    BDD current = bdd;
    BDD vars = variables.set.bdd;

    if (current == bdd_false) return result;

    for (; !bdd_set_is_empty(vars); vars = bdd_set_next(vars)) {
        uint32_t var = bdd_set_first(vars);
        if (current == bdd_true) {
            // pick 0
            result.push_back(false);
        } else {
            if (mtbdd_node_variable(current) != var) {
                // pick 0
                result.push_back(false);
            } else {
                if (mtbdd_node_low(current) == bdd_false) {
                    // pick 1
                    result.push_back(true);
                    current = mtbdd_node_high(current);
                } else {
                    // pick 0
                    result.push_back(false);
                    current = mtbdd_node_low(current);
                }
            }
        }
    }

    return result;
}

Bdd
Bdd::PickOneCube() const
{
    return Bdd(bdd_pick_cube(bdd, mtbdd_support(bdd)));
}

Bdd
Bdd::UnionCube(const BddSet &variables, uint8_t *values) const
{
    return bdd_or_cube(bdd, variables.set.bdd, values);
}

Bdd
Bdd::UnionCube(const BddSet &variables, std::vector<uint8_t> values) const
{
    uint8_t *data = values.data();
    return bdd_or_cube(bdd, variables.set.bdd, data);
}

/**
 * @brief Generate a cube representing a set of variables
 */
Bdd
Bdd::VectorCube(const std::vector<Bdd> variables)
{
    Bdd result = Bdd::bddOne();
    for (size_t i=variables.size(); i>0; i--) {
        result *= variables[i-1];
    }
    return result;
}

/**
 * @brief Generate a cube representing a set of variables
 */
Bdd
Bdd::VariablesCube(std::vector<uint32_t> variables)
{
    BDD result = bdd_true;
    for (size_t i=variables.size(); i>0; i--) {
        result = mtbdd_make_node(variables[i-1], bdd_false, result);
    }
    return result;
}

size_t
Bdd::NodeCount() const
{
    return mtbdd_node_count(bdd);
}

Bdd
Bdd::bddOne()
{
    return bdd_true;
}

Bdd
Bdd::bddZero()
{
    return bdd_false;
}

Bdd
Bdd::bddVar(uint32_t index)
{
    return bdd_var_at_level(index);
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
    return bdd == bdd_true || bdd == bdd_false;
}

bool
Bdd::isTerminal() const
{
    return bdd == bdd_true || bdd == bdd_false;
}

bool
Bdd::isOne() const
{
    return bdd == bdd_true;
}

bool
Bdd::isZero() const
{
    return bdd == bdd_false;
}

uint32_t
Bdd::TopVar() const
{
    return mtbdd_node_variable(bdd);
}

Bdd
Bdd::Then() const
{
    return Bdd(mtbdd_node_high(bdd));
}

Bdd
Bdd::Else() const
{
    return Bdd(mtbdd_node_low(bdd));
}

/***
 * Implementation of class BddMap
 */

BddMap::BddMap(uint32_t key_variable, const Bdd value)
{
    bdd = mtbdd_map_set(mtbdd_map_empty(), key_variable, value.bdd);
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
    return BddMap(mtbdd_map_remove_all(bdd, other.bdd));
}

BddMap&
BddMap::operator-=(const Bdd& other)
{
    bdd = mtbdd_map_remove_all(bdd, other.bdd);
    return *this;
}

void
BddMap::put(uint32_t key, Bdd value)
{
    bdd = mtbdd_map_set(bdd, key, value.bdd);
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
    return mtbdd_map_is_empty(bdd);
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
    return mtbdd_leaf(type, value);
}

Mtbdd
Mtbdd::mtbddVar(uint32_t variable)
{
    return mtbdd_make_node(variable, mtbdd_undefined, bdd_true);
}

Mtbdd
Mtbdd::mtbddOne()
{
    return bdd_true;
}

Mtbdd
Mtbdd::mtbddZero()
{
    return mtbdd_undefined;
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
    return mtbdd_is_leaf(mtbdd);
}

bool
Mtbdd::isLeaf() const
{
    return mtbdd_is_leaf(mtbdd);
}

bool
Mtbdd::isOne() const
{
    return mtbdd == bdd_true;
}

bool
Mtbdd::isZero() const
{
    return mtbdd == mtbdd_undefined;
}

uint32_t
Mtbdd::TopVar() const
{
    return mtbdd_node_variable(mtbdd);
}

Mtbdd
Mtbdd::Then() const
{
    return !mtbdd_is_leaf(mtbdd) ? mtbdd_node_high(mtbdd) : mtbdd;
}

Mtbdd
Mtbdd::Else() const
{
    return !mtbdd_is_leaf(mtbdd) ? mtbdd_node_low(mtbdd) : mtbdd;
}

Mtbdd
Mtbdd::Negate() const
{
    return mtbdd_neg(mtbdd);
}

Mtbdd
Mtbdd::Apply(const Mtbdd &other, mtbdd_apply_cb op) const
{
    return mtbdd_apply(mtbdd, other.mtbdd, op);
}

Mtbdd
Mtbdd::UApply(mtbdd_apply_unary_cb op, size_t param) const
{
    return mtbdd_apply_unary(mtbdd, op, param);
}

Mtbdd
Mtbdd::Abstract(const BddSet &variables, mtbdd_abstract_cb op) const
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
    return mtbdd_add(mtbdd, other.mtbdd);
}

Mtbdd
Mtbdd::Times(const Mtbdd &other) const
{
    return mtbdd_mul(mtbdd, other.mtbdd);
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
    return mtbdd_abstract_add(mtbdd, variables.set.bdd);
}

Mtbdd
Mtbdd::AbstractTimes(const BddSet &variables) const
{
    return mtbdd_abstract_mul(mtbdd, variables.set.bdd);
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
    return mtbdd_mul_abstract_add(mtbdd, other.mtbdd, variables.set.bdd);
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
    return bdd_not(mtbdd);
}

Mtbdd
Mtbdd::operator~() const
{
    return bdd_not(mtbdd);
}

Mtbdd
Mtbdd::operator*(const Mtbdd& other) const
{
    return mtbdd_mul(mtbdd, other.mtbdd);
}

Mtbdd&
Mtbdd::operator*=(const Mtbdd& other)
{
    mtbdd = mtbdd_mul(mtbdd, other.mtbdd);
    return *this;
}

Mtbdd
Mtbdd::operator+(const Mtbdd& other) const
{
    return mtbdd_add(mtbdd, other.mtbdd);
}

Mtbdd&
Mtbdd::operator+=(const Mtbdd& other)
{
    mtbdd = mtbdd_add(mtbdd, other.mtbdd);
    return *this;
}

Mtbdd
Mtbdd::operator-(const Mtbdd& other) const
{
    return mtbdd_sub(mtbdd, other.mtbdd);
}

Mtbdd&
Mtbdd::operator-=(const Mtbdd& other)
{
    mtbdd = mtbdd_sub(mtbdd, other.mtbdd);
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
    for (size_t i=from.size(); i>0; i--) {
        map.put(from[i-1], Bdd::bddVar(to[i-1]));
    }

    return mtbdd_compose(mtbdd, map.mtbdd);
}

double
Mtbdd::SatCount(size_t nvars) const
{
    return mtbdd_sat_count(mtbdd, nvars);
}

double
Mtbdd::SatCount(const BddSet &variables) const
{
    return SatCount(bdd_set_count(variables.set.bdd));
}

size_t
Mtbdd::NodeCount() const
{
    return mtbdd_node_count(mtbdd);
}


/***
 * Implementation of class MtbddMap
 */

MtbddMap::MtbddMap(uint32_t key_variable, Mtbdd value)
{
    mtbdd = mtbdd_map_set(mtbdd_map_empty(), key_variable, value.mtbdd);
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
    return MtbddMap(mtbdd_map_remove_all(mtbdd, other.mtbdd));
}

MtbddMap&
MtbddMap::operator-=(const Mtbdd& other)
{
    mtbdd = mtbdd_map_remove_all(mtbdd, other.mtbdd);
    return *this;
}

void
MtbddMap::put(uint32_t key, Mtbdd value)
{
    mtbdd = mtbdd_map_set(mtbdd, key, value.mtbdd);
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
    return mtbdd_map_is_empty(mtbdd);
}


/***
 * Implementation of class Sylvan
 */

void
Sylvan::initPackage(size_t initialTableSize, size_t maxTableSize, size_t initialCacheSize, size_t maxCacheSize)
{
    sylvan_set_sizes(initialTableSize, maxTableSize, initialCacheSize, maxCacheSize);
    sylvan_init_package();
}

void
Sylvan::initMtbdd()
{
    mtbdd_init();
}

void
Sylvan::quitPackage()
{
    sylvan_quit();
}
