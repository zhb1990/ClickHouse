#include <gtest/gtest.h>

#include <barrier>
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <vector>
#include <thread>
#include <pcg_random.hpp>

#include <base/types.h>
#include <base/sleep.h>
#include <Common/Exception.h>
#include <Common/AsyncLoader.h>
#include <Common/randomSeed.h>

using namespace DB;

namespace CurrentMetrics
{
    extern const Metric TablesLoaderThreads;
    extern const Metric TablesLoaderThreadsActive;
}

namespace DB::ErrorCodes
{
    extern const int ASYNC_LOAD_CYCLE;
    extern const int ASYNC_LOAD_FAILED;
    extern const int ASYNC_LOAD_CANCELED;
}

struct AsyncLoaderTest
{
    AsyncLoader loader;

    std::mutex rng_mutex;
    pcg64 rng{randomSeed()};

    explicit AsyncLoaderTest(size_t max_threads = 1)
        : loader(CurrentMetrics::TablesLoaderThreads, CurrentMetrics::TablesLoaderThreadsActive, max_threads, /* log_failures = */ false)
    {}

    template <typename T>
    T randomInt(T from, T to)
    {
        std::uniform_int_distribution<T> distribution(from, to);
        std::scoped_lock lock(rng_mutex);
        return distribution(rng);
    }

    void randomSleepUs(UInt64 min_us, UInt64 max_us, int probability_percent)
    {
        if (randomInt(0, 99) < probability_percent)
            std::this_thread::sleep_for(std::chrono::microseconds(randomInt(min_us, max_us)));
    }

    template <typename JobFunc>
    LoadJobSet randomJobSet(int job_count, int dep_probability_percent, JobFunc job_func, std::string_view name_prefix = "job")
    {
        std::vector<LoadJobPtr> jobs;
        jobs.reserve(job_count);
        for (int j = 0; j < job_count; j++)
        {
            LoadJobSet deps;
            for (int d = 0; d < j; d++)
            {
                if (randomInt(0, 99) < dep_probability_percent)
                    deps.insert(jobs[d]);
            }
            jobs.push_back(makeLoadJob(std::move(deps), fmt::format("{}{}", name_prefix, j), job_func));
        }
        return {jobs.begin(), jobs.end()};
    }

    template <typename JobFunc>
    LoadJobSet randomJobSet(int job_count, int dep_probability_percent, const std::vector<LoadJobPtr> & external_deps, JobFunc job_func, std::string_view name_prefix = "job")
    {
        std::vector<LoadJobPtr> jobs;
        jobs.reserve(job_count);
        for (int j = 0; j < job_count; j++)
        {
            LoadJobSet deps;
            for (int d = 0; d < j; d++)
            {
                if (randomInt(0, 99) < dep_probability_percent)
                    deps.insert(jobs[d]);
            }
            if (!external_deps.empty() && randomInt(0, 99) < dep_probability_percent)
                deps.insert(external_deps[randomInt<size_t>(0, external_deps.size() - 1)]);
            jobs.push_back(makeLoadJob(std::move(deps), fmt::format("{}{}", name_prefix, j), job_func));
        }
        return {jobs.begin(), jobs.end()};
    }

    template <typename JobFunc>
    LoadJobSet chainJobSet(int job_count, JobFunc job_func, std::string_view name_prefix = "job")
    {
        std::vector<LoadJobPtr> jobs;
        jobs.reserve(job_count);
        jobs.push_back(makeLoadJob({}, fmt::format("{}{}", name_prefix, 0), job_func));
        for (int j = 1; j < job_count; j++)
            jobs.push_back(makeLoadJob({ jobs[j - 1] }, fmt::format("{}{}", name_prefix, j), job_func));
        return {jobs.begin(), jobs.end()};
    }

    LoadTaskPtr schedule(LoadJobSet && jobs)
    {
        LoadTaskPtr task = makeLoadTask(loader, std::move(jobs));
        task->schedule();
        return task;
    }
};

TEST(AsyncLoader, Smoke)
{
    AsyncLoaderTest t(2);

    static constexpr ssize_t low_priority = -1;

    std::atomic<size_t> jobs_done{0};
    std::atomic<size_t> low_priority_jobs_done{0};

    auto job_func = [&] (const LoadJobPtr & self) {
        jobs_done++;
        if (self->priority() == low_priority)
            low_priority_jobs_done++;
    };

    {
        auto job1 = makeLoadJob({}, "job1", job_func);
        auto job2 = makeLoadJob({ job1 }, "job2", job_func);
        auto task1 = t.schedule({ job1, job2 });

        auto job3 = makeLoadJob({ job2 }, "job3", job_func);
        auto job4 = makeLoadJob({ job2 }, "job4", job_func);
        auto task2 = t.schedule({ job3, job4 });
        auto job5 = makeLoadJob({ job3, job4 }, "job5", low_priority, job_func);
        task2->merge(t.schedule({ job5 }));

        std::thread waiter_thread([=] { job5->wait(); });

        t.loader.start();

        job3->wait();
        t.loader.wait();
        job4->wait();

        waiter_thread.join();

        ASSERT_EQ(job1->status(), LoadStatus::OK);
        ASSERT_EQ(job2->status(), LoadStatus::OK);
    }

    ASSERT_EQ(jobs_done, 5);
    ASSERT_EQ(low_priority_jobs_done, 1);

    t.loader.stop();
}

TEST(AsyncLoader, CycleDetection)
{
    AsyncLoaderTest t;

    auto job_func = [&] (const LoadJobPtr &) {};

    LoadJobPtr cycle_breaker; // To avoid memleak we introduce with a cycle

    try
    {
        std::vector<LoadJobPtr> jobs;
        jobs.reserve(16);
        jobs.push_back(makeLoadJob({}, "job0", job_func));
        jobs.push_back(makeLoadJob({ jobs[0] }, "job1", job_func));
        jobs.push_back(makeLoadJob({ jobs[0], jobs[1] }, "job2", job_func));
        jobs.push_back(makeLoadJob({ jobs[0], jobs[2] }, "job3", job_func));

        // Actually it is hard to construct a cycle, but suppose someone was able to succeed violating constness
        const_cast<LoadJobSet &>(jobs[1]->dependencies).insert(jobs[3]);
        cycle_breaker = jobs[1];

        // Add couple unrelated jobs
        jobs.push_back(makeLoadJob({ jobs[1] }, "job4", job_func));
        jobs.push_back(makeLoadJob({ jobs[4] }, "job5", job_func));
        jobs.push_back(makeLoadJob({ jobs[3] }, "job6", job_func));
        jobs.push_back(makeLoadJob({ jobs[1], jobs[2], jobs[3], jobs[4], jobs[5], jobs[6] }, "job7", job_func));

        // Also add another not connected jobs
        jobs.push_back(makeLoadJob({}, "job8", job_func));
        jobs.push_back(makeLoadJob({}, "job9", job_func));
        jobs.push_back(makeLoadJob({ jobs[9] }, "job10", job_func));

        auto task1 = t.schedule({ jobs.begin(), jobs.end()});
        FAIL();
    }
    catch (Exception & e)
    {
        int present[] = { 0, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0 };
        for (int i = 0; i < std::size(present); i++)
            ASSERT_EQ(e.message().find(fmt::format("job{}", i)) != String::npos, present[i]);
    }

    const_cast<LoadJobSet &>(cycle_breaker->dependencies).clear();
}

TEST(AsyncLoader, CancelPendingJob)
{
    AsyncLoaderTest t;

    auto job_func = [&] (const LoadJobPtr &) {};

    auto job = makeLoadJob({}, "job", job_func);
    auto task = t.schedule({ job });

    task->remove(); // this cancels pending the job (async loader was not started to execute it)

    ASSERT_EQ(job->status(), LoadStatus::CANCELED);
    try
    {
        job->wait();
        FAIL();
    }
    catch (Exception & e)
    {
        ASSERT_EQ(e.code(), ErrorCodes::ASYNC_LOAD_CANCELED);
    }
}

TEST(AsyncLoader, CancelPendingTask)
{
    AsyncLoaderTest t;

    auto job_func = [&] (const LoadJobPtr &) {};

    auto job1 = makeLoadJob({}, "job1", job_func);
    auto job2 = makeLoadJob({ job1 }, "job2", job_func);
    auto task = t.schedule({ job1, job2 });

    task->remove(); // this cancels both jobs (async loader was not started to execute it)

    ASSERT_EQ(job1->status(), LoadStatus::CANCELED);
    ASSERT_EQ(job2->status(), LoadStatus::CANCELED);

    try
    {
        job1->wait();
        FAIL();
    }
    catch (Exception & e)
    {
        ASSERT_TRUE(e.code() == ErrorCodes::ASYNC_LOAD_CANCELED);
    }

    try
    {
        job2->wait();
        FAIL();
    }
    catch (Exception & e)
    {
        ASSERT_TRUE(e.code() == ErrorCodes::ASYNC_LOAD_CANCELED);
    }
}

TEST(AsyncLoader, CancelPendingDependency)
{
    AsyncLoaderTest t;

    auto job_func = [&] (const LoadJobPtr &) {};

    auto job1 = makeLoadJob({}, "job1", job_func);
    auto job2 = makeLoadJob({ job1 }, "job2", job_func);
    auto task1 = t.schedule({ job1 });
    auto task2 = t.schedule({ job2 });

    task1->remove(); // this cancels both jobs, due to dependency (async loader was not started to execute it)

    ASSERT_EQ(job1->status(), LoadStatus::CANCELED);
    ASSERT_EQ(job2->status(), LoadStatus::CANCELED);

    try
    {
        job1->wait();
        FAIL();
    }
    catch (Exception & e)
    {
        ASSERT_TRUE(e.code() == ErrorCodes::ASYNC_LOAD_CANCELED);
    }

    try
    {
        job2->wait();
        FAIL();
    }
    catch (Exception & e)
    {
        ASSERT_TRUE(e.code() == ErrorCodes::ASYNC_LOAD_CANCELED);
    }
}

TEST(AsyncLoader, CancelExecutingJob)
{
    AsyncLoaderTest t;
    t.loader.start();

    std::barrier sync(2);

    auto job_func = [&] (const LoadJobPtr &)
    {
        sync.arrive_and_wait(); // (A) sync with main thread
        sync.arrive_and_wait(); // (B) wait for waiter
        // signals (C)
    };

    auto job = makeLoadJob({}, "job", job_func);
    auto task = t.schedule({ job });

    sync.arrive_and_wait(); // (A) wait for job to start executing
    std::thread canceler([&]
    {
        task->remove(); // waits for (C)
    });
    while (job->waitersCount() == 0)
        std::this_thread::yield();
    ASSERT_EQ(job->status(), LoadStatus::PENDING);
    sync.arrive_and_wait(); // (B) sync with job
    canceler.join();

    ASSERT_EQ(job->status(), LoadStatus::OK);
    job->wait();
}

TEST(AsyncLoader, CancelExecutingTask)
{
    AsyncLoaderTest t(16);
    t.loader.start();
    std::barrier sync(2);

    auto blocker_job_func = [&] (const LoadJobPtr &)
    {
        sync.arrive_and_wait(); // (A) sync with main thread
        sync.arrive_and_wait(); // (B) wait for waiter
        // signals (C)
    };

    auto job_to_cancel_func = [&] (const LoadJobPtr &)
    {
        FAIL(); // this job should be canceled
    };

    auto job_to_succeed_func = [&] (const LoadJobPtr &)
    {
    };

    // Make several iterations to catch the race (if any)
    for (int iteration = 0; iteration < 10; iteration++) {
        std::vector<LoadJobPtr> task1_jobs;
        task1_jobs.reserve(256);
        auto blocker_job = makeLoadJob({}, "blocker_job", blocker_job_func);
        task1_jobs.push_back(blocker_job);
        for (int i = 0; i < 100; i++)
            task1_jobs.push_back(makeLoadJob({ blocker_job }, "job_to_cancel", job_to_cancel_func));
        auto task1 = t.schedule({ task1_jobs.begin(), task1_jobs.end() });
        auto job_to_succeed = makeLoadJob({ blocker_job }, "job_to_succeed", job_to_succeed_func);
        auto task2 = t.schedule({ job_to_succeed });

        sync.arrive_and_wait(); // (A) wait for job to start executing
        std::thread canceler([&]
        {
            task1->remove(); // waits for (C)
        });
        while (blocker_job->waitersCount() == 0)
            std::this_thread::yield();
        ASSERT_EQ(blocker_job->status(), LoadStatus::PENDING);
        sync.arrive_and_wait(); // (B) sync with job
        canceler.join();
        t.loader.wait();

        ASSERT_EQ(blocker_job->status(), LoadStatus::OK);
        ASSERT_EQ(job_to_succeed->status(), LoadStatus::OK);
        for (const auto & job : task1_jobs)
        {
            if (job != blocker_job)
                ASSERT_EQ(job->status(), LoadStatus::CANCELED);
        }
    }
}

TEST(AsyncLoader, JobFailure)
{
    AsyncLoaderTest t;
    t.loader.start();

    std::string error_message = "test job failure";

    auto job_func = [&] (const LoadJobPtr &) {
        throw std::runtime_error(error_message);
    };

    auto job = makeLoadJob({}, "job", job_func);
    auto task = t.schedule({ job });

    t.loader.wait();

    ASSERT_EQ(job->status(), LoadStatus::FAILED);
    try
    {
        job->wait();
        FAIL();
    }
    catch (Exception & e)
    {
        ASSERT_EQ(e.code(), ErrorCodes::ASYNC_LOAD_FAILED);
        ASSERT_TRUE(e.message().find(error_message) != String::npos);
    }
}

TEST(AsyncLoader, ScheduleJobWithFailedDependencies)
{
    AsyncLoaderTest t;
    t.loader.start();

    std::string_view error_message = "test job failure";

    auto failed_job_func = [&] (const LoadJobPtr &) {
        throw Exception(ErrorCodes::ASYNC_LOAD_FAILED, "{}", error_message);
    };

    auto failed_job = makeLoadJob({}, "failed_job", failed_job_func);
    auto failed_task = t.schedule({ failed_job });

    t.loader.wait();

    auto job_func = [&] (const LoadJobPtr &) {};

    auto job1 = makeLoadJob({ failed_job }, "job1", job_func);
    auto job2 = makeLoadJob({ job1 }, "job2", job_func);
    auto task = t.schedule({ job1, job2 });

    t.loader.wait();

    ASSERT_EQ(job1->status(), LoadStatus::CANCELED);
    ASSERT_EQ(job2->status(), LoadStatus::CANCELED);
    try
    {
        job1->wait();
        FAIL();
    }
    catch (Exception & e)
    {
        ASSERT_EQ(e.code(), ErrorCodes::ASYNC_LOAD_CANCELED);
        ASSERT_TRUE(e.message().find(error_message) != String::npos);
    }
    try
    {
        job2->wait();
        FAIL();
    }
    catch (Exception & e)
    {
        ASSERT_EQ(e.code(), ErrorCodes::ASYNC_LOAD_CANCELED);
        ASSERT_TRUE(e.message().find(error_message) != String::npos);
    }
}

TEST(AsyncLoader, ScheduleJobWithCanceledDependencies)
{
    AsyncLoaderTest t;

    auto canceled_job_func = [&] (const LoadJobPtr &) {};
    auto canceled_job = makeLoadJob({}, "canceled_job", canceled_job_func);
    auto canceled_task = t.schedule({ canceled_job });
    canceled_task->remove();

    t.loader.start();

    auto job_func = [&] (const LoadJobPtr &) {};
    auto job1 = makeLoadJob({ canceled_job }, "job1", job_func);
    auto job2 = makeLoadJob({ job1 }, "job2", job_func);
    auto task = t.schedule({ job1, job2 });

    t.loader.wait();

    ASSERT_EQ(job1->status(), LoadStatus::CANCELED);
    ASSERT_EQ(job2->status(), LoadStatus::CANCELED);
    try
    {
        job1->wait();
        FAIL();
    }
    catch (Exception & e)
    {
        ASSERT_EQ(e.code(), ErrorCodes::ASYNC_LOAD_CANCELED);
    }
    try
    {
        job2->wait();
        FAIL();
    }
    catch (Exception & e)
    {
        ASSERT_EQ(e.code(), ErrorCodes::ASYNC_LOAD_CANCELED);
    }
}

TEST(AsyncLoader, TestConcurrency)
{
    AsyncLoaderTest t(10);
    t.loader.start();

    for (int concurrency = 1; concurrency <= 10; concurrency++)
    {
        std::barrier sync(concurrency);

        std::atomic<int> executing{0};
        auto job_func = [&] (const LoadJobPtr &)
        {
            executing++;
            ASSERT_LE(executing, concurrency);
            sync.arrive_and_wait();
            executing--;
        };

        std::vector<LoadTaskPtr> tasks;
        tasks.reserve(concurrency);
        for (int i = 0; i < concurrency; i++)
            tasks.push_back(t.schedule(t.chainJobSet(5, job_func)));
        t.loader.wait();
        ASSERT_EQ(executing, 0);
    }
}

TEST(AsyncLoader, TestOverload)
{
    AsyncLoaderTest t(3);
    t.loader.start();

    size_t max_threads = t.loader.getMaxThreads();
    std::atomic<int> executing{0};

    for (int concurrency = 4; concurrency <= 8; concurrency++)
    {
        auto job_func = [&] (const LoadJobPtr &)
        {
            executing++;
            t.randomSleepUs(100, 200, 100);
            ASSERT_LE(executing, max_threads);
            executing--;
        };

        t.loader.stop();
        std::vector<LoadTaskPtr> tasks;
        tasks.reserve(concurrency);
        for (int i = 0; i < concurrency; i++)
            tasks.push_back(t.schedule(t.chainJobSet(5, job_func)));
        t.loader.start();
        t.loader.wait();
        ASSERT_EQ(executing, 0);
    }
}

TEST(AsyncLoader, StaticPriorities)
{
    AsyncLoaderTest t(1);

    std::string schedule;

    auto job_func = [&] (const LoadJobPtr & self)
    {
        schedule += fmt::format("{}{}", self->name, self->priority());
    };

    std::vector<LoadJobPtr> jobs;
    jobs.push_back(makeLoadJob({}, "A", 0, job_func)); // 0
    jobs.push_back(makeLoadJob({ jobs[0] }, "B", 3, job_func)); // 1
    jobs.push_back(makeLoadJob({ jobs[0] }, "C", 4, job_func)); // 2
    jobs.push_back(makeLoadJob({ jobs[0] }, "D", 1, job_func)); // 3
    jobs.push_back(makeLoadJob({ jobs[0] }, "E", 2, job_func)); // 4
    jobs.push_back(makeLoadJob({ jobs[3], jobs[4] }, "F", 0, job_func)); // 5
    jobs.push_back(makeLoadJob({ jobs[5] }, "G", 0, job_func)); // 6
    jobs.push_back(makeLoadJob({ jobs[6] }, "H", 9, job_func)); // 7
    auto task = t.schedule({ jobs.begin(), jobs.end() });

    t.loader.start();
    t.loader.wait();

    ASSERT_EQ(schedule, "A9E9D9F9G9H9C4B3");
}

TEST(AsyncLoader, DynamicPriorities)
{
    AsyncLoaderTest t(1);

    for (bool prioritize : {false, true})
    {
        std::string schedule;

        LoadJobPtr job_to_prioritize;

        auto job_func = [&] (const LoadJobPtr & self)
        {
            if (prioritize && self->name == "C")
                t.loader.prioritize(job_to_prioritize, 9); // dynamic prioritization
            schedule += fmt::format("{}{}", self->name, self->priority());
        };

        // Job DAG with initial priorities. During execution of C4, job G0 priority is increased to G9, postponing B3 job executing.
        // A0 -+-> B3
        //     |
        //     `-> C4
        //     |
        //     `-> D1 -.
        //     |       +-> F0 --> G0 --> H0
        //     `-> E2 -'
        std::vector<LoadJobPtr> jobs;
        jobs.push_back(makeLoadJob({}, "A", 0, job_func)); // 0
        jobs.push_back(makeLoadJob({ jobs[0] }, "B", 3, job_func)); // 1
        jobs.push_back(makeLoadJob({ jobs[0] }, "C", 4, job_func)); // 2
        jobs.push_back(makeLoadJob({ jobs[0] }, "D", 1, job_func)); // 3
        jobs.push_back(makeLoadJob({ jobs[0] }, "E", 2, job_func)); // 4
        jobs.push_back(makeLoadJob({ jobs[3], jobs[4] }, "F", 0, job_func)); // 5
        jobs.push_back(makeLoadJob({ jobs[5] }, "G", 0, job_func)); // 6
        jobs.push_back(makeLoadJob({ jobs[6] }, "H", 0, job_func)); // 7
        auto task = t.schedule({ jobs.begin(), jobs.end() });

        job_to_prioritize = jobs[6];

        t.loader.start();
        t.loader.wait();
        t.loader.stop();

        if (prioritize)
            ASSERT_EQ(schedule, "A4C4E9D9F9G9B3H0");
        else
            ASSERT_EQ(schedule, "A4C4B3E2D1F0G0H0");
    }
}

TEST(AsyncLoader, RandomIndependentTasks)
{
    AsyncLoaderTest t(16);
    t.loader.start();

    auto job_func = [&] (const LoadJobPtr & self)
    {
        for (const auto & dep : self->dependencies)
            ASSERT_EQ(dep->status(), LoadStatus::OK);
        t.randomSleepUs(100, 500, 5);
    };

    std::vector<LoadTaskPtr> tasks;
    tasks.reserve(512);
    for (int i = 0; i < 512; i++)
    {
        int job_count = t.randomInt(1, 32);
        tasks.push_back(t.schedule(t.randomJobSet(job_count, 5, job_func)));
        t.randomSleepUs(100, 900, 20); // avg=100us
    }
}

TEST(AsyncLoader, RandomDependentTasks)
{
    AsyncLoaderTest t(16);
    t.loader.start();

    std::mutex mutex;
    std::condition_variable cv;
    std::vector<LoadTaskPtr> tasks;
    std::vector<LoadJobPtr> all_jobs;

    auto job_func = [&] (const LoadJobPtr & self)
    {
        for (const auto & dep : self->dependencies)
            ASSERT_EQ(dep->status(), LoadStatus::OK);
        cv.notify_one();
    };

    std::unique_lock lock{mutex};

    int tasks_left = 1000;
    tasks.reserve(tasks_left);
    while (tasks_left-- > 0)
    {
        cv.wait(lock, [&] { return t.loader.getScheduledJobCount() < 100; });

        // Add one new task
        int job_count = t.randomInt(1, 32);
        LoadJobSet jobs = t.randomJobSet(job_count, 5, all_jobs, job_func);
        all_jobs.insert(all_jobs.end(), jobs.begin(), jobs.end());
        tasks.push_back(t.schedule(std::move(jobs)));

        // Cancel random old task
        if (tasks.size() > 100)
            tasks.erase(tasks.begin() + t.randomInt<size_t>(0, tasks.size() - 1));
    }

    t.loader.wait();
}

TEST(AsyncLoader, SetMaxThreads)
{
    AsyncLoaderTest t(1);

    std::atomic<int> sync_index{0};
    std::atomic<int> executing{0};
    int max_threads_values[] = {1, 2, 3, 4, 5, 4, 3, 2, 1, 5, 10, 5, 1, 20, 1};
    std::vector<std::unique_ptr<std::barrier<>>> syncs;
    syncs.reserve(std::size(max_threads_values));
    for (int max_threads : max_threads_values)
        syncs.push_back(std::make_unique<std::barrier<>>(max_threads + 1));


    auto job_func = [&] (const LoadJobPtr &)
    {
        int idx = sync_index;
        if (idx < syncs.size())
        {
            executing++;
            syncs[idx]->arrive_and_wait(); // (A)
            executing--;
            syncs[idx]->arrive_and_wait(); // (B)
        }
    };

    // Generate enough independent jobs
    for (int i = 0; i < 1000; i++)
        t.schedule({makeLoadJob({}, "job", job_func)})->detach();

    t.loader.start();
    while (sync_index < syncs.size())
    {
        // Wait for `max_threads` jobs to start executing
        int idx = sync_index;
        while (executing.load() != max_threads_values[idx])
        {
            ASSERT_LE(executing, max_threads_values[idx]);
            std::this_thread::yield();
        }

        // Allow all jobs to finish
        syncs[idx]->arrive_and_wait(); // (A)
        sync_index++;
        if (sync_index < syncs.size())
            t.loader.setMaxThreads(max_threads_values[sync_index]);
        syncs[idx]->arrive_and_wait(); // (B) this sync point is required to allow `executing` value to go back down to zero after we change number of workers
    }
    t.loader.wait();
}
