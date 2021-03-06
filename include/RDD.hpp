#ifndef RDD_HPP_
#define RDD_HPP_

#include "RDD.h"

#include <iostream>

#include "ReduceTask.hpp"
#include "Task.hpp"
#include "TaskResult.hpp"
#include "VectorIteratorSeq.hpp"
#include "MappedRDD.hpp"
#include "FlatMappedRDD.hpp"
#include "PairRDD.hpp"
#include "Partition.hpp"
#include "SunwayMRContext.hpp"
#include "Logging.hpp"
#include "CollectTask.hpp"
#include "Pair.hpp"
#include "UnionRDD.hpp"
#include "VectorAutoPointer.hpp"
#include "StringConversion.hpp"
using namespace std;

/*
 * constructor
 */
template <class T>
RDD<T>::RDD(SunwayMRContext *c)
: context(c), sticky(false)
{
	rddID = XYZ_CURRENT_RDD_ID++;
	pthread_mutex_init(&mutex_iterator_seqs, NULL);
}

/*
 * copy constructor
 */
template <class T>
RDD<T> & RDD<T>::operator=(const RDD<T> &r) {
	if(this == &r) {
		return *this;
	}

	this->clean();
	this->initRDDFrom(r);
	return *this;
}

/*
 * initialization function for copy constructor
 */
template <class T>
void RDD<T>::initRDDFrom(const RDD<T> &r) {
	this->context = r.context;
	this->partitions = r.partitions;
	this->rddID = r.rddID;
	this->iteratorSeqs = r.iteratorSeqs;
	this->sticky = r.sticky;
}

/*
 * destructor
 */
template <class T>
RDD<T>::~RDD()
{
	this->clean();
}

/*
 * whether this RDD is sticky or not.
 * a sticky RDD will not be deleted automatically.
 */
template <class T>
bool RDD<T>::isSticky() {
	return sticky;
}

/*
 * to set this as sticky or not.
 * a sticky RDD will not be deleted automatically.
 */
template <class T>
void RDD<T>::setSticky(bool s) {
	sticky = s;
}

/*
 * add created IteratorSeq pointer.
 * this is for garbage collection.
 */
template <class T>
void RDD<T>::addIteratorSeq(IteratorSeq<T> * i) {
	pthread_mutex_lock(&mutex_iterator_seqs);
	this->iteratorSeqs.push_back(i);
	pthread_mutex_unlock(&mutex_iterator_seqs);
}

/*
 * clean operations that must done in destructor
 */
template <class T>
void RDD<T>::clean() {
	this->deletePartitions();
	this->deleteIteratorSeqs();
	pthread_mutex_destroy(&mutex_iterator_seqs);
}

/*
 * delete all partition in this RDD
 */
template <class T>
void RDD<T>::deletePartitions() {
	for(unsigned int i = 0; i < this->partitions.size(); i++) {
		delete this->partitions[i];
	}
	this->partitions.clear();
}

/*
 * delete all created IteratorSeqs in this RDD
 */
template <class T>
void RDD<T>::deleteIteratorSeqs() {
	typename vector<IteratorSeq<T> *>::iterator it;
	for(it = this->iteratorSeqs.begin(); it != this->iteratorSeqs.end(); ++it) {
		delete (*it);
	}
	this->iteratorSeqs.clear();
}

/*
 * mapping this RDD's data set into a new MappedRDD
 */
template <class T> template <class U>
MappedRDD<U, T> * RDD<T>::map(U (*f)(T&))
{
	return new MappedRDD<U, T>(this, f);
}

/*
 * flat mapping this RDD's data set into a new FlatMappedRDD
 */
template <class T> template <class U>
FlatMappedRDD<U, T> * RDD<T>::flatMap(vector<U> (*f)(T&))
{
	return new FlatMappedRDD<U, T>(this, f);
}

/*
 * virtual shuffle function
 */
template <class T>
void RDD<T>::shuffle()
{
	// do nothing
}

/*
 * mapping this RDD's data set into a new PairRDD
 */
template <class T> template <class K, class V>
PairRDD<K, V, T> * RDD<T>::mapToPair(Pair<K, V> (*f)(T&))
{
	return new PairRDD<K, V, T>(this, f);
}

/*
 * reducing this RDD's data set by a reducing function
 */
template <class T>
T RDD<T>::reduce(T (*g)(T&, T&))
{
	this->shuffle();

	// construct tasks
	vector< Task< vector<T> >* > tasks;
	vector<Partition*> pars = this->getPartitions();
	for (unsigned int i = 0; i < pars.size(); i++)
	{
		Task< vector<T> > *task = new ReduceTask<T>(this, pars[i], g);
		tasks.push_back(task);
	}
	VectorAutoPointer< Task< vector<T> > > auto_ptr1(tasks);

	// run tasks via context
	vector< TaskResult< vector<T> >* > results = this->context->runTasks(tasks);
	VectorAutoPointer< TaskResult< vector<T> > > auto_ptr2(results);

	//get results
	vector<T> values_results;
	for (unsigned int j = 0; j < results.size(); j++)
		if(results[j]->value.size() > 0) {
			values_results.push_back(results[j]->value[0]);
		}

	if (values_results.size() == 0)
	{
		//should do logging
		Logging::logWarning("RDD: reduce received empty results collection!!!");
		return 0;
	}
	//reduce left results
	VectorIteratorSeq<T> it(values_results);
	return it.reduceLeft(g)[0];
}

/*
 * inner map to pair function for distinct
 */
template <class T>
Pair< T, int > distinct_inner_map_to_pair_f (T &t) {
	int i = 0;
	return Pair< T, int >(t, i);
}

/*
 * inner reduce function for distinct
 */
template <class T>
Pair< T, int> distinct_inner_reduce_f (Pair< T, int > &p1, Pair< T, int > &p2) {
	int i = 0;
	return Pair< T, int >(p1.v1, i);
}

/*
 * inner hash function for distinct
 */
template <class T>
long distinct_inner_hash_f (Pair< T, int > &p) {
	return std::tr1::hash<string>()(to_string(p));
}

/*
 * inner to_string function for distinct
 */
template <class T>
string distinct_inner_to_string_f (Pair< T, int > &p) {
	return to_string(p);
}

/*
 * inner from_string function for distinct
 */
template <class T>
Pair< T, int > distinct_inner_from_string_f (string &s) {
	Pair< T, int > p;
	from_string(p, s);
	return p;
}

/*
 * inner map function for distinct
 */
template <class T>
T distinct_inner_map_f (Pair< T, int > &p) {
	return p.v1;
}

/*
 * distinct all the duplicate elements in this RDD.
 * accepting a new number as the number of partitions in the newly created RDD.
 */
template <class T>
MappedRDD<T, Pair< T, int > > * RDD<T>::distinct(int newNumSlices) {
	return this->mapToPair(distinct_inner_map_to_pair_f<T>)
			->reduceByKey(distinct_inner_reduce_f<T>, newNumSlices)
			->map(distinct_inner_map_f<T>);
}

/*
 * distinct all the duplicate elements in this RDD.
 * by default, to create a new RDD with the same number of partitions as this RDD.
 */
template <class T>
MappedRDD<T, Pair< T, int > > * RDD<T>::distinct() {
	return this->distinct(this->getPartitions().size());
}

/*
 * to collect all data set in this RDD.
 */
template <class T>
vector<T> RDD<T>::collect()
{
	this->shuffle();
	vector<T> ret;

	// construct tasks
	vector< Task< vector<T> >* > tasks;
	vector<Partition*> pars = this->getPartitions();
	for(unsigned int i=0; i<pars.size(); i++)
	{
		Task< vector<T> > *task = new CollectTask<T>(this, pars[i]);
		tasks.push_back(task);
	}
	VectorAutoPointer< Task< vector<T> > > auto_ptr1(tasks); // delete pointers automatically

	// run tasks via context
	vector< TaskResult< vector<T> >* > results = this->context->runTasks(tasks);
	VectorAutoPointer< TaskResult< vector<T> > > auto_ptr2(results); // delete pointers automatically

	//get results
	for(unsigned int i=0; i<results.size(); i++)
	{
		for(unsigned int j=0; j<(results[i]->value).size(); j++)
		{
			ret.push_back((results[i]->value)[j]);
		}
	}
	return ret;
}

/*
 * union this RDD with an other RDD.
 * these two RDD must the same template type RDD.
 * return a newly created UnionRDD hold all partitions from these two union RDDs.
 */
template <class T>
UnionRDD<T> * RDD<T>::unionRDD(RDD<T> *other) {
	vector< RDD<T>* > rdds;
	rdds.push_back(this);
	rdds.push_back(other);
	return new UnionRDD<T>(this->context, rdds);
}

#endif /* RDD_HPP_ */
