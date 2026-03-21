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

struct Task {
    std::function<void()> func;
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
    std::condition_variable cv;
    
    std::atomic<bool> stop{false};
    std::atomic<bool> paused{false};

public:
    ThreadPool (int threads_q1, int threads_q2) {
        for (int i = 0; i < threads_q1; ++i) {
            workers.emplace_back([this] { worker_routine(1); });
        }
        for (int i = 0; i < threads_q2; ++i) {
            workers.emplace_back([this] { worker_routine(2); });
        }
        
        // Потік для моніторингу часу очікування та перенесення задач 
        std::thread([this] {
            while (!stop) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                transfer_stale_tasks();
            }
        }).detach();
    }

    ~ThreadPool () {
        terminate();
    }

    void add_task(int id, int duration) {
        std::unique_lock<std::mutex> lock(mtx);
        Task t{
            [id, duration] {
                std::cout << "[Task " << id << "] Running for " << duration << "s...\n";
                std::this_thread::sleep_for(std::chrono::seconds(duration));
            },
            duration,
            std::chrono::steady_clock::now(),
            id
        };
        queue1.push(t);
        cv.notify_all();
    }

    void toggle_pause() { paused = !paused; cv.notify_all(); }

    void terminate() {
        stop = true;
        cv.notify_all();
        for (std::thread &worker : workers) {
            if (worker.joinable()) worker.join();
        }
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
            // Якщо очікування > 2 * час виконання, то переносимо в другу чергу 
            if (wait_time > (t.duration_sec * 2)) {
                std::cout << "!!! Task " << t.id << " moved to Queue 2 (waited " << wait_time << "s)\n";
                queue2.push(t);
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
                cv.wait(lock, [this, queue_id] {
                    if (stop) return true;
                    if (paused) return false;
                    return (queue_id == 1 && !queue1.empty()) || (queue_id == 2 && !queue2.empty());
                });

                if (stop && queue1.empty() && queue2.empty()) return;

                if (queue_id == 1 && !queue1.empty()) {
                    task = queue1.top();
                    queue1.pop();
                } else if (queue_id == 2 && !queue2.empty()) {
                    task = queue2.front();
                    queue2.pop();
                } else continue;
            }
            task.func();
            std::cout << "Done Task " << task.id << " by Worker Q" << queue_id << "\n";
        }
    }
};

int main() {
    ThreadPool pool(3, 1);

    std::thread producer([&pool] {
        for (int i = 1; i <= 10; ++i) {
            int duration = 5 + rand() % 6; // Від 5 до 10 сек 
            std::cout << "Adding Task " << i << " (Dur: " << duration << "s)\n";
            pool.add_task(i, duration);
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    });

    producer.join();
    std::this_thread::sleep_for(std::chrono::seconds(40));
    return 0;
}
