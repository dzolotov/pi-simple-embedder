// Threaded DHT22 reader for Flutter embedder integration
// Runs sensor reading in background thread with thread-safe access

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <chrono>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <pthread.h>

// Configuration
#define PYTHON_SCRIPT_PATH "/home/dmitrii/flutter_pi_demo/read_am2302.py"
#define UPDATE_INTERVAL_MS 5000  // Update every 5 seconds
#define MIN_READ_INTERVAL_MS 2000

// Thread-safe sensor data
struct SensorData {
    float temperature;
    float humidity;
    bool valid;
    std::chrono::steady_clock::time_point timestamp;
};

class ThreadedSensorReader {
private:
    // Thread management
    std::thread worker_thread;
    std::atomic<bool> running;
    std::atomic<bool> initialized;
    
    // Data protection
    mutable std::mutex data_mutex;
    SensorData current_data;
    SensorData cached_data;
    
    // Synchronization
    std::condition_variable cv;
    std::mutex cv_mutex;
    
    // Statistics
    std::atomic<int> total_reads;
    std::atomic<int> successful_reads;
    std::atomic<int> failed_reads;
    
    // Worker thread function
    void worker_loop() {
        printf("DHT22: Worker thread started (PID=%d, TID=%ld)\n", 
               getpid(), (long)pthread_self());
        
        while (running.load()) {
            // Read sensor
            float temp = 0, hum = 0;
            bool success = read_sensor_internal(&temp, &hum);
            
            // Update data with lock
            {
                std::lock_guard<std::mutex> lock(data_mutex);
                if (success) {
                    current_data.temperature = temp;
                    current_data.humidity = hum;
                    current_data.valid = true;
                    current_data.timestamp = std::chrono::steady_clock::now();
                    
                    // Update cache
                    cached_data = current_data;
                    
                    successful_reads++;
                    printf("DHT22: [Thread] Updated: T=%.1f°C, H=%.1f%% (success #%d)\n", 
                           temp, hum, successful_reads.load());
                } else {
                    // Mark current data as stale after 30 seconds
                    auto now = std::chrono::steady_clock::now();
                    auto age = std::chrono::duration_cast<std::chrono::seconds>(
                        now - current_data.timestamp);
                    if (age.count() > 30) {
                        current_data.valid = false;
                    }
                    failed_reads++;
                    printf("DHT22: [Thread] Read failed (failure #%d)\n", failed_reads.load());
                }
                total_reads++;
            }
            
            // Wait for next update interval
            std::unique_lock<std::mutex> lock(cv_mutex);
            cv.wait_for(lock, std::chrono::milliseconds(UPDATE_INTERVAL_MS), 
                       [this] { return !running.load(); });
        }
        
        printf("DHT22: Worker thread stopped\n");
    }
    
    // Internal sensor reading (runs in worker thread)
    bool read_sensor_internal(float* temperature, float* humidity) {
        // Check Python script
        if (access(PYTHON_SCRIPT_PATH, X_OK) != 0) {
            printf("DHT22: [Thread] Script not accessible: %s\n", PYTHON_SCRIPT_PATH);
            return false;
        }
        
        printf("DHT22: [Thread] Executing script: %s\n", PYTHON_SCRIPT_PATH);
        
        // Run Python script with timeout and sudo for GPIO access
        char command[256];
        snprintf(command, sizeof(command), "sudo timeout 25 /usr/bin/python3 %s 2>&1", PYTHON_SCRIPT_PATH);
        
        FILE* pipe = popen(command, "r");
        if (!pipe) {
            printf("DHT22: [Thread] Failed to execute script\n");
            return false;
        }
        
        char buffer[128];
        bool success = false;
        
        if (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            printf("DHT22: [Thread] Script output: %s", buffer);
            if (strncmp(buffer, "ERROR", 5) != 0) {
                char* comma = strchr(buffer, ',');
                if (comma) {
                    *comma = '\0';
                    float temp = atof(buffer);
                    float hum = atof(comma + 1);
                    
                    printf("DHT22: [Thread] Parsed: temp=%.1f, hum=%.1f\n", temp, hum);
                    
                    if (temp > -40 && temp < 80 && hum >= 0 && hum <= 100) {
                        *temperature = temp;
                        *humidity = hum;
                        success = true;
                    }
                }
            }
        } else {
            printf("DHT22: [Thread] No output from script\n");
        }
        
        pclose(pipe);
        return success;
    }
    
public:
    ThreadedSensorReader() : 
        running(false),
        initialized(false),
        total_reads(0),
        successful_reads(0),
        failed_reads(0) {
        
        // Initialize with default values
        current_data = {25.0f, 50.0f, false, std::chrono::steady_clock::now()};
        cached_data = current_data;
    }
    
    ~ThreadedSensorReader() {
        stop();
    }
    
    bool start() {
        if (initialized.load()) {
            printf("DHT22: Already running\n");
            return true;
        }
        
        // Check script before starting thread
        if (access(PYTHON_SCRIPT_PATH, X_OK) != 0) {
            printf("DHT22: Python script not found: %s\n", PYTHON_SCRIPT_PATH);
            return false;
        }
        
        running = true;
        initialized = true;
        
        // Start worker thread
        worker_thread = std::thread(&ThreadedSensorReader::worker_loop, this);
        
        printf("DHT22: Threaded reader started\n");
        printf("DHT22: Update interval: %dms\n", UPDATE_INTERVAL_MS);
        return true;
    }
    
    void stop() {
        if (!initialized.load()) {
            return;
        }
        
        printf("DHT22: Stopping worker thread...\n");
        running = false;
        cv.notify_all();
        
        if (worker_thread.joinable()) {
            worker_thread.join();
        }
        
        initialized = false;
        printf("DHT22: Stopped (total reads: %d, success: %d, failed: %d)\n",
               total_reads.load(), successful_reads.load(), failed_reads.load());
    }
    
    // Get current sensor data (thread-safe)
    bool get_data(float* temperature, float* humidity) {
        std::lock_guard<std::mutex> lock(data_mutex);
        
        if (current_data.valid) {
            *temperature = current_data.temperature;
            *humidity = current_data.humidity;
            return true;
        } else if (cached_data.valid) {
            // Return cached data if current is invalid
            *temperature = cached_data.temperature;
            *humidity = cached_data.humidity;
            printf("DHT22: Returning cached data\n");
            return true;
        }
        
        return false;
    }
    
    // Get data age in milliseconds
    int get_data_age_ms() const {
        std::lock_guard<std::mutex> lock(data_mutex);
        auto now = std::chrono::steady_clock::now();
        auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - current_data.timestamp);
        return age.count();
    }
    
    // Force immediate update (non-blocking)
    void request_update() {
        cv.notify_all();
    }
    
    // Get statistics
    void get_stats(int* total, int* success, int* failed) const {
        *total = total_reads.load();
        *success = successful_reads.load();
        *failed = failed_reads.load();
    }
    
    bool is_running() const {
        return initialized.load() && running.load();
    }
};

// C interface for embedder
extern "C" {
    static ThreadedSensorReader* threaded_reader = nullptr;
    
    bool dht22_init() {
        static std::mutex init_mutex;
        std::lock_guard<std::mutex> lock(init_mutex);
        
        if (threaded_reader) {
            printf("DHT22: Already initialized, returning existing instance\n");
            return true;
        }
        
        printf("DHT22: Creating new threaded reader instance\n");
        threaded_reader = new ThreadedSensorReader();
        if (!threaded_reader->start()) {
            printf("DHT22: Failed to start threaded reader\n");
            delete threaded_reader;
            threaded_reader = nullptr;
            return false;
        }
        
        printf("DHT22: Initialization complete, background thread running\n");
        return true;
    }
    
    bool dht22_read(float* temperature, float* humidity) {
        if (!threaded_reader) {
            printf("DHT22: Not initialized\n");
            return false;
        }
        
        if (!temperature || !humidity) {
            printf("DHT22: NULL pointers\n");
            return false;
        }
        
        bool result = threaded_reader->get_data(temperature, humidity);
        
        if (result) {
            int age_ms = threaded_reader->get_data_age_ms();
            printf("DHT22: Returning data (age: %dms): T=%.1f°C, H=%.1f%%\n", 
                   age_ms, *temperature, *humidity);
        } else {
            printf("DHT22: No valid data available\n");
        }
        
        return result;
    }
    
    void dht22_cleanup() {
        if (threaded_reader) {
            int total, success, failed;
            threaded_reader->get_stats(&total, &success, &failed);
            
            printf("DHT22: Shutting down...\n");
            printf("DHT22: Statistics - Total: %d, Success: %d (%.1f%%), Failed: %d\n",
                   total, success, 
                   total > 0 ? (100.0f * success / total) : 0.0f,
                   failed);
            
            delete threaded_reader;
            threaded_reader = nullptr;
            printf("DHT22: Cleanup complete\n");
        }
    }
    
    // Additional functions for embedder
    void dht22_request_update() {
        if (threaded_reader) {
            threaded_reader->request_update();
            printf("DHT22: Update requested\n");
        }
    }
    
    bool dht22_is_running() {
        return threaded_reader && threaded_reader->is_running();
    }
    
    int dht22_get_data_age_ms() {
        if (threaded_reader) {
            return threaded_reader->get_data_age_ms();
        }
        return -1;
    }
}