#include <gtest/gtest.h>
#include "vds_access/vds_reader.hpp"
#include <memory>
#include <thread>
#include <chrono>

using namespace remoteview;

class PrefetchCancellationTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create a basic config for testing
        config_ = std::make_shared<Config>();
        config_->set_data_dir(".");
        config_->set_vds_file("test.vds"); // Non-existent file is OK for this test
        
        vds_reader_ = std::make_unique<VdsReader>(config_);
        // Note: We won't initialize() since we don't have a real VDS file
        // We're testing the cancellation logic, not the VDS reading
    }
    
    std::shared_ptr<Config> config_;
    std::unique_ptr<VdsReader> vds_reader_;
};

TEST_F(PrefetchCancellationTest, SliceChangeGenerationIncrement) {
    // Test that slice changes affect cancellation behavior
    vds_reader_->set_current_slice(10, 20, 30);
    
    // Simulate getting generation at start of request
    uint64_t gen1 = 0; // We'll use 0 as old generation
    
    // Initially should not cancel (no generation mismatch)
    EXPECT_FALSE(vds_reader_->should_cancel_request(gen1 + 1000)); // Future generation
    
    // Change slice multiple times
    vds_reader_->set_current_slice(15, 25, 35);
    vds_reader_->set_current_slice(20, 30, 40);
    
    // Old generation requests should now be cancelled
    EXPECT_TRUE(vds_reader_->should_cancel_request(gen1));
}

TEST_F(PrefetchCancellationTest, ShouldCancelRequest) {
    // Test cancellation logic with generation numbers
    uint64_t old_generation = 100;
    uint64_t current_generation = 105;
    
    // Older generation should be cancelled
    EXPECT_TRUE(vds_reader_->should_cancel_request(old_generation));
    
    // Much newer generation should not be cancelled (future request)
    EXPECT_FALSE(vds_reader_->should_cancel_request(current_generation + 1000));
}

TEST_F(PrefetchCancellationTest, CancelPrefetchRequests) {
    // Test that cancel_prefetch_requests() works without crashing
    // We can't verify internal state, but we can ensure it doesn't crash
    
    vds_reader_->cancel_prefetch_requests();
    
    // Should not crash and method should return successfully
    EXPECT_TRUE(true); // Just verify we got here
}

TEST_F(PrefetchCancellationTest, SliceIndicesTracking) {
    // Test that slice change operations work without error
    // We can't access private members but we can test the public interface
    
    vds_reader_->set_current_slice(50, 75, 100);
    vds_reader_->set_current_slice(51, 76, 101);
    
    // Just verify the operations complete without throwing
    EXPECT_TRUE(true);
}

TEST_F(PrefetchCancellationTest, ConcurrentSliceChanges) {
    // Test thread safety of slice changes
    const int NUM_THREADS = 4;
    const int CHANGES_PER_THREAD = 10;
    
    std::vector<std::thread> threads;
    std::atomic<int> completed_changes{0};
    
    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([this, t, &completed_changes]() {
            for (int i = 0; i < CHANGES_PER_THREAD; ++i) {
                uint32_t inline_idx = t * 100 + i;
                uint32_t xline_idx = t * 100 + i + 1;  
                uint32_t z_idx = t * 100 + i + 2;
                
                vds_reader_->set_current_slice(inline_idx, xline_idx, z_idx);
                completed_changes.fetch_add(1);
                
                // Small delay to increase chance of contention
                std::this_thread::sleep_for(std::chrono::microseconds(1));
            }
        });
    }
    
    // Wait for all threads
    for (auto& thread : threads) {
        thread.join();
    }
    
    // All slice changes should have completed without crashes
    EXPECT_EQ(completed_changes.load(), NUM_THREADS * CHANGES_PER_THREAD);
}