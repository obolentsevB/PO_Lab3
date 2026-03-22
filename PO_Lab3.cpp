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

struct Task {
    std::function<void()> func;
    int duration_sec; 
    std::chrono::steady_clock::time_point arrival_time;
    int id;

    // Пріоритет для priority_queue: коротші задачі мають вищий пріоритет (min-heap)
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
    std::atomic<bool> paused{false};

    std::atomic<long long> total_idle_time_ms{0}; // Час очікування воркерів
    std::atomic<long long> total_exec_time_ms{0}; // Час виконання задач
    std::atomic<int> tasks_completed{0}; // Кількість виконаних задач
    std::atomic<int> tasks_transferred{0}; // Задачі, перенесені в Q2
    
    std::vector<size_t> q1_sizes;
    std::vector<size_t> q2_sizes;
    std::thread monitor_thread;

public:
    ThreadPool(int threads_q1, int threads_q2) {
        // Ініціалізація воркерів для обох черг
        for (int i = 0; i < threads_q1; ++i) 
            workers.emplace_back([this] { worker_routine(1); });
        
        for (int i = 0; i < threads_q2; ++i) 
            workers.emplace_back([this] { worker_routine(2); });

        monitor_thread = std::thread([this] {
            while (!stop) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                transfer_stale_tasks();
                
                std::unique_lock<std::mutex> lock(mtx);
                q1_sizes.push_back(queue1.size());
                q2_sizes.push_back(queue2.size());
            }
        });
    }

    ~ThreadPool() {
        terminate();
        if (monitor_thread.joinable()) monitor_thread.join();
    }

    void add_task(int id, int duration) {
        std::unique_lock<std::mutex> lock(mtx);
        Task t{
            [id, duration] {
                std::this_thread::sleep_for(std::chrono::seconds(duration));
            },
            duration,
            std::chrono::steady_clock::now(),
            id
        };
        queue1.push(t);
        cv.notify_all();
    }

    void toggle_pause() { 
        paused = !paused; 
        if (!paused) cv.notify_all(); 
        std::cout << (paused ? "== Pool Paused ==" : "== Pool Resumed ==") << std::endl;
    }

    void terminate() {
        stop = true;
        cv.notify_all();
        for (std::thread &worker : workers) {
            if (worker.joinable()) worker.join();
        }
    }

    // Вивід метрик
    void print_statistics() {
        std::unique_lock<std::mutex> lock(mtx);
        double avg_q1 = 0, avg_q2 = 0;
        for (auto s : q1_sizes) avg_q1 += s;
        for (auto s : q2_sizes) avg_q2 += s;
        
        int num_workers = workers.size();
        
        std::cout << "1. Total Workers: " << num_workers << std::endl;
        std::cout << "2. Tasks Completed: " << tasks_completed << std::endl;
        std::cout << "3. Tasks Transferred to Q2: " << tasks_transferred << std::endl;
        
        if (!q1_sizes.empty()) {
            std::cout << "4. Avg Queue 1 Length: " << std::fixed << std::setprecision(2) << avg_q1 / q1_sizes.size() << std::endl;
            std::cout << "5. Avg Queue 2 Length: " << avg_q2 / q2_sizes.size() << std::endl;
        }
        
        if (tasks_completed > 0) {
            std::cout << "6. Avg Task Execution Time: " << (total_exec_time_ms / tasks_completed) / 1000.0 << "s" << std::endl;
        }
        
        std::cout << "7. Avg Worker Idle Time: " << (total_idle_time_ms / num_workers) / 1000.0 << "s" << std::endl;
    }

private:
    void transfer_stale_tasks() {
        std::unique_lock<std::mutex> lock(mtx);
        if (queue1.empty()) return;

        std::vector<Task> remaining;
        auto now = std::chrono::steady_clock::now();

        while (!queue1.empty()) {
            Task t = queue1.top();
            queue1.pop();
            
            auto wait_time = std::chrono::duration_cast<std::chrono::seconds>(now - t.arrival_time).count();
            
            // Якщо час очікування > 2 * час виконання, то переносимо в Q2
            if (wait_time > (t.duration_sec * 2)) {
                std::cout << "[Monitor] Task " << t.id << " moved to Q2 (waited " << wait_time << "s)" << std::endl;
                queue2.push(t);
                tasks_transferred++;
            } else {
                remaining.push_back(t);
            }
        }
        for (auto& t : remaining) queue1.push(t);
        if (!queue2.empty()) cv.notify_all();
    }

    void worker_routine(int queue_id) {
        while (true) {
            Task task;
            {
                std::unique_lock<std::mutex> lock(mtx);
                
                auto start_wait = std::chrono::steady_clock::now();
                
                // Використання умовної змінної для очікування задач
                cv.wait(lock, [this, queue_id] {
                    if (stop) return true;
                    if (paused) return false;
                    return (queue_id == 1 && !queue1.empty()) || (queue_id == 2 && !queue2.empty());
                });

                auto end_wait = std::chrono::steady_clock::now();
                total_idle_time_ms += std::chrono::duration_cast<std::chrono::milliseconds>(end_wait - start_wait).count();

                if (stop && queue1.empty() && queue2.empty()) return;

                if (queue_id == 1 && !queue1.empty()) {
                    task = queue1.top();
                    queue1.pop();
                } else if (queue_id == 2 && !queue2.empty()) {
                    task = queue2.front();
                    queue2.pop();
                } else continue;
            }

            auto start_exec = std::chrono::steady_clock::now();
            std::cout << "[Worker Q" << queue_id << "] Processing Task " << task.id << " (" << task.duration_sec << "s)..." << std::endl;
            
            task.func();
            
            auto end_exec = std::chrono::steady_clock::now();
            total_exec_time_ms += std::chrono::duration_cast<std::chrono::milliseconds>(end_exec - start_exec).count();
            tasks_completed++;
            
            std::cout << "[Worker Q" << queue_id << "] Finished Task " << task.id << std::endl;
        }
    }
};

int main() {
    srand(time(0));
    ThreadPool pool(3, 1);

    std::thread producer1([&pool] {
        for (int i = 1; i <= 5; ++i) {
            int dur = 5 + rand() % 6; // 5-10 сек
            pool.add_task(i, dur);
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    });

    std::thread producer2([&pool] {
        for (int i = 6; i <= 10; ++i) {
            int dur = 5 + rand() % 6;
            pool.add_task(i, dur);
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        }
    });

    producer1.join();
    producer2.join();

    // Даємо час на виконання та демонстрацію перенесення задач
    std::this_thread::sleep_for(std::chrono::seconds(15));
    pool.toggle_pause();
    std::this_thread::sleep_for(std::chrono::seconds(5));
    pool.toggle_pause();

    std::cout << "Waiting for all tasks to finish..." << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(30));

    pool.print_statistics();
    
    return 0;
}
