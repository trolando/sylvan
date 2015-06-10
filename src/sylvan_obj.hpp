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

#ifndef SYLVAN_OBJ_H
#define SYLVAN_OBJ_H

#include <string>
#include <vector>

#include <lace.h>
#include <sylvan.h>

namespace sylvan {

class Bdd;
class BddMap;

class Bdd {
    friend class Sylvan;
    friend class BddMap;

public:
    Bdd() { bdd = sylvan_false; sylvan_protect(&bdd); }
    Bdd(const BDD from) : bdd(from) { sylvan_protect(&bdd); }
    Bdd(const Bdd &from) : bdd(from.bdd) { sylvan_protect(&bdd); }
    Bdd(const uint32_t var) { bdd = sylvan_ithvar(var); sylvan_protect(&bdd); }
    ~Bdd() { sylvan_unprotect(&bdd); }

    /**
     * @brief Creates a Bdd representing just the variable index in its positive form
     * The variable index must be a 0<=index<=2^23 (we use 24 bits internally)
     */
    static Bdd bddVar(uint32_t index);

    /**
     * @brief Returns the Bdd representing "True"
     */
    static Bdd bddOne();

    /**
     * @brief Returns the Bdd representing "False"
     */
    static Bdd bddZero();

    /**
     * @brief Returns the Bdd representing a cube of variables, according to the given values.
     * @param variables the variables that will be in the cube in their positive or negative form
     * @param values a character array describing how the variables will appear in the result
     * The length of string must be equal to the number of variables in the cube.
     * For every ith char in string, if it is 0, the corresponding variable will appear in its negative form,
     * if it is 1, it will appear in its positive form, and if it is 2, it will appear as "any", thus it will
     * be skipped.
     */
    static Bdd bddCube(Bdd &variables, unsigned char *values);

    /**
     * @brief Returns the Bdd representing a cube of variables, according to the given values.
     * @param variables the variables that will be in the cube in their positive or negative form
     * @param string a character array describing how the variables will appear in the result
     * The length of string must be equal to the number of variables in the cube.
     * For every ith char in string, if it is 0, the corresponding variable will appear in its negative form,
     * if it is 1, it will appear in its positive form, and if it is 2, it will appear as "any", thus it will
     * be skipped.
     */
    static Bdd bddCube(Bdd &variables, std::vector<uint8_t> values);

    int operator==(const Bdd& other) const;
    int operator!=(const Bdd& other) const;
    Bdd operator=(const Bdd& right);
    int operator<=(const Bdd& other) const;
    int operator>=(const Bdd& other) const;
    int operator<(const Bdd& other) const;
    int operator>(const Bdd& other) const;
    Bdd operator!() const;
    Bdd operator~() const;
    Bdd operator*(const Bdd& other) const;
    Bdd operator*=(const Bdd& other);
    Bdd operator&(const Bdd& other) const;
    Bdd operator&=(const Bdd& other);
    Bdd operator+(const Bdd& other) const;
    Bdd operator+=(const Bdd& other);
    Bdd operator|(const Bdd& other) const;
    Bdd operator|=(const Bdd& other);
    Bdd operator^(const Bdd& other) const;
    Bdd operator^=(const Bdd& other);
    Bdd operator-(const Bdd& other) const;
    Bdd operator-=(const Bdd& other);

    /**
     * @brief Returns non-zero if this Bdd is bddOne() or bddZero()
     */
    int isConstant() const;

    /**
     * @brief Returns non-zero if this Bdd is bddOne() or bddZero()
     */
    int isTerminal() const;

    /**
     * @brief Returns non-zero if this Bdd is bddOne() or bddZero()
     */
    int isOne() const;

    /**
     * @brief Returns non-zero if this Bdd is bddOne() or bddZero()
     */
    int isZero() const;

    /**
     * @brief Returns the top variable index of this Bdd (the variable in the root node)
     */
    uint32_t TopVar() const;

    /**
     * @brief Follows the high edge ("then") of the root node of this Bdd
     */
    Bdd Then() const;

    /**
     * @brief Follows the low edge ("else") of the root node of this Bdd
     */
    Bdd Else() const;

    /**
     * @brief Computes \exists cube: f \and g
     */
    Bdd AndAbstract(const Bdd& g, const Bdd& cube) const;

    /**
     * @brief Computes \exists cube: f
     */
    Bdd ExistAbstract(const Bdd& cube) const;

    /**
     * @brief Computes \forall cube: f
     */
    Bdd UnivAbstract(const Bdd& cube) const;

    /**
     * @brief Computes if f then g else h
     */
    Bdd Ite(const Bdd& g, const Bdd& h) const;

    /**
     * @brief Computes f \and g
     */
    Bdd And(const Bdd& g) const;

    /**
     * @brief Computes f \or g
     */
    Bdd Or(const Bdd& g) const;

    /**
     * @brief Computes \not (f \and g)
     */
    Bdd Nand(const Bdd& g) const;

    /**
     * @brief Computes \not (f \or g)
     */
    Bdd Nor(const Bdd& g) const;

    /**
     * @brief Computes f \xor g
     */
    Bdd Xor(const Bdd& g) const;

    /**
     * @brief Computes \not (f \xor g), i.e. f \equiv g
     */
    Bdd Xnor(const Bdd& g) const;

    /**
     * @brief Returns whether all elements in f are also in g
     */
    int Leq(const Bdd& g) const;

    /**
     * @brief Computes the reverse application of a transition relation to this set.
     * @param relation the transition relation to apply
     * @param cube the variables that are in the transition relation
     * This function assumes that s,t are interleaved with s odd and t even.
     * Other variables in the relation are ignored (by existential quantification)
     * Set cube to "false" (illegal cube) to assume all encountered variables are in s,t
     *
     * Use this function to concatenate two relations   --> -->
     * or to take the 'previous' of a set               -->  S
     */
    Bdd RelPrev(const Bdd& relation, const Bdd& cube) const;

    /**
     * @brief Computes the application of a transition relation to this set.
     * @param relation the transition relation to apply
     * @param cube the variables that are in the transition relation
     * This function assumes that s,t are interleaved with s odd and t even.
     * Other variables in the relation are ignored (by existential quantification)
     * Set cube to "false" (illegal cube) to assume all encountered variables are in s,t
     *
     * Use this function to take the 'next' of a set     S  -->
     */
    Bdd RelNext(const Bdd& relation, const Bdd& cube) const;

    /**
     * @brief Computes the transitive closure by traversing the BDD recursively.
     * See Y. Matsunaga, P. C. McGeer, R. K. Brayton
     *     On Computing the Transitive Closre of a State Transition Relation
     *     30th ACM Design Automation Conference, 1993.
     */
    Bdd Closure() const;

    /**
     * @brief Computes the constrain f @ c
     */
    Bdd Constrain(Bdd &c) const;

    /**
     * @brief Computes the BDD restrict according to Coudert and Madre's algorithm (ICCAD90).
     */
    Bdd Restrict(Bdd &c) const;

    /**
     * @brief Functional composition. Whenever a variable v in the map m is found in the BDD,
     *        it is substituted by the associated function.
     * You can also use this function to implement variable reordering.
     */
    Bdd Compose(BddMap &m) const;

    /**
     * @brief Substitute all variables in the array from by the corresponding variables in to.
     */
    Bdd Permute(const std::vector<Bdd>& from, const std::vector<Bdd>& to) const;

    /**
     * @brief Computes the support of a Bdd.
     */
    Bdd Support() const;

    /**
     * @brief Gets the BDD of this Bdd (for C functions)
     */
    BDD GetBDD() const;

    /**
     * @brief Writes .dot file of this Bdd. Not thread-safe!
     */
    void PrintDot(FILE *out) const;

    /**
     * @brief Gets a SHA2 hash that describes the structure of this Bdd.
     * @param string a character array of at least 65 characters (includes zero-termination)
     * This hash is 64 characters long and is independent of the memory locations of BDD nodes.
     */
    void GetShaHash(char *string) const;

    std::string GetShaHash() const;

    /**
     * @brief Computes the number of satisfying variable assignments, using variables in cube.
     */
    double SatCount(Bdd &variables) const;

    /**
     * @brief Gets one satisfying assignment according to the variables.
     * @param variables The set of variables to be assigned, must include the support of the Bdd.
     */
    void PickOneCube(Bdd &variables, uint8_t *string) const;

    /**
     * @brief Gets one satisfying assignment according to the variables.
     * @param variables The set of variables to be assigned, must include the support of the Bdd.
     * Returns an empty vector when either this Bdd equals bddZero() or the cube is empty.
     */
    std::vector<bool> PickOneCube(Bdd &variables) const;

    /**
     * @brief Gets a cube that satisfies this Bdd.
     */
    Bdd PickOneCube() const;

    /**
     * @brief Faster version of: *this + Sylvan::bddCube(variables, values);
     */
    Bdd UnionCube(Bdd &variables, uint8_t *values) const;

    /**
     * @brief Faster version of: *this + Sylvan::bddCube(variables, values);
     */
    Bdd UnionCube(Bdd &variables, std::vector<uint8_t> values) const;

    /**
     * @brief Generate a cube representing a set of variables
     */
    static Bdd VectorCube(const std::vector<Bdd> variables);

    /**
     * @brief Generate a cube representing a set of variables
     * @param variables An sorted set of variable indices
     */
    static Bdd VariablesCube(const std::vector<uint32_t> variables);

    /**
     * @brief Gets the number of nodes in this Bdd. Not thread-safe!
     */
    size_t NodeCount() const;

private:
    BDD bdd;
};

class BddMap
{
    friend class Bdd;
    BDD bdd;
    BddMap(BDD from) : bdd(from) { sylvan_protect(&bdd); }
    BddMap(Bdd &from) : bdd(from.bdd) { sylvan_protect(&bdd); }
public:
    BddMap() : bdd(sylvan_map_empty()) { sylvan_protect(&bdd); }
    ~BddMap() { sylvan_unprotect(&bdd); }

    BddMap(uint32_t key_variable, Bdd value);

    BddMap operator+(const Bdd& other) const;
    BddMap operator+=(const Bdd& other);
    BddMap operator-(const Bdd& other) const;
    BddMap operator-=(const Bdd& other);

    /**
     * @brief Adds a key-value pair to the map
     */
    void put(uint32_t key, Bdd value);

    /**
     * @brief Removes a key-value pair from the map
     */
    void removeKey(uint32_t key);

    /**
     * @brief Returns the number of key-value pairs in this map
     */
    size_t size();

    /**
     * @brief Returns non-zero when this map is empty
     */
    int isEmpty();
};

class Sylvan {
public:
    /**
     * @brief Initializes the Sylvan framework, call this only once in your program.
     * @param initialTableSize The initial size of the nodes table. Must be a power of two.
     * @param maxTableSize The maximum size of the nodes table. Must be a power of two.
     * @param initialCacheSize The initial size of the operation cache. Must be a power of two.
     * @param maxCacheSize The maximum size of the operation cache. Must be a power of two.
     */
    static void initPackage(size_t initialTableSize, size_t maxTableSize, size_t initialCacheSize, size_t maxCacheSize);

    /**
     * @brief Initializes the BDD module of the Sylvan framework.
     * @param granularity determins operation cache behavior; for higher values (2+) it will use the operation cache less often.
     * Values of 3-7 may result in better performance, since occasionally not using the operation cache is fine in practice.
     * A granularity of 1 means that every BDD operation will be cached at every variable level.
     */
    static void initBdd(int granularity);

    /**
     * @brief Frees all memory in use by Sylvan.
     * Warning: if you have any Bdd objects which are not bddZero() or bddOne() after this, your program may crash!
     */
    static void quitPackage();
};

}

#endif
