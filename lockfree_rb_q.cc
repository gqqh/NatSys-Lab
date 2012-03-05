/**
 * Implementation of Naive and Lock-free ring-buffer queues and
 * performance and verification tests.
 *
 * Build with (g++ version must be >= 4.5.0):
 * $ g++ -Wall -std=c++0x -O2 -lpthread lockfree_rb_q.cc
 *
 * Copyright (C) 2012 Alexander Krizhanovsky (ak@natsys-lab.com).
 *
 * This file is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 * See http://www.gnu.org/licenses/lgpl.html .
 */
#ifndef __x86_64__
#warning "Please ensure that you run on 64bit architecture."
#endif

#include <limits.h>
#include <malloc.h>
#include <string.h>
#include <unistd.h>

#include <atomic>
#include <cassert>
#include <iostream>
#include <condition_variable>
#include <mutex>
#include <thread>

#define QUEUE_SIZE	(32 * 1024)

/*
 * ------------------------------------------------------------------------
 * Naive serialized ring buffer queue
 * ------------------------------------------------------------------------
 */
template<class T, unsigned long Q_SIZE = QUEUE_SIZE>
class NaiveQueue {
private:
	static const unsigned long Q_MASK = Q_SIZE - 1;

public:
	NaiveQueue()
		: head_(0), tail_(0)
	{
		ptr_array_ = (T **)::memalign(getpagesize(),
				Q_SIZE * sizeof(void *));
		assert(ptr_array_);
	}

	void
	push(T *x)
	{
		std::unique_lock<std::mutex> lock(mtx_);

		cond_overflow_.wait(lock, [&head_, &tail_]() {
					return tail_ + Q_SIZE > head_;
				});

		ptr_array_[head_++ & Q_MASK] = x;

		cond_empty_.notify_one();
	}

	T *
	pop()
	{
		std::unique_lock<std::mutex> lock(mtx_);

		cond_empty_.wait(lock, [&head_, &tail_]() {
					return tail_ < head_;
				});

		T *x = ptr_array_[tail_++ & Q_MASK];

		cond_overflow_.notify_one();

		return x;
	}

private:
	unsigned long		head_, tail_;
	std::condition_variable	cond_empty_;
	std::condition_variable	cond_overflow_;
	std::mutex		mtx_;
	T			**ptr_array_;
};


/*
 * ------------------------------------------------------------------------
 * Lock-free N-producers M-consumers ring-buffer queue.
 * ABA problem safe.
 *
 * This implementation is bit complicated, so possibly it has sense to use
 * classic list-based queues. See:
 * 1. D.Fober, Y.Orlarey, S.Letz, "Lock-Free Techniques for Concurrent
 *    Access to Shared Ojects"
 * 2. M.M.Michael, M.L.Scott, "Simple, Fast and Practical Non-Blocking and
 *    Blocking Concurrent Queue Algorithms"
 * 3. E.Ladan-Mozes, N.Shavit, "An Optimistic Approach to Lock-Free FIFO Queues"
 *
 * See also implementation of N-producers M-consumers FIFO and
 * 1-producer 1-consumer ring-buffer from Tim Blechmann:
 *	http://tim.klingt.org/boost_lockfree/
 *	git://tim.klingt.org/boost_lockfree.git
 * ------------------------------------------------------------------------
 */
// thread_local is still not implemented in GCC.
static size_t __thread __thr_id;

/**
 * @return continous thread IDs starting from 0 as opposed to pthread_self().
 */
inline size_t
thr_id()
{
	return __thr_id;
}

inline void
set_thr_id(size_t id)
{
	__thr_id = id;
}

template<class T,
	decltype(thr_id) ThrId = thr_id,
	unsigned long Q_SIZE = QUEUE_SIZE>
class LockFreeQueue {
private:
	static const unsigned long Q_MASK = Q_SIZE - 1;

	struct ThrPos {
		volatile unsigned long head, tail;
	};

public:
	LockFreeQueue(size_t n_producers, size_t n_consumers)
		: n_producers_(n_producers),
		n_consumers_(n_consumers),
		head_(0),
		tail_(0),
		last_head_(0),
		last_tail_(0)
	{
		auto n = std::max(n_consumers_, n_producers_);
		thr_p_ = (ThrPos *)::memalign(getpagesize(), sizeof(ThrPos) * n);
		assert(thr_p_);
		// Set per thread tail and head to ULONG_MAX.
		::memset((void *)thr_p_, 0xFF, sizeof(ThrPos) * n);

		ptr_array_ = (T **)::memalign(getpagesize(),
				Q_SIZE * sizeof(void *));
		assert(ptr_array_);
	}

	~LockFreeQueue()
	{
		::free(ptr_array_);
		::free(thr_p_);
	}

	ThrPos&
	thr_pos() const
	{
		assert(ThrId() < std::max(n_consumers_, n_producers_));
		return thr_p_[ThrId()];
	}

	void
	push(T *ptr)
	{
		/*
		 * Request next place to push.
		 *
		 * Second assignemnt is atomic only for head shift, so there is
		 * a time window in which thr_p_[tid].head = ULONG_MAX, and
		 * head could be shifted significantly by other threads,
		 * so pop() will set last_head_ to head.
		 * After that thr_p_[tid].head is setted to old head value
		 * (which is stored in local CPU register) and written by @ptr.
		 *
		 * First assignment guaranties that pop() sees values for
		 * head and thr_p_[tid].head not greater that they will be
		 * after the second assignment with head shift.
		 */
		thr_pos().head = head_;
		__sync_synchronize();
		thr_pos().head = __sync_fetch_and_add(&head_, 1);

		/*
		 * We do not know when a consumer uses the pop()'ed pointer,
		 * se we can not overwrite it and have to wait the lowest tail.
		 */
		while (__builtin_expect(thr_pos().head >= last_tail_ + Q_SIZE, 0))
		{
			::sched_yield();

			auto min = tail_;
			// Update the last_tail_.
			for (size_t i = 0; i < n_consumers_; ++i) {
				auto tmp_t = thr_p_[i].tail;
				if (tmp_t < min)
					min = tmp_t;
			}
			last_tail_ = min;
		}

		ptr_array_[thr_pos().head & Q_MASK] = ptr;
		// Allow consumers eat the item.
		thr_pos().head = ULONG_MAX;
	}

	T *
	pop()
	{
		/*
		 * Request next place from which to pop.
		 * See comments for push().
		 */
		thr_pos().tail = tail_;
		__sync_synchronize();
		thr_pos().tail = __sync_fetch_and_add(&tail_, 1);

		/*
		 * tid'th place in ptr_array_ is reserved by the thread -
		 * this place shall never be rewritten by push() and
		 * last_tail_ at push() is a guarantee.
		 * last_head_ guaraties that no any consumer eats the item
		 * before producer reserved the position writes to it.
		 */
		while (__builtin_expect(thr_pos().tail >= last_head_, 0))
		{
			::sched_yield();

			auto min = head_;
			// Update the last_head_.
			for (size_t i = 0; i < n_producers_; ++i) {
				auto tmp_h = thr_p_[i].head;
				if (tmp_h < min)
					min = tmp_h;
			}
			last_head_ = min;
		}

		T *ret = ptr_array_[thr_pos().tail & Q_MASK];
		// Allow producers rewrite the slot.
		thr_pos().tail = ULONG_MAX;
		return ret;
	}

private:
	const size_t n_producers_, n_consumers_;
	// currently free position (next to insert)
	volatile unsigned long head_;
	// current tail, next to pop
	volatile unsigned long tail_;
	// last not-processed producer's pointer
	volatile unsigned long	last_head_;
	// last not-processed consumer's pointer
	volatile unsigned long	last_tail_;
	ThrPos	*thr_p_;
	T	**ptr_array_;
};


/*
 * ------------------------------------------------------------------------
 *	Tests for naive and lock-free queues
 * ------------------------------------------------------------------------
 */
static const auto N = QUEUE_SIZE * 1024;
static const auto CONSUMERS = 16;
static const auto PRODUCERS = 16;

typedef unsigned char	q_type;

static const q_type X_EMPTY = 0; // the address skipped by producers
static const q_type X_MISSED = 255; // the address skipped by consumers
q_type x[N * PRODUCERS];
std::atomic<int> n(0);

template<class Q>
struct Worker {
	Worker(Q *q, size_t id = 0)
		: q_(q),
		thr_id_(id)
	{}

	Q *q_;
	size_t thr_id_;
};

template<class Q>
struct Producer : public Worker<Q> {
	Producer(Q *q, size_t id)
		: Worker<Q>(q, id)
	{}

	void operator()()
	{
		set_thr_id(Worker<Q>::thr_id_);

		for (auto i = thr_id(); i < N * PRODUCERS; i += PRODUCERS) {
			x[i] = X_MISSED;
			Worker<Q>::q_->push(x + i);
		}
	}
};

template<class Q>
struct Consumer : public Worker<Q> {
	Consumer(Q *q, size_t id)
		: Worker<Q>(q, id)
	{}

	void operator()()
	{
		set_thr_id(Worker<Q>::thr_id_);

		while (n.fetch_add(1) < N * PRODUCERS) {
			q_type *v = Worker<Q>::q_->pop();
			assert(v);
			assert(*v == X_MISSED);
			*v = (q_type)(thr_id() + 1); // don't write zero
		}
	}
};

template<class Q>
void
run_test(Q &&q)
{
	std::thread thr[PRODUCERS + CONSUMERS];

	n.store(0);
	::memset(x, X_EMPTY, N * sizeof(q_type) * PRODUCERS);

	// Run producers.
	for (auto i = 0; i < PRODUCERS; ++i)
		thr[i] = std::thread(Producer<Q>(&q, i));

	::usleep(10 * 1000); // sleep to wait the queue is full

	/*
	 * Run consumers.
	 * Create consumers with the same thread IDs as producers.
	 * The IDs are used for queue head and tail indexing only,
	 * so we  care only about different IDs for threads of the same type.
	 */
	for (auto i = 0; i < CONSUMERS; ++i)
		thr[PRODUCERS + i] = std::thread(Consumer<Q>(&q, i));

	// Wait for all threads completion.
	for (auto i = 0; i < PRODUCERS + CONSUMERS; ++i)
		thr[i].join();

	// Check data.
	auto res = 0;
	std::cout << "check X data..." << std::endl;
	for (auto i = 0; i < N * PRODUCERS; ++i) {
		if (x[i] == X_EMPTY) {
			std::cout << "empty " << i << std::endl;
			res = 1;
			break;
		} else if (x[i] == X_MISSED) {
			std::cout << "missed " << i << std::endl;
			res = 2;
			break;
		}
	}
	std::cout << (res ? "FAILED" : "Passed") << std::endl;
}

int
main()
{
	LockFreeQueue<q_type> lf_q(PRODUCERS, CONSUMERS);
	run_test<LockFreeQueue<q_type>>(std::move(lf_q));

	NaiveQueue<q_type> n_q;
	run_test<NaiveQueue<q_type>>(std::move(n_q));

	return 0;
}
