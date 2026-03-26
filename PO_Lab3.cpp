#include <iostream>
#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <chrono>
#include <atomic>
#include <iomanip>
#include <string>
#include <sstream>

struct Task {
    std::function<void(const std::atomic<bool>&)> func;
    int duration_sec;
    std::chrono::steady_clock::time_point arrival_time;
    int id;

    bool operator>(const Task& other) const {
        return duration_sec > other.duration_sec;
    }
};

class ThreadPool {
private:
    std::priority_queue<Task, std::vector<Task>, std::greater<Task>> queue1;
    std::queue<Task> queue2;
    std::vector<std::thread> workers;
    std::mutex mtx;
    std::mutex out_mtx; 
    std::condition_variable cv;

    std::atomic<bool> stop{false};
    std::atomic<bool> abort_requested{false};
    std::atomic<bool> paused{false};

    std::atomic<long long> total_idle_time_ms{0};
    std::atomic<long long> total_exec_time_ms{0};
    std::atomic<int> tasks_completed{0};
    std::atomic<int> tasks_transferred{0};
    int initial_threads_count;

    std::vector<size_t> q1_sizes;
    std::vector<size_t> q2_sizes;
    std::thread monitor_thread;

public:
    void safe_log(const std::string& msg) {
        std::lock_guard<std::mutex> lock(out_mtx);
        std::cout << msg << std::endl;
    }

    ThreadPool(int threads_q1, int threads_q2) : initial_threads_count(threads_q1 + threads_q2) {
        for (int i = 0; i < threads_q1; ++i) workers.emplace_back([this] { worker_routine(1); });
        for (int i = 0; i < threads_q2; ++i) workers.emplace_back([this] { worker_routine(2); });

        monitor_thread = std::thread([this] {
            while (!stop && !abort_requested) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                transfer_stale_tasks();
                std::lock_guard<std::mutex> lock(mtx);
                q1_sizes.push_back(queue1.size());
                q2_sizes.push_back(queue2.size());
            }
        });
    }

    ~ThreadPool() {
        if (!stop && !abort_requested) terminate();
        if (monitor_thread.joinable()) monitor_thread.join();
    }

    void add_task(int id, int duration) {
        std::unique_lock<std::mutex> lock(mtx);
        if (stop || abort_requested) return;
        auto task_logic = [id, duration](const std::atomic<bool>& abort_sig) {
            for (int s = 0; s < duration; ++s) {
                if (abort_sig) return; 
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        };
        queue1.push({task_logic, duration, std::chrono::steady_clock::now(), id});
        cv.notify_one();
    }

    void toggle_pause() {
        paused = !paused;
        if (!paused) cv.notify_all();
        safe_log(paused ? "\n[System] POOL PAUSED" : "\n[System] POOL RESUMED");
    }

    void terminate() {
        stop = true;
        cv.notify_all();
        join_all_workers();
    }

    void abort() {
        abort_requested = true;
        {
            std::lock_guard<std::mutex> lock(mtx);
            while (!queue1.empty()) queue1.pop();
            while (!queue2.empty()) queue2.pop();
        }
        safe_log("\n[System] ABORTING (clearing queues & signaling workers)...");
        cv.notify_all();
        join_all_workers();
    }

    void print_statistics() {
        std::lock_guard<std::mutex> lock(mtx);
        double avg_q1 = 0, avg_q2 = 0;
        for (auto s : q1_sizes) avg_q1 += s;
        for (auto s : q2_sizes) avg_q2 += s;

        std::cout << "\n--- Final Statistics ---" << std::endl;
        std::cout << "Threads Created: " << initial_threads_count << std::endl;
        std::cout << "Tasks Completed: " << tasks_completed.load() << std::endl;
        std::cout << "Tasks Moved to Q2: " << tasks_transferred.load() << std::endl;
        if (!q1_sizes.empty()) {
            std::cout << "Avg Q1 Length: " << std::fixed << std::setprecision(2) << avg_q1 / q1_sizes.size() << std::endl;
            std::cout << "Avg Q2 Length: " << avg_q2 / q2_sizes.size() << std::endl;
        }
        if (tasks_completed > 0) {
            std::cout << "Avg Exec Time: " << (total_exec_time_ms.load() / tasks_completed.load()) / 1000.0 << "s" << std::endl;
        }
        double avg_idle = (total_idle_time_ms.load() / (double)initial_threads_count) / 1000.0;
        std::cout << "Avg Worker Wait Time (Idle): " << avg_idle << "s" << std::endl;
    }

private:
    void join_all_workers() {
        for (auto &w : workers) if (w.joinable()) w.join();
        workers.clear();
    }

    void transfer_stale_tasks() {
        std::vector<Task> stay;
        std::vector<Task> move;
        {
            std::lock_guard<std::mutex> lock(mtx);
            auto now = std::chrono::steady_clock::now();
            while (!queue1.empty()) {
                Task t = queue1.top();
                queue1.pop();
                auto wait_sec = std::chrono::duration_cast<std::chrono::seconds>(now - t.arrival_time).count();
                if (wait_sec > (t.duration_sec * 2)) move.push_back(t);
                else stay.push_back(t);
            }
            for (auto& t : stay) queue1.push(t);
            for (auto& t : move) {
                queue2.push(t);
                tasks_transferred++;
                safe_log("[Monitor] Task " + std::to_string(t.id) + " moved to Q2 (Wait timeout)");
            }
        }
        if (!move.empty()) cv.notify_all();
    }

    void worker_routine(int pref_q) {
        while (true) {
            Task task;
            {
                std::unique_lock<std::mutex> lock(mtx);
                auto wait_start = std::chrono::steady_clock::now();
                cv.wait(lock, [this] {
                    return stop || abort_requested || (!paused && (!queue1.empty() || !queue2.empty()));
                });
                total_idle_time_ms += std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - wait_start).count();
                if (abort_requested) return;
                if (stop && queue1.empty() && queue2.empty()) return;

                bool found = false;
                if (pref_q == 1) {
                    if (!queue1.empty()) { task = queue1.top(); queue1.pop(); found = true; }
                    else if (!queue2.empty()) { task = queue2.front(); queue2.pop(); found = true; }
                } else {
                    if (!queue2.empty()) { task = queue2.front(); queue2.pop(); found = true; }
                    else if (!queue1.empty()) { task = queue1.top(); queue1.pop(); found = true; }
                }
                if (!found) { if (stop) return; continue; }
            }
            safe_log("[Worker Q" + std::to_string(pref_q) + "] Starting Task " + std::to_string(task.id) + " (" + std::to_string(task.duration_sec) + "s)");
            auto exec_start = std::chrono::steady_clock::now();
            task.func(abort_requested); 
            if (!abort_requested) {
                total_exec_time_ms += std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - exec_start).count();
                tasks_completed++;
                safe_log("[Worker Q" + std::to_string(pref_q) + "] Finished Task " + std::to_string(task.id));
            }
        }
    }
};

int main() {
    srand(static_cast<unsigned>(time(NULL)));
    ThreadPool pool(3, 1);

    auto producer = [&](int start_id, int count) {
        for (int i = start_id; i < start_id + count; ++i) {
            int random_duration = 5 + rand() % 6;
            pool.add_task(i, random_duration);
            
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    };

    // Запускаємо два потоки-продюсери (разом 40 задач)
    std::thread p1(producer, 1, 20);
    std::thread p2(producer, 100, 20);
    p1.join(); p2.join();

    pool.safe_log("[Main] All tasks added. Waiting for congestion...");

    // Чекаємо 25 секунд, за цей час перші задачі почнуть виконуватися, 
    std::this_thread::sleep_for(std::chrono::seconds(25));

    // Демонстрація паузи
    pool.toggle_pause();
    std::this_thread::sleep_for(std::chrono::seconds(5));
    pool.toggle_pause();
    
    std::this_thread::sleep_for(std::chrono::seconds(5));

    pool.safe_log("[Main] Triggering abort...");
    pool.abort(); 

    pool.print_statistics();
    return 0;
}
