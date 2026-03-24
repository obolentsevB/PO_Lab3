#include <iostream>
#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <chrono>
#include <algorithm>
#include <atomic>
#include <iomanip>
#include <ctime>

struct Task {
    std::function<void(const std::atomic<bool>&)> func; 
    int duration_sec; 
    std::chrono::steady_clock::time_point arrival_time;
    int id;

    // Коротші задачі мають вищий пріоритет (SJF)
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
    std::condition_variable cv;
    
    std::atomic<bool> stop{false};
    std::atomic<bool> abort_requested{false};
    std::atomic<bool> paused{false};

    // Метрики
    std::atomic<long long> total_idle_time_ms{0};
    std::atomic<long long> total_exec_time_ms{0};
    std::atomic<int> tasks_completed{0};
    std::atomic<int> tasks_transferred{0};
    
    std::vector<size_t> q1_sizes;
    std::vector<size_t> q2_sizes;
    std::thread monitor_thread;

public:
    ThreadPool(int threads_q1, int threads_q2) {
        // Створення воркерів з прив'язкою до основної черги
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
        if (!stop && !abort_requested) {
            terminate();
        }
        if (monitor_thread.joinable()) {
            monitor_thread.join();
        }
    }

    // Інтерфейс додавання задач
    void add_task(int id, int duration) {
        std::unique_lock<std::mutex> lock(mtx);
        if (stop || abort_requested) return;

        auto task_logic = [id, duration](const std::atomic<bool>& cancelled) {
            for (int s = 0; s < duration; ++s) {
                if (cancelled) return; // Миттєвий вихід при abort
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        };

        queue1.push({task_logic, duration, std::chrono::steady_clock::now(), id});
        cv.notify_all();
    }

    void toggle_pause() { 
        paused = !paused; 
        if (!paused) cv.notify_all(); 
        std::cout << (paused ? "\n[System] POOL PAUSED" : "\n[System] POOL RESUMED") << std::endl;
    }

    // М'яка зупинка: дочекатися завершення черг
    void terminate() {
        stop = true;
        cv.notify_all();
        join_all_workers();
    }

    // Жорстка зупинка: очистити черги та перервати активні задачі
    void abort() {
        abort_requested = true;
        {
            std::lock_guard<std::mutex> lock(mtx);
            while(!queue1.empty()) queue1.pop();
            while(!queue2.empty()) queue2.pop();
        }
        std::cout << "\n[System] ABORTING (clearing queues & signals sent)..." << std::endl;
        cv.notify_all();
        join_all_workers();
    }

    void print_statistics() {
        std::lock_guard<std::mutex> lock(mtx);
        double avg_q1 = 0, avg_q2 = 0;
        for (auto s : q1_sizes) avg_q1 += s;
        for (auto s : q2_sizes) avg_q2 += s;
        
        std::cout << "Workers: " << workers.size() << std::endl;
        std::cout << "Completed: " << tasks_completed.load() << std::endl;
        std::cout << "Moved to Q2: " << tasks_transferred.load() << std::endl;
        if (!q1_sizes.empty()) {
            std::cout << "Avg Q1 Length: " << std::fixed << std::setprecision(2) << avg_q1 / q1_sizes.size() << std::endl;
            std::cout << "Avg Q2 Length: " << avg_q2 / q2_sizes.size() << std::endl;
        }
        if (tasks_completed > 0) {
            std::cout << "Avg Exec Time: " << (total_exec_time_ms.load() / tasks_completed.load()) / 1000.0 << "s" << std::endl;
        }
    }

private:
    void join_all_workers() {
        for (auto &w : workers) {
            if (w.joinable()) w.join();
        }
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
                auto wait = std::chrono::duration_cast<std::chrono::seconds>(now - t.arrival_time).count();
                
                if (wait > (t.duration_sec * 2)) move.push_back(t);
                else stay.push_back(t);
            }
            for (auto& t : stay) queue1.push(t);
            for (auto& t : move) {
                queue2.push(t);
                tasks_transferred++;
                std::cout << "[Monitor] Task " << t.id << " moved to Q2 (Wait timeout)\n";
            }
        }
        if (!move.empty()) cv.notify_all();
    }

    void worker_routine(int pref_q) {
        while (true) {
            Task task;
            {
                std::unique_lock<std::mutex> lock(mtx);
                auto start_wait = std::chrono::steady_clock::now();
                
                cv.wait(lock, [this] {
                    bool has_work = !queue1.empty() || !queue2.empty();
                    return stop || abort_requested || (!paused && has_work);
                });

                total_idle_time_ms += std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_wait).count();

                if (abort_requested) return;
                if (stop && queue1.empty() && queue2.empty()) return;

                // Fallback логіка: спочатку своя черга, якщо порожня — інша
                if (pref_q == 1) {
                    if (!queue1.empty()) { task = queue1.top(); queue1.pop(); }
                    else if (!queue2.empty()) { task = queue2.front(); queue2.pop(); }
                    else continue;
                } else {
                    if (!queue2.empty()) { task = queue2.front(); queue2.pop(); }
                    else if (!queue1.empty()) { task = queue1.top(); queue1.pop(); }
                    else continue;
                }
            }

            auto start_exec = std::chrono::steady_clock::now();
            std::cout << "[Worker Q" << pref_q << "] Running Task " << task.id << " (" << task.duration_sec << "s)\n";
            
            task.func(abort_requested); // Виконання з перевіркою на abort
            
            total_exec_time_ms += std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_exec).count();
            if (!abort_requested) tasks_completed++;
        }
    }
};

int main() {
    srand(static_cast<unsigned>(time(0)));
    ThreadPool pool(3, 1);

    auto producer = [&](int start_id) {
        for (int i = start_id; i < start_id + 5; ++i) {
            pool.add_task(i, 5 + rand() % 6);
            std::this_thread::sleep_for(std::chrono::milliseconds(800));
        }
    };

    std::thread p1(producer, 1);
    std::thread p2(producer, 10);
    p1.join(); p2.join();

    std::this_thread::sleep_for(std::chrono::seconds(5));
    pool.toggle_pause();
    std::this_thread::sleep_for(std::chrono::seconds(3));
    pool.toggle_pause();
    
    std::cout << "[Main] Shutting down pool..." << std::endl;
    pool.abort(); 

    pool.print_statistics();
    return 0;
}
