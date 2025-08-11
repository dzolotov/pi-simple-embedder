// Test program for dht22_threaded component
#include <stdio.h>
#include <unistd.h>
#include <signal.h>

// External C interface from dht22_threaded.cc
extern "C" {
    bool dht22_init();
    bool dht22_read(float* temperature, float* humidity);
    void dht22_cleanup();
    void dht22_request_update();
    bool dht22_is_running();
    int dht22_get_data_age_ms();
}

volatile bool running = true;

void signal_handler(int sig) {
    printf("\nReceived signal %d, shutting down...\n", sig);
    running = false;
}

int main() {
    printf("=== DHT22 Threaded Component Test ===\n\n");
    
    // Setup signal handler
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // Initialize DHT22 reader
    printf("Initializing DHT22 threaded reader...\n");
    if (!dht22_init()) {
        printf("Failed to initialize DHT22 reader\n");
        return 1;
    }
    
    printf("DHT22 initialized successfully\n");
    printf("Reading sensor every 2 seconds (Ctrl+C to stop)...\n\n");
    
    int read_count = 0;
    int success_count = 0;
    
    while (running) {
        float temperature = 0;
        float humidity = 0;
        
        read_count++;
        
        if (dht22_read(&temperature, &humidity)) {
            success_count++;
            int age_ms = dht22_get_data_age_ms();
            printf("[%d] SUCCESS: T=%.1f°C, H=%.1f%% (age=%dms)\n", 
                   read_count, temperature, humidity, age_ms);
        } else {
            printf("[%d] FAILED: No valid data\n", read_count);
        }
        
        // Check if reader is still running
        if (!dht22_is_running()) {
            printf("WARNING: Reader thread is not running!\n");
            break;
        }
        
        // Request update every 5th read
        if (read_count % 5 == 0) {
            printf("Requesting immediate update...\n");
            dht22_request_update();
        }
        
        sleep(2);
    }
    
    printf("\n=== Test Summary ===\n");
    printf("Total reads: %d\n", read_count);
    printf("Successful: %d (%.1f%%)\n", success_count, 
           read_count > 0 ? (100.0f * success_count / read_count) : 0.0f);
    
    printf("\nCleaning up...\n");
    dht22_cleanup();
    
    printf("Test complete\n");
    return 0;
}