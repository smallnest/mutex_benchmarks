#include <mutex>
#include <thread>
#include <list>

#include "benchmark/benchmark.h"
#include <algorithm>
#include <chrono>
#include <array>
#include <condition_variable>
#include <shared_mutex>
#include <atomic>
#include <cstring>
#include <emmintrin.h>
#include <numeric>
#include <random>
#include <deque>
#ifdef _WIN32
#include <intrin.h>
#pragma intrinsic(_umul128)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN 1
#include <Windows.h>
#else
#include <semaphore.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <linux/futex.h>
#include <pthread.h>
#endif

/*
 * This benchmark is disturbed by realtime trottling (see https://lwn.net/Articles/296419/)
 * I have found that CPU affinity can be used instead!
 */
int set_realtime_priority(pid_t pid, int policy, int priority)
{
        struct sched_param schp;
        memset(&schp, 0, sizeof(schp));
        schp.sched_priority = priority;

        /*
        * set the process to realtime privs
        */

        printf("Attempt to set realtime for pid %d policy: %d prio: %d ", pid, policy, priority);

        // do not set SCHED_RESET_ON_FORK bit as we have no control over what threads are used in benchmark
        if (sched_setscheduler(pid, policy, &schp) == 0) {
            printf("- done!\n");
            return 0;
        }
        printf("- failed!\n");
        perror("sched_setscheduler");


        return -1;
}

unsigned int count_enabled_cpus() {
        // how many cores are enabled for us?
        unsigned int cpus = std::thread::hardware_concurrency();
        size_t cpusetsize = CPU_ALLOC_SIZE(cpus);
        cpu_set_t *mask = CPU_ALLOC(cpusetsize);

        CPU_ZERO_S(cpusetsize, mask);
        if (sched_getaffinity(0, cpusetsize, mask)) {
            perror("sched_getaffinity");
        }


        int count = CPU_COUNT_S(cpusetsize, mask);
        CPU_FREE(mask);
        return count;
}

unsigned int no_of_enabled_cpus = count_enabled_cpus();


// todo: try WTF lock (aka parking lot)

template<typename T>
inline void futex_wait(std::atomic<T>& to_wait_on, T expected)
{
#ifdef _WIN32
	WaitOnAddress(&to_wait_on, &expected, sizeof(expected), INFINITE);
#else
	syscall(SYS_futex, &to_wait_on, FUTEX_WAIT_PRIVATE, expected, nullptr, nullptr, 0);
#endif
}
template<typename T>
inline void futex_wake_single(std::atomic<T>& to_wake)
{
#ifdef _WIN32
	WakeByAddressSingle(&to_wake);
#else
	syscall(SYS_futex, &to_wake, FUTEX_WAKE_PRIVATE, 1, nullptr, nullptr, 0);
#endif
}
template<typename T>
inline void futex_wake_all(std::atomic<T>& to_wake)
{
#ifdef _WIN32
	WakeByAddressAll(&to_wake);
#else
	syscall(SYS_futex, &to_wake, FUTEX_WAKE_PRIVATE, std::numeric_limits<int>::max(), nullptr, nullptr, 0);
#endif
}


struct ticket_mutex
{
	void lock()
	{
		unsigned my = in.fetch_add(1);
		for (;;)
		{
			unsigned now = out;
			if (my == now)
				break;
			futex_wait(out, now);
		}
	}
	void unlock()
	{
		unsigned new_value = out.fetch_add(1) + 1;
		if (new_value != in)
			futex_wake_all(out);
	}

private:
	std::atomic<uint32_t> in{ 0 };
	std::atomic<uint32_t> out{ 0 };
};

struct ticket_spinlock
{
	void lock()
	{
		unsigned my = in.fetch_add(1, std::memory_order_relaxed);
		for (int spin_count = 0; out.load(std::memory_order_acquire) != my; ++spin_count)
		{
			if (spin_count < 16)
				_mm_pause();
			else
			{
				std::this_thread::yield();
				spin_count = 0;
			}
		}
	}
	void unlock()
	{
		out.store(out.load(std::memory_order_relaxed) + 1, std::memory_order_release);
	}

private:
	std::atomic<unsigned> in{ 0 };
	std::atomic<unsigned> out{ 0 };
};

#ifdef _WIN32
struct critical_section
{
	critical_section()
	{
		InitializeCriticalSection(&cs);
	}
	~critical_section()
	{
		DeleteCriticalSection(&cs);
	}
	void lock()
	{
		EnterCriticalSection(&cs);
	}
	void unlock()
	{
		LeaveCriticalSection(&cs);
	}

private:
	CRITICAL_SECTION cs;
};

struct critical_section_spin
{
	critical_section_spin()
	{
		InitializeCriticalSectionAndSpinCount(&cs, 4000);
	}
	~critical_section_spin()
	{
		DeleteCriticalSection(&cs);
	}
	void lock()
	{
		EnterCriticalSection(&cs);
	}
	void unlock()
	{
		LeaveCriticalSection(&cs);
	}

private:
	CRITICAL_SECTION cs;
};

struct srw_lock
{
	void lock()
	{
		AcquireSRWLockExclusive(&l);
	}
	void unlock()
	{
		ReleaseSRWLockExclusive(&l);
	}

private:
	SRWLOCK l = SRWLOCK_INIT;
};
#else
struct pthread_mutex
{
	~pthread_mutex()
	{
		pthread_mutex_destroy(&mutex);
	}

	void lock()
	{
		pthread_mutex_lock(&mutex);
	}
	void unlock()
	{
		pthread_mutex_unlock(&mutex);
	}

private:
	pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
};
struct pthread_mutex_recursive
{
	~pthread_mutex_recursive()
	{
		pthread_mutex_destroy(&mutex);
	}

	void lock()
	{
		pthread_mutex_lock(&mutex);
	}
	void unlock()
	{
		pthread_mutex_unlock(&mutex);
	}

private:
	pthread_mutex_t mutex = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
};
struct pthread_mutex_adaptive
{
	~pthread_mutex_adaptive()
	{
		pthread_mutex_destroy(&mutex);
	}

	void lock()
	{
		pthread_mutex_lock(&mutex);
	}
	void unlock()
	{
		pthread_mutex_unlock(&mutex);
	}

private:
	pthread_mutex_t mutex = PTHREAD_ADAPTIVE_MUTEX_INITIALIZER_NP;
};
#endif

struct futex_mutex
{
	void lock()
	{
		if (state.exchange(locked, std::memory_order_acquire) == unlocked)
			return;
		while (state.exchange(sleeper, std::memory_order_acquire) != unlocked)
		{
			futex_wait(state, sleeper);
		}
	}
	void unlock()
	{
		if (state.exchange(unlocked, std::memory_order_release) == sleeper)
			futex_wake_single(state);
	}

private:
	std::atomic<uint32_t> state{ unlocked };

	static constexpr uint32_t unlocked = 0;
	static constexpr uint32_t locked = 1;
	static constexpr uint32_t sleeper = 2;
};

struct spin_to_futex_mutex
{
	void lock()
	{
		unsigned old_state = unlocked;
		if (state.compare_exchange_strong(old_state, locked, std::memory_order_acquire))
			return;
		for (;;)
		{
			old_state = state.exchange(sleeper, std::memory_order_acquire);
			if (old_state == unlocked)
				return;
			else if (old_state == locked)
			{
				for (;;)
				{
					_mm_pause();
					if (state.load(std::memory_order_acquire) == unlocked)
						break;
					std::this_thread::yield();
					if (state.load(std::memory_order_acquire) == unlocked)
						break;
				}
			}
			else
				futex_wait(state, sleeper);
		}
	}
	void unlock()
	{
		unsigned old_state = state.load(std::memory_order_relaxed);
		state.store(unlocked, std::memory_order_release);
		if (old_state == sleeper)
			futex_wake_single(state);
	}

private:
	std::atomic<unsigned> state{ unlocked };
	static constexpr unsigned unlocked = 0;
	static constexpr unsigned locked = 1;
	static constexpr unsigned sleeper = 2;
};

#ifdef _WIN32

struct semaphore
{
	semaphore()
		: semaphore(0)
	{
	}
	explicit semaphore(LONG initial_amount)
		: windows_semaphore(CreateSemaphoreA(nullptr, initial_amount, std::numeric_limits<LONG>::max(), nullptr))
	{
	}
	~semaphore()
	{
		CloseHandle(windows_semaphore);
	}
	void acquire()
	{
		WaitForSingleObject(windows_semaphore, INFINITE);
	}
	void release(ptrdiff_t update = 1)
	{
		ReleaseSemaphore(windows_semaphore, static_cast<LONG>(update), nullptr);
	}

private:
	HANDLE windows_semaphore{ 0 };
};

#else

struct semaphore
{
	semaphore()
		: semaphore(0)
	{
	}
	explicit semaphore(std::ptrdiff_t value)
	{
		sem_init(&sem, false, value);
	}
	~semaphore()
	{
		sem_destroy(&sem);
	}
	void release()
	{
		int result = sem_post(&sem);
		assert(!result); static_cast<void>(result);
	}
	void release(ptrdiff_t update)
	{
		for (; update > 0; --update)
		{
			release();
		}
	}

	void acquire()
	{
		while (sem_wait(&sem))
		{
			assert(errno == EINTR);
		}
	}

private:
	sem_t sem;
};
#endif

struct semaphore_custom
{
private:
	std::mutex mutex;
	std::condition_variable condition;
	std::ptrdiff_t count = 0;

public:
	explicit semaphore_custom(ptrdiff_t desired = 0)
		: count(desired)
	{
	}

	void release()
	{
		{
			std::lock_guard<std::mutex> lock(mutex);
			++count;
		}
		condition.notify_one();
	}
	void release(ptrdiff_t update)
	{
		{
			std::lock_guard<std::mutex> lock(mutex);
			count += update;
		}
		condition.notify_all();
	}

	void acquire()
	{
		std::unique_lock<std::mutex> lock(mutex);
		while (!count)
			condition.wait(lock);
		--count;
	}
};

struct semaphore_mutex
{
	void lock()
	{
		sema.acquire();
	}
	void unlock()
	{
		sema.release();
	}
private:
	semaphore sema{ 1 };
};

struct terrible_spinlock
{
	void lock()
	{
		while (locked.test_and_set(std::memory_order_acquire))
		{
		}
	}
	void unlock()
	{
		locked.clear(std::memory_order_release);
	}

private:
	std::atomic_flag locked = ATOMIC_FLAG_INIT;
};
struct spinlock_test_and_set
{
	void lock()
	{
		for (;;)
		{
			if (try_lock())
				break;
			_mm_pause();
			if (try_lock())
				break;
			std::this_thread::yield();
		}
	}
	bool try_lock()
	{
		return !locked.test_and_set(std::memory_order_acquire);
	}
	void unlock()
	{
		locked.clear(std::memory_order_release);
	}

private:
	std::atomic_flag locked = ATOMIC_FLAG_INIT;
};
struct spinlock_test_and_set_once
{
	void lock()
	{
		for (;;)
		{
			if (!locked.exchange(true, std::memory_order_acquire))
				break;
			for (;;)
			{
				_mm_pause();
				if (!locked.load(std::memory_order_acquire))
					break;
				std::this_thread::yield();
				if (!locked.load(std::memory_order_acquire))
					break;
			}
		}
	}
	bool try_lock()
	{
		return !locked.load(std::memory_order_acquire) && !locked.exchange(true, std::memory_order_acquire);
	}
	void unlock()
	{
		locked.store(false, std::memory_order_release);
	}

private:
	std::atomic<bool> locked{ false };
};
struct spinlock_compare_exchange
{
	void lock()
	{
		for (;;)
		{
			bool unlocked = false;
			if (locked.compare_exchange_weak(unlocked, true, std::memory_order_acquire))
				break;
			for (;;)
			{
				_mm_pause();
				if (!locked.load(std::memory_order_acquire))
					break;
				std::this_thread::yield();
				if (!locked.load(std::memory_order_acquire))
					break;
			}
		}
	}
	bool try_lock()
	{
		return !locked.load(std::memory_order_acquire) && !locked.exchange(true, std::memory_order_acquire);
	}
	void unlock()
	{
		locked.store(false, std::memory_order_release);
	}

private:
	std::atomic<bool> locked{ false };
};
struct spinlock_compare_exchange_only
{
	void lock()
	{
		for (;;)
		{
			if (try_lock())
				break;
			_mm_pause();
			if (try_lock())
				break;
			std::this_thread::yield();
		}
	}
	bool try_lock()
	{
		bool unlocked = false;
		return locked.compare_exchange_weak(unlocked, true, std::memory_order_acquire);
	}
	void unlock()
	{
		locked.store(false, std::memory_order_release);
	}

private:
	std::atomic<bool> locked{ false };
};

struct spinlock_amd
{
	void lock()
	{
		for (;;)
		{
			bool was_locked = locked.load(std::memory_order_relaxed);
			if (!was_locked && locked.compare_exchange_weak(was_locked, true, std::memory_order_acquire))
				break;
			_mm_pause();
		}
	}
	void unlock()
	{
		locked.store(false, std::memory_order_release);
	}

private:
	std::atomic<bool> locked{ false };
};

struct spinlock
{
	void lock()
	{
		for (int spin_count = 0; !try_lock(); ++spin_count)
		{
			if (spin_count < 16)
				_mm_pause();
			else
			{
				std::this_thread::yield();
				spin_count = 0;
			}
		}
	}
	bool try_lock()
	{
		return !locked.load(std::memory_order_relaxed) && !locked.exchange(true, std::memory_order_acquire);
	}
	void unlock()
	{
		locked.store(false, std::memory_order_release);
	}

private:
	std::atomic<bool> locked{ false };
};

template<int SpinNoOpCount, int SpinPauseCount, bool ResetCountAfterYield>
struct parameterized_spinlock
{
	void lock()
	{
		int spin_count = 0;
		auto on_spin = [&]
		{
			if constexpr (SpinNoOpCount > 0)
			{
				if (SpinNoOpCount == std::numeric_limits<int>::max() || spin_count < SpinNoOpCount)
					return;
			}
			if constexpr (SpinNoOpCount != std::numeric_limits<int>::max() && SpinPauseCount > 0)
			{
				if (SpinPauseCount == std::numeric_limits<int>::max() || spin_count < SpinPauseCount)
				{
					_mm_pause();
					return;
				}
			}
			if constexpr (SpinNoOpCount != std::numeric_limits<int>::max() && SpinPauseCount != std::numeric_limits<int>::max())
			{
				std::this_thread::yield();
				if constexpr (ResetCountAfterYield)
					spin_count = 0;
			}
		};
		for (;;)
		{
			bool expected = false;
			if (locked.compare_exchange_weak(expected, true, std::memory_order_acquire))
				return;
			for (;; ++spin_count)
			{
				on_spin();
				if (!locked.load(std::memory_order_acquire))
					break;
			}
		}
	}
	bool try_lock()
	{
		bool expected = false;
		return locked.compare_exchange_strong(expected, true, std::memory_order_acquire);
	}
	void unlock()
	{
		locked.store(false, std::memory_order_release);
	}

private:
	std::atomic<bool> locked{ false };
};

template<typename T>
void benchmark_mutex_lock_unlock(benchmark::State& state)
{
	T m;
	while (state.KeepRunning())
	{
		m.lock();
		m.unlock();
	}
}

#ifdef _WIN32
#define RegisterBenchmarkWithWindowsMutexes(benchmark, ...)\
    BENCHMARK_TEMPLATE(benchmark, critical_section) __VA_ARGS__;\
    BENCHMARK_TEMPLATE(benchmark, critical_section_spin) __VA_ARGS__;\
    BENCHMARK_TEMPLATE(benchmark, srw_lock) __VA_ARGS__;
#define RegisterBenchmarkWithPthreadMutexes(benchmark, ...)
#else
#define RegisterBenchmarkWithWindowsMutexes(benchmark, ...)
#define RegisterBenchmarkWithPthreadMutexes(benchmark, ...)\
    BENCHMARK_TEMPLATE(benchmark, pthread_mutex) __VA_ARGS__;\
    BENCHMARK_TEMPLATE(benchmark, pthread_mutex_recursive) __VA_ARGS__;\
    BENCHMARK_TEMPLATE(benchmark, pthread_mutex_adaptive) __VA_ARGS__;
#endif

#define RegisterBenchmarkWithAllMutexes(benchmark, ...)\
    BENCHMARK_TEMPLATE(benchmark, futex_mutex) __VA_ARGS__;\
    BENCHMARK_TEMPLATE(benchmark, spinlock_amd) __VA_ARGS__;\
/*#define RegisterBenchmarkWithAllMutexes(benchmark, ...)\
    BENCHMARK_TEMPLATE(benchmark, std::mutex) __VA_ARGS__;\
    BENCHMARK_TEMPLATE(benchmark, std::shared_mutex) __VA_ARGS__;\
    BENCHMARK_TEMPLATE(benchmark, std::recursive_mutex) __VA_ARGS__;\
    RegisterBenchmarkWithWindowsMutexes(benchmark, __VA_ARGS__);\
    RegisterBenchmarkWithPthreadMutexes(benchmark, __VA_ARGS__);\
    BENCHMARK_TEMPLATE(benchmark, futex_mutex) __VA_ARGS__;\
    BENCHMARK_TEMPLATE(benchmark, spin_to_futex_mutex) __VA_ARGS__;\
    BENCHMARK_TEMPLATE(benchmark, ticket_spinlock) __VA_ARGS__;\
    BENCHMARK_TEMPLATE(benchmark, ticket_mutex) __VA_ARGS__;\
    BENCHMARK_TEMPLATE(benchmark, semaphore_mutex) __VA_ARGS__;\
    BENCHMARK_TEMPLATE(benchmark, spinlock) __VA_ARGS__;\
    BENCHMARK_TEMPLATE(benchmark, spinlock_amd) __VA_ARGS__;\
    BENCHMARK_TEMPLATE(benchmark, spinlock_test_and_set) __VA_ARGS__;\
    BENCHMARK_TEMPLATE(benchmark, spinlock_test_and_set_once) __VA_ARGS__;\
    BENCHMARK_TEMPLATE(benchmark, spinlock_compare_exchange) __VA_ARGS__;\
    BENCHMARK_TEMPLATE(benchmark, spinlock_compare_exchange_only) __VA_ARGS__;\
    BENCHMARK_TEMPLATE(benchmark, terrible_spinlock) __VA_ARGS__;\
    /*BENCHMARK_TEMPLATE(benchmark, parameterized_spinlock<0, 0>) __VA_ARGS__;\
    BENCHMARK_TEMPLATE(benchmark, parameterized_spinlock<0, 1>) __VA_ARGS__;\
    BENCHMARK_TEMPLATE(benchmark, parameterized_spinlock<0, 4>) __VA_ARGS__;\
    BENCHMARK_TEMPLATE(benchmark, parameterized_spinlock<0, 16>) __VA_ARGS__;\
    BENCHMARK_TEMPLATE(benchmark, parameterized_spinlock<0, 64>) __VA_ARGS__;\
    BENCHMARK_TEMPLATE(benchmark, parameterized_spinlock<0, 256>) __VA_ARGS__;\
    BENCHMARK_TEMPLATE(benchmark, parameterized_spinlock<1, 1>) __VA_ARGS__;\
    BENCHMARK_TEMPLATE(benchmark, parameterized_spinlock<1, 4>) __VA_ARGS__;\
    BENCHMARK_TEMPLATE(benchmark, parameterized_spinlock<1, 16>) __VA_ARGS__;\
    BENCHMARK_TEMPLATE(benchmark, parameterized_spinlock<1, 64>) __VA_ARGS__;\
    BENCHMARK_TEMPLATE(benchmark, parameterized_spinlock<1, 256>) __VA_ARGS__;\
    BENCHMARK_TEMPLATE(benchmark, parameterized_spinlock<4, 4>) __VA_ARGS__;\
    BENCHMARK_TEMPLATE(benchmark, parameterized_spinlock<4, 16>) __VA_ARGS__;\
    BENCHMARK_TEMPLATE(benchmark, parameterized_spinlock<4, 64>) __VA_ARGS__;\
    BENCHMARK_TEMPLATE(benchmark, parameterized_spinlock<4, 256>) __VA_ARGS__;\
    BENCHMARK_TEMPLATE(benchmark, parameterized_spinlock<16, 16>) __VA_ARGS__;\
    BENCHMARK_TEMPLATE(benchmark, parameterized_spinlock<16, 64>) __VA_ARGS__;\
    BENCHMARK_TEMPLATE(benchmark, parameterized_spinlock<16, 256>) __VA_ARGS__;\
    BENCHMARK_TEMPLATE(benchmark, parameterized_spinlock<64, 64>) __VA_ARGS__;\
    BENCHMARK_TEMPLATE(benchmark, parameterized_spinlock<64, 256>) __VA_ARGS__;*/\

RegisterBenchmarkWithAllMutexes(benchmark_mutex_lock_unlock);

void benchmark_shared_mutex_lock_shared(benchmark::State& state)
{
	std::shared_mutex m;
	while (state.KeepRunning())
	{
		m.lock_shared();
		m.unlock_shared();
	}
}
BENCHMARK(benchmark_shared_mutex_lock_shared);

void benchmark_semaphore_release_and_acquire(benchmark::State& state)
{
	semaphore m;
	while (state.KeepRunning())
	{
		m.release();
		m.acquire();
	}
}
BENCHMARK(benchmark_semaphore_release_and_acquire);

void benchmark_custom_semaphore_release_and_acquire(benchmark::State& state)
{
	semaphore_custom m;
	while (state.KeepRunning())
	{
		m.release();
		m.acquire();
	}
}
BENCHMARK(benchmark_custom_semaphore_release_and_acquire);


template<typename State>
struct ThreadedBenchmarkRunner
{
	ThreadedBenchmarkRunner(State* state, int num_threads)
		: state(state)
	{
		threads.reserve(num_threads);
		for (int i = 0; i < num_threads; ++i)
		{
			threads.emplace_back([this, state, i]
			{
				finished.release();
				for (;;)
				{
					all_started_sema.acquire();
					if (ended)
						break;
					state->run_benchmark(i);
					finished.release();
				}
			});
		}
		for (size_t i = threads.size(); i > 0; --i)
			finished.acquire();
	}

	void wait_until_started()
	{
		state->start_run();
	}
	void release()
	{
		all_started_sema.release(threads.size());
	}
	void wait_until_finished()
	{
		for (size_t i = threads.size(); i > 0; --i)
			finished.acquire();
		state->end_run();
	}

	void full_run()
	{
		wait_until_started();
		release();
		wait_until_finished();
	}

	void shut_down()
	{
		ended = true;
		all_started_sema.release(threads.size());
		for (std::thread& thread : threads)
			thread.join();
	}

private:
	State* state;
	std::vector<std::thread> threads;
	semaphore all_started_sema;
	semaphore finished;
	bool ended = false;
};

template<typename State>
struct ThreadedBenchmarkRunnerMultipleTimes
{
	template<typename... StateArgs>
	ThreadedBenchmarkRunnerMultipleTimes(int num_runners, int num_threads, StateArgs&&... state_args)
	{
		for (int i = 0; i < num_runners; ++i)
		{
			runners.emplace_back(num_threads, state_args...);
		}
	}

	void wait_until_started()
	{
		for (OneRunnerAndState& runner : runners)
			runner.runner.wait_until_started();
	}

	void start_run()
	{
		for (OneRunnerAndState& runner : runners)
			runner.runner.release();
	}

	void finish_run()
	{
		for (OneRunnerAndState& runner : runners)
			runner.runner.wait_until_finished();
	}

	void full_run()
	{
		wait_until_started();
		start_run();
		finish_run();
	}

	void shut_down()
	{
		for (OneRunnerAndState& runner : runners)
			runner.runner.shut_down();
	}

	struct OneRunnerAndState
	{
		State state;
		ThreadedBenchmarkRunner<State> runner;

		template<typename... StateArgs>
		OneRunnerAndState(int num_threads, StateArgs&&... state_args)
			: state(num_threads, std::forward<StateArgs>(state_args)...), runner(&state, num_threads)
		{
		}
	};
	std::list<OneRunnerAndState> runners;
};

template<typename T>
struct ContendedMutexRunner
{
	size_t num_loops = 1;
	size_t sum = 0;
	size_t expected_sum;
	T mutex;

	explicit ContendedMutexRunner(int num_threads, size_t num_loops)
		: num_loops(num_loops)
		, expected_sum(num_threads* num_loops)
	{
	}

	void start_run()
	{
		sum = 0;
	}
	void end_run()
	{
		assert(sum == expected_sum);
	}

	void run_benchmark(int)
	{
		for (size_t i = num_loops; i != 0; --i)
		{
			mutex.lock();
			++sum;
			mutex.unlock();
		}
	}
};
template<typename T>
struct ContendedMutexRunnerSized
{
	size_t num_loops = 1;
	std::vector<size_t> sums;
	size_t expected_sum;
	T mutex;

	explicit ContendedMutexRunnerSized(int num_threads, size_t num_loops, size_t num_sums)
		: num_loops(num_loops)
		, sums(num_sums)
		, expected_sum(num_threads* num_loops)
	{
	}

	void start_run()
	{
		for (size_t& sum : sums)
			sum = 0;
	}
	void end_run()
	{
		assert(sums.front() == expected_sum);
	}

	void run_benchmark(int)
	{
		for (size_t i = num_loops; i != 0; --i)
		{
			mutex.lock();
			for (size_t& sum : sums)
				++sum;
			mutex.unlock();
		}
	}
};

#ifdef _WIN32
template<typename Randomness>
uint64_t UniformRandom(uint64_t max, Randomness& random)
{
	static_assert(Randomness::min() == 0 && Randomness::max() == std::numeric_limits<uint64_t>::max());
	uint64_t random_value = random();
	if (max == std::numeric_limits<uint64_t>::max())
		return random_value;
	++max;
	uint64_t high_bits;
	uint64_t lower_bits = _umul128(random_value, max, &high_bits);
	if (lower_bits < max)
	{
		uint64_t threshold = -max % max;
		while (lower_bits < threshold)
		{
			random_value = random();
			uint64_t lower_bits = _umul128(random_value, max, &high_bits);
		}
	}
	return high_bits;
}
#else
template<typename Randomness>
uint64_t UniformRandom(uint64_t max, Randomness& random)
{
	static_assert(Randomness::min() == 0 && Randomness::max() == std::numeric_limits<uint64_t>::max());
	uint64_t random_value = random();
	if (max == std::numeric_limits<uint64_t>::max())
		return random_value;
	++max;
	__uint128_t to_range = static_cast<__uint128_t>(random_value)* static_cast<__uint128_t>(max);
	uint64_t lower_bits = static_cast<uint64_t>(to_range);
	if (lower_bits < max)
	{
		uint64_t threshold = -max % max;
		while (lower_bits < threshold)
		{
			random_value = random();
			to_range = static_cast<__uint128_t>(random_value)* static_cast<__uint128_t>(max);
			lower_bits = static_cast<uint64_t>(to_range);
		}
	}
	return static_cast<uint64_t>(to_range >> 64);
}
#endif

template<typename T>
struct ManyContendedMutexRunner
{
	struct OneMutexAndState
	{
		explicit OneMutexAndState(size_t num_sums)
			: sums(num_sums)
		{
		}
		std::vector<size_t> sums;
		T mutex;
	};

	std::deque<OneMutexAndState> state;
	size_t expected_sum;
	struct ThreadState
	{
		template<typename Seed>
		ThreadState(Seed& random_seed, size_t num_loops, std::deque<OneMutexAndState>& state)
			: randomness(random_seed)
		{
			CHECK_FOR_PROGRAMMER_ERROR(num_loops % state.size() == 0);
			iteration_order.reserve(num_loops);
			for (size_t i = 0; i < num_loops; ++i)
			{
				iteration_order.push_back(&state[i % state.size()]);
			}
		}

		std::mt19937_64 randomness;
		std::vector<OneMutexAndState*> iteration_order;
	};

	std::vector<ThreadState> thread_state;

	explicit ManyContendedMutexRunner(int num_threads, size_t num_loops, size_t num_sums)
		: expected_sum(num_loops)
	{
		for (int i = 0; i < num_threads; ++i)
		{
			state.emplace_back(num_sums);
		}
		//random_seed_seq seed;
		std::random_device true_random;
		thread_state.reserve(num_threads);
		for (int i = 0; i < num_threads; ++i)
		{
			thread_state.emplace_back(true_random(), num_loops, state);
		}
	}

	void start_run()
	{
		for (OneMutexAndState& state : state)
		{
			for (size_t& sum : state.sums)
				sum = 0;
		}
	}
	void end_run()
	{
		CHECK_FOR_PROGRAMMER_ERROR(state.front().sums.front() == expected_sum);
	}

	void run_benchmark(int thread_num)
	{
		ThreadState& state = thread_state[thread_num];
		std::mt19937_64& my_thread_randomness = state.randomness;
		size_t num_states = state.iteration_order.size();
		for (auto it = state.iteration_order.begin(), end = state.iteration_order.end(); it != end; ++it)
		{
			--num_states;
			if (num_states)
			{
				size_t random_pick = UniformRandom(num_states, my_thread_randomness);
				std::iter_swap(it, it + random_pick);
			}
			OneMutexAndState& one = **it;
			one.mutex.lock();
			for (size_t& sum : one.sums)
				++sum;
			one.mutex.unlock();
		}
	}
};

template<typename T>
void BenchmarkContendedMutex(benchmark::State& state)
{
	static constexpr size_t num_loops = 1024 * 16;

	ThreadedBenchmarkRunnerMultipleTimes<ContendedMutexRunner<T>> runner(state.range(0), state.range(1), num_loops);
	while (state.KeepRunning())
	{
		runner.full_run();
	}
	runner.shut_down();
}

auto work() {
    std::chrono::high_resolution_clock::time_point time_before = std::chrono::high_resolution_clock::now();

    volatile size_t sum = 0;
    for (size_t i = 1000; i != 0; --i) {
        sum += i;
    }
    
    return std::chrono::high_resolution_clock::now() - time_before;
}

template<typename T>
struct ContendedMutexRunnerMoreIdle
{
	size_t sum = 0;
	size_t num_loops = 1;
	size_t expected_sum = 0;
	T mutex;

	explicit ContendedMutexRunnerMoreIdle(int num_threads, size_t num_loops)
		: num_loops(num_loops), expected_sum(num_threads* num_loops)
	{
	}

	void start_run()
	{
		sum = 0;
	}
	void end_run()
	{
		assert(sum == expected_sum);
	}

	void run_benchmark(int)
	{
		for (size_t i = num_loops; i != 0; --i)
		{
			mutex.lock();
			++sum;
			mutex.unlock();
		}
	}
};

template<typename T>
void BenchmarkContendedMutexMoreIdle(benchmark::State& state)
{
	static constexpr size_t num_loops = 1024;

	ThreadedBenchmarkRunnerMultipleTimes<ContendedMutexRunnerMoreIdle<T>> runner(state.range(0), state.range(1), num_loops);
	while (state.KeepRunning())
	{
		runner.full_run();
	}
	runner.shut_down();
}

template<typename T, size_t N>
struct TopNHeap
{
	void fill(const T& value)
	{
		heap.fill(value);
	}

	void add(const T& value)
	{
		if (value > heap.front())
		{
			std::pop_heap(heap.begin(), heap.end(), std::greater<>());
			heap.back() = value;
			std::push_heap(heap.begin(), heap.end(), std::greater<>());
		}
	}
	void add(T&& value)
	{
		if (value > heap.front())
		{
			std::pop_heap(heap.begin(), heap.end(), std::greater<>());
			heap.back() = std::move(value);
			std::push_heap(heap.begin(), heap.end(), std::greater<>());
		}
	}

	void sort()
	{
		std::sort_heap(heap.begin(), heap.end(), std::greater<>());
	}

	template<size_t ON>
	void merge(const TopNHeap<T, ON>& to_merge)
	{
		for (const T& val : to_merge.heap)
			add(val);
	}

	std::array<T, N> heap;
};


template<typename T>
struct LongestWaitRunner
{
	TopNHeap<double, 4> longest_waits;
	T mutex;
        double current_longest_wait{0.0};
	size_t num_loops = 1;

	explicit LongestWaitRunner(int, size_t num_loops)
		: num_loops(num_loops)
	{
		longest_waits.fill(0.0);
	}

	void start_run()
	{
		current_longest_wait = 0.0;
	}
	void end_run()
	{
		longest_waits.add(current_longest_wait);
	}

	void run_benchmark(int)
	{
		for (size_t i = num_loops; i != 0; --i)
		{
			auto time_before = std::chrono::high_resolution_clock::now();
			mutex.lock();
			auto wait_duration = std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - time_before).count();
                        if (wait_duration > current_longest_wait) {
                            current_longest_wait = wait_duration;
                        }
			mutex.unlock();
                        work();
		}
	}
};

template<typename T>
void BenchmarkLongestWait(benchmark::State& state)
{
	std::this_thread::sleep_for(std::chrono::milliseconds(100)); // give it enough non RT time to avoid RT trottling (or turn of RT trottling)
	static constexpr size_t num_loops = 1024 * 16;
	ThreadedBenchmarkRunnerMultipleTimes<LongestWaitRunner<T>> runner(state.range(0), state.range(1), num_loops);
	while (state.KeepRunning())
	{
		runner.full_run();
	}
	runner.shut_down();
	TopNHeap<double, 4> longest_waits_merged;
	longest_waits_merged.fill(0.0);
	for (auto& runner : runner.runners)
	{
		longest_waits_merged.merge(runner.state.longest_waits);
	}
	longest_waits_merged.sort();
	for (size_t i = 0; i < longest_waits_merged.heap.size(); ++i)
	{
		state.counters["Wait" + std::to_string(i)] = longest_waits_merged.heap[i];
	}
}

template<typename T>
struct LongestIdleRunner
{
	TopNHeap<double, 4> longest_idles;
	T mutex;
	double current_longest_idle = 0.0;
	size_t num_loops = 1;
	bool first = true;
        // from my unlock to my lock
	std::chrono::high_resolution_clock::time_point time_before = std::chrono::high_resolution_clock::now();

	explicit LongestIdleRunner(int, size_t num_loops)
		: num_loops(num_loops)
	{
		longest_idles.fill(0.0);
	}

	void start_run()
	{
		current_longest_idle = 0.0;
		first = true;
	}
	void end_run()
	{
		longest_idles.add(current_longest_idle);
	}

	void run_benchmark(int)
	{
		for (size_t i = num_loops; i != 0; --i)
		{
			mutex.lock();
			auto wait_duration = std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - time_before).count();
			if (first)
				first = false;
			else if (wait_duration > current_longest_idle)
				current_longest_idle = wait_duration;
			time_before = std::chrono::high_resolution_clock::now();
			mutex.unlock();
                        time_before += work();
		}
	}
};

template<typename T>
void BenchmarkLongestIdle(benchmark::State& state)
{
        std::this_thread::sleep_for(std::chrono::milliseconds(100)); // give it enough non RT time to clear RT trottling (or turn of RT trottling)
	static constexpr size_t num_loops = 1024 * 16;
	ThreadedBenchmarkRunnerMultipleTimes<LongestIdleRunner<T>> runner(state.range(0), state.range(1), num_loops);
	while (state.KeepRunning())
	{
		runner.full_run();
	}
	runner.shut_down();
	TopNHeap<double, 4> longest_idles_merged;
	longest_idles_merged.fill(0);
	for (auto& runner : runner.runners)
	{
		longest_idles_merged.merge(runner.state.longest_idles);
	}
	longest_idles_merged.sort();
	for (size_t i = 0; i < longest_idles_merged.heap.size(); ++i)
	{
		state.counters["Idle" + std::to_string(i)] = longest_idles_merged.heap[i];
	}
}

static void CustomBenchmarkArguments(benchmark::internal::Benchmark* b)
{
        unsigned int max_num_threads = 2 * std::thread::hardware_concurrency();

	for (unsigned int i = 1; i < no_of_enabled_cpus; i *= 2)
	{
		b->Args({ 1, i });
	}
	b->Args({ 1, no_of_enabled_cpus });
	b->Args({ 1, max_num_threads });

	for (unsigned int i = 2; i < no_of_enabled_cpus; i *= 2)
	{
		b->Args({ i, i });
	}
	b->Args({ no_of_enabled_cpus, no_of_enabled_cpus });
	b->Args({ max_num_threads, max_num_threads });

	for (unsigned int i = 2; i < no_of_enabled_cpus; i *= 2)
	{
		unsigned int num_threads = std::max(1U, no_of_enabled_cpus / i);
		if (num_threads != i)
		{
			b->Args({ i, num_threads });
		}
	}
	b->Args({ no_of_enabled_cpus, 1 });
	b->Args({ max_num_threads, 1 });
}


template<typename T>
struct ContendedMutexMoreWork
{
	static constexpr size_t NUM_LISTS = 8;
	std::list<size_t> linked_lists[NUM_LISTS];
	T mutex[NUM_LISTS];
	size_t num_loops = 1;

	explicit ContendedMutexMoreWork(int num_threads, size_t num_loops)
		: num_loops(num_loops)
	{
		for (std::list<size_t>& l : linked_lists)
		{
			for (int i = 0; i < num_threads; ++i)
				l.push_back(i);
		}
	}

	void start_run()
	{
	}
	void end_run()
	{
	}

	void run_benchmark(int)
	{
		for (size_t i = num_loops; i != 0; --i)
		{
			size_t index = i % NUM_LISTS;
			mutex[index].lock();
			linked_lists[index].push_back(i);
			linked_lists[index].pop_front();
			mutex[index].unlock();
		}
	}
};

template<typename T>
void BenchmarkContendedMutexMoreWork(benchmark::State& state)
{
	static constexpr size_t num_loops = 1024;
	ThreadedBenchmarkRunnerMultipleTimes<ContendedMutexMoreWork<T>> runner(state.range(0), state.range(1), num_loops);
	while (state.KeepRunning())
	{
		runner.full_run();
	}
	runner.shut_down();
}

template<typename T>
struct ThroughputRunner
{
	std::chrono::high_resolution_clock::time_point* done_time;
	size_t sum = 0;
	T mutex;

	explicit ThroughputRunner(int, std::chrono::high_resolution_clock::time_point* done_time)
		: done_time(done_time)
	{
	}

	void start_run()
	{
		sum = 0;
	}
	void end_run()
	{
	}

	void run_benchmark(int)
	{
		std::chrono::high_resolution_clock::time_point until = *done_time;
		while (std::chrono::high_resolution_clock::now() < until)
		{
			mutex.lock();
			++sum;
			mutex.unlock();
		}
	}
};

template<typename T>
void BenchmarkThroughput(benchmark::State& state)
{
	std::chrono::high_resolution_clock::time_point end_time = std::chrono::high_resolution_clock::now();
	int num_states = state.range(0);
	ThreadedBenchmarkRunnerMultipleTimes<ThroughputRunner<T>> runner(num_states, state.range(1), &end_time);
	runner.wait_until_started();
	end_time = std::chrono::high_resolution_clock::now() + std::chrono::seconds(1);
	runner.start_run();
	runner.finish_run();
	size_t sum = std::accumulate(runner.runners.begin(), runner.runners.end(), size_t(0), [](size_t l, const auto& r)
	{
		return l + r.state.sum;
	});
	runner.shut_down();
	state.counters["Throughput"] = sum;
}

template<typename T>
struct ThroughputRunnerMultipleMutex
{
	std::chrono::high_resolution_clock::time_point* done_time;
	struct alignas(64) MutexAndSum
	{
		size_t sum = 0;
		T mutex;
	};
	static constexpr size_t num_mutexes = 8;
	MutexAndSum sums[num_mutexes];

	explicit ThroughputRunnerMultipleMutex(int, std::chrono::high_resolution_clock::time_point* done_time)
		: done_time(done_time)
	{
	}

	void start_run()
	{
		for (MutexAndSum& sum : sums)
			sum.sum = 0;
	}
	void end_run()
	{
	}

	void run_benchmark(int)
	{
		std::chrono::high_resolution_clock::time_point until = *done_time;
		std::mt19937_64 randomness{ std::random_device()() };
		std::uniform_int_distribution<int> distribution(0, num_mutexes - 1);
		while (std::chrono::high_resolution_clock::now() < until)
		{
			MutexAndSum& to_increment = sums[distribution(randomness)];
			to_increment.mutex.lock();
			++to_increment.sum;
			to_increment.mutex.unlock();
		}
	}
};

template<typename T>
void BenchmarkThroughputMultipleMutex(benchmark::State& state)
{
	std::chrono::high_resolution_clock::time_point end_time = std::chrono::high_resolution_clock::now();
	int64_t num_states = state.range(0);
	ThreadedBenchmarkRunnerMultipleTimes<ThroughputRunnerMultipleMutex<T>> runner(num_states, state.range(1), &end_time);
	runner.wait_until_started();
	end_time = std::chrono::high_resolution_clock::now() + std::chrono::seconds(1);
	runner.start_run();
	runner.finish_run();
	size_t sum = std::accumulate(runner.runners.begin(), runner.runners.end(), size_t(0), [](size_t l, const auto& r)
	{
		return l + std::accumulate(std::begin(r.state.sums), std::end(r.state.sums), size_t(0), [](size_t l, const auto& r)
		{
			return l + r.sum;
		});
	});
	runner.shut_down();
	state.counters["Throughput"] = sum;
}

template<typename T>
void BenchmarkDemingWS(benchmark::State& state)
{
	// code from http://demin.ws/blog/english/2012/05/05/atomic-spinlock-mutex/
	T mutex;
	int value = 0;
	semaphore sem;
	semaphore one_loop_done;
	bool done = false;
	auto loop = [&](bool inc, int limit)
	{
		for (int i = 0; i < limit; ++i)
		{
			mutex.lock();
			if (inc)
				++value;
			else
				--value;
			mutex.unlock();
		}
	};
	auto background_thread = [&](bool inc, int limit)
	{
		for (;;)
		{
			sem.acquire();
			if (done)
				break;
			loop(inc, limit);
			one_loop_done.release();
		}
	};
	int num_increment = 20000000;
	int num_decrement = 10000000;
	std::thread t(background_thread, true, num_increment);
	while (state.KeepRunning())
	{
		value = 0;
		sem.release();
		loop(false, num_decrement);
		one_loop_done.acquire();
	}
	assert(value == num_increment - num_decrement);
	done = true;
	sem.release();
	t.join();
}

/*#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <memory.h>*/


// code from https://github.com/goldshtn/shmemq-blog
template<typename Mutex>
struct alignas(64) shmemq
{
	shmemq(unsigned long max_count, unsigned int element_size)
		: element_size(element_size)
		, max_size(max_count * element_size)
	{
		data.reset(new char[max_size]);
	}

	bool try_enqueue(void* element, size_t len)
	{
		if (len != element_size)
			return false;

		std::lock_guard<Mutex> lock(mutex);

		if (write_index - read_index == max_size)
			return false; // There is no more room in the queue

		memcpy(data.get() + write_index % max_size, element, len);
		write_index += element_size;
		return true;
	}

	bool try_dequeue(void* element, size_t len)
	{
		if (len != element_size)
			return false;

		std::lock_guard<Mutex> lock(mutex);

		if (read_index == write_index)
			return false; // There are no elements that haven't been consumed yet

		memcpy(element, data.get() + read_index % max_size, len);
		read_index += element_size;
		return true;
	}

	Mutex mutex;
	size_t element_size = 0;
	size_t max_size = 0;
	size_t read_index = 0;
	size_t write_index = 0;
	std::unique_ptr<char[]> data;
};

template<typename T, typename State>
void BenchmarkShmemq(State& state)
{
	static constexpr int QUEUE_SIZE = 1000;
	static constexpr int REPETITIONS = 10000;
	int DATA_SIZE = state.range(0);

	shmemq<T> server_queue(QUEUE_SIZE, DATA_SIZE);
	shmemq<T> client_queue(QUEUE_SIZE, DATA_SIZE);

	auto build_message = [&]
	{
		std::unique_ptr<char[]> result(new char[DATA_SIZE]);
		memset(result.get(), 0, DATA_SIZE);
		int forty_two = 42;
		assert(sizeof(forty_two) <= static_cast<size_t>(DATA_SIZE));
		memcpy(result.get(), &forty_two, sizeof(forty_two));
		assert(5 + sizeof(forty_two) <= static_cast<size_t>(DATA_SIZE));
		memcpy(result.get() + sizeof(forty_two), "Hello", 5);
		return result;
	};

	auto server = [&]
	{
		std::unique_ptr<char[]> msg = build_message();
		for (int i = 0; i < REPETITIONS; ++i)
		{
			while (!server_queue.try_dequeue(msg.get(), DATA_SIZE))
			{
			}
			while (!client_queue.try_enqueue(msg.get(), DATA_SIZE))
			{
			}
		}
	};
	auto client = [&]
	{
		std::unique_ptr<char[]> msg = build_message();
		for (int i = 0; i < REPETITIONS; ++i)
		{
			while (!server_queue.try_enqueue(msg.get(), DATA_SIZE))
			{
			}
			while (!client_queue.try_dequeue(msg.get(), DATA_SIZE))
			{
			}
		}
	};
	while (state.KeepRunning())
	{
		std::thread s(server);
		std::thread c(client);
		s.join();
		c.join();
	}
	state.SetItemsProcessed(REPETITIONS * state.iterations());
}

// skip RegisterBenchmarkWithAllMutexes(BenchmarkShmemq, ->Arg(256));
// skip RegisterBenchmarkWithAllMutexes(BenchmarkDemingWS);
//err RegisterBenchmarkWithAllMutexes(BenchmarkThroughputMultipleMutex, ->Apply(CustomBenchmarkArguments));
//err RegisterBenchmarkWithAllMutexes(BenchmarkThroughput, ->Apply(CustomBenchmarkArguments));
// skip RegisterBenchmarkWithAllMutexes(BenchmarkContendedMutex, ->Apply(CustomBenchmarkArguments));
RegisterBenchmarkWithAllMutexes(BenchmarkLongestIdle, ->Apply(CustomBenchmarkArguments));
RegisterBenchmarkWithAllMutexes(BenchmarkLongestWait, ->Apply(CustomBenchmarkArguments));
// skip RegisterBenchmarkWithAllMutexes(BenchmarkContendedMutexMoreWork, ->Apply(CustomBenchmarkArguments));
// RegisterBenchmarkWithAllMutexes(BenchmarkContendedMutexMoreIdle, ->Apply(CustomBenchmarkArguments));

void benchmark_yield(benchmark::State& state)
{
	while (state.KeepRunning())
	{
		std::this_thread::yield();
	}
}
BENCHMARK(benchmark_yield);

#include <sys/types.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <iostream>
#include <fstream>
#include <string>

long long check_proc_setting(const std::string &filename) {
            std::ifstream sched_file_us {filename};
            if (!sched_file_us.is_open()) {
                std::cout << "Unable to open file:" << filename << std::endl;
                exit(1);
            }
            long long read_value_us;
            sched_file_us >> read_value_us;
            sched_file_us.close();

            return read_value_us;
}

const std::string runtime_filename = "/proc/sys/kernel/sched_rt_runtime_us";
const std::string period_filename = "/proc/sys/kernel/sched_rt_period_us";
#define SUGGESTED_PERIOD_US (500000)
#define SUGGESTED_RUNTIME_US (400000) // take away 100 ms every 500 ms
#define SAFE_PERIOD_US (5000000)
#define SAFE_RUNTIME_US (4990000) // take away 10 ms every 5 s (larger than test steps)
int main(int argc, char* argv[])
{
        //no_of_enabled_cpus = count_enabled_cpus();
        std::cout << "Number of enabled cpus " << no_of_enabled_cpus << std::endl;

        int rt_rc = set_realtime_priority(getpid(), SCHED_RR, 11); // RR 10 is used by firefox...

        long long actual_runtime_us = check_proc_setting(runtime_filename);
        long long actual_period_us = check_proc_setting(period_filename);

        if (no_of_enabled_cpus >= std::thread::hardware_concurrency() && rt_rc == 0) {
            std::cout << "***WARNING*** ALL cpus are enabled for use with RT ";
            if (actual_runtime_us < 0) {
                std::cout << "and no protection" << std::endl;
                std::cout << "***CRITICAL*** would lock up for long time - exiting!" << std::endl;
                exit(1);
            } else if (actual_runtime_us != SUGGESTED_RUNTIME_US || actual_period_us != SUGGESTED_PERIOD_US) {
                std::cout << "got RT protection, but NOT the recomended (to show its effect), as root do:" << std::endl;
                // order matters
                std::cout << "# echo " << SUGGESTED_RUNTIME_US << " > " << runtime_filename << std::endl;
                std::cout << "# echo " << SUGGESTED_PERIOD_US << " > " << period_filename << std::endl;
            } else {
                std::cout << "got aggressive protection with recommended values, will NOT perform as expected" << std::endl;
            }
            std::cout << "Alternative protection is to avoid using one CPU for RT, i.e. set affinity excluding one cpu for all RT treads" << std::endl;
            std::cout << "$ taskset --cpu-list 1-999 " << argv[0] << std::endl; // cpu0 is not selected
        } else {
            std::cout << "***INFO*** Spare CPUs are allowed to run non RT processes ";
            if (actual_runtime_us == SAFE_RUNTIME_US && actual_period_us == SAFE_PERIOD_US) {
                std::cout << "using recomended setting for development" << std::endl;
            } else {
                if (actual_runtime_us < 0) {
                    std::cout << "and no RT protection (recomended setting for deployment)" << std::endl;
                } else if (actual_runtime_us == SUGGESTED_RUNTIME_US && actual_period_us == SUGGESTED_PERIOD_US) {
                    std::cout << "using aggresive RT protection (should not affect result)" << std::endl;
                } else {
                    std::cout << "and RT minor protection interfering with tests, as root do:" << std::endl;
                }
                // order matters
                std::cout << "# echo " << SAFE_PERIOD_US << " > " << period_filename << std::endl;
                std::cout << "# echo " << SAFE_RUNTIME_US << " > " << runtime_filename << std::endl;
                std::cout << "for actual demployment (completely turning of the protection) as root do:" << std::endl;
                std::cout << "# echo -1 > " << runtime_filename << std::endl;
            }
        }

        if (rt_rc != 0) {
            std::cout << "***WARNING*** did not get RT prio, will NOT perform as expected" << std::endl;
        }

	::benchmark::Initialize(&argc, argv);
	::benchmark::RunSpecifiedBenchmarks();
}
