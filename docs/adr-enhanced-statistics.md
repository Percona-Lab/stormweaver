# ADR: Enhanced Statistics System for StormWeaver

## Status
Accepted

## Context
StormWeaver currently provides minimal per-worker statistics, only tracking the total number of successful and failed actions. This limited visibility makes it difficult to:
- Identify which specific action types are causing failures
- Distinguish between different types of errors (logic errors vs SQL errors)
- Measure performance characteristics like execution times
- Monitor system behavior during long-running tests

## Decision
Implement a comprehensive worker-level statistics system that provides detailed metrics collection and reporting.

## Overview of Functionality
Create an enhanced statistics system that replaces the current minimal success/failure counters with detailed per-action-type metrics, comprehensive error categorization, and timing measurements at the worker level.

### Files to be Created
- `libstormweaver/include/statistics.hpp` - Statistics data structures and interfaces
- `libstormweaver/src/statistics.cpp` - Statistics implementation
- `libstormweaver/test/unit/statistics_test.cpp` - Unit tests for statistics system

### Files to be Modified
- `libstormweaver/include/workload.hpp` - Add enhanced statistics structures
- `libstormweaver/src/workload.cpp` - Implement statistics collection in workers
- `libstormweaver/include/action/action.hpp` - Add timing and error code interfaces
- `libstormweaver/src/action/action.cpp` - Implement error codes and timing
- `libstormweaver/include/action/action_registry.hpp` - Enhance exception classes
- `libstormweaver/src/action/action_registry.cpp` - Add error code handling
- `libstormweaver/include/sql_variant/generic.hpp` - Add error codes to SqlException
- `libstormweaver/src/sql_variant/generic.cpp` - Implement error code mapping

## Design Decisions

### 1. Data Structure Design
- **Map-based storage**: Use `std::unordered_map<std::string, ActionStatistics>` with ActionFactory names as keys
- **Worker-level only**: No thread safety concerns since workers are single-threaded
- **Simple design**: Statistics are local to each worker and reported independently

### 2. Error Categorization
- **Three exception categories**: ActionException, SqlException, and Other (generic std::exception)
- **String-based error identifiers**: SQL error codes as strings, ActionException error names as descriptive strings
- **Separate tracking**: Maintain separate maps for SQL error codes vs ActionException error names
- **No backward compatibility**: Update all exception throwing sites to include error codes/names

### 3. Timing Measurements
- **Dual timing**: Measure both overall action execution time and SQL server query time
- **High precision**: Use `std::chrono::high_resolution_clock` for nanosecond precision
- **Simple aggregation**: Track min, max, mean for each action type

### 4. Performance Considerations
- **Minimal overhead**: Use efficient containers and minimize string allocations
- **Single-threaded**: No locking or atomic operations needed
- **Local collection**: All statistics operations are local to worker thread

## Architecture

### Core Components

```cpp
// Per-action statistics within a worker
struct ActionStatistics {
    uint64_t successCount = 0;
    uint64_t actionFailureCount = 0;     // ActionException
    uint64_t sqlFailureCount = 0;        // SqlException
    uint64_t otherFailureCount = 0;      // Any other exception
    
    // Error categorization (separate maps for different error types)
    std::map<std::string, uint64_t> actionErrorNames;  // ActionException error names
    std::map<std::string, uint64_t> sqlErrorCodes;     // SQL error codes
    
    // Timing statistics
    uint64_t totalExecutionTimeNs = 0;
    uint64_t totalSqlTimeNs = 0;
    uint64_t minExecutionTimeNs = UINT64_MAX;
    uint64_t maxExecutionTimeNs = 0;
    uint64_t minSqlTimeNs = UINT64_MAX;
    uint64_t maxSqlTimeNs = 0;
};

// Worker-level statistics (replaces current simple counters)
struct WorkerStatistics {
    std::unordered_map<std::string, ActionStatistics> actionStats;
    std::chrono::steady_clock::time_point startTime;
    std::chrono::steady_clock::time_point endTime;
    
    // Methods for updating and reporting
    void recordSuccess(const std::string& actionName, uint64_t execTimeNs, uint64_t sqlTimeNs);
    void recordActionFailure(const std::string& actionName, const std::string& errorName, uint64_t execTimeNs);
    void recordSqlFailure(const std::string& actionName, const std::string& errorCode, uint64_t execTimeNs);
    void recordOtherFailure(const std::string& actionName, uint64_t execTimeNs);
    void reset();
    void report() const;
};
```

### Integration Points

1. **Worker Integration**: Replace simple counters in `RandomWorker` with `WorkerStatistics`
2. **Action Integration**: Add timing wrapper around action execution
3. **Exception Integration**: Enhance exception classes with error codes/names
4. **Reporting Integration**: Update existing worker reporting to use detailed statistics

## Challenges Encountered

### 1. Exception Handling Complexity
**Challenge**: Actions can throw any type of exception, not just ActionException or SqlException
**Solution**: Implement exception categorization with three categories: ActionException, SqlException, and other generic exceptions

### 2. Error Code Consistency
**Challenge**: Different error types need different identification schemes
**Solution**: Use separate maps for ActionException error names and SQL error codes, simple count for other exceptions

### 3. Breaking Changes
**Challenge**: Updating all exception throwing sites without backward compatibility
**Solution**: Systematically update all SqlException and ActionException throwing sites with appropriate error identifiers

## Solutions Implemented

### 1. Simplified Exception Categorization
- ActionException with descriptive error names (e.g., "invalid_table", "constraint_violation")
- SqlException with string error codes from SQL server or hardcoded strings
- Other exceptions tracked with simple count (no error categorization)

### 2. String-Based Error Identifiers
- Keep SQL error codes as strings (matching current implementation)
- Use descriptive string names for ActionException errors
- Simple count for other exception types

### 3. Systematic Exception Updates
- Update all SqlException throw sites to include error codes
- Update all ActionException throw sites to include error names
- No backward compatibility concerns since these are internal APIs

### 4. RAII Timing
Implement timing measurement using RAII pattern to ensure accurate measurement even with exceptions

## Future Considerations

### 1. Export Formats
- Add JSON and CSV export capabilities for external analysis
- Consider structured logging for statistics data

### 2. Cross-Worker Analysis
- If needed in the future, add utilities to aggregate statistics across workers
- Consider adding worker identification for correlation

### 3. Advanced Metrics
- Add percentile calculations (P50, P95, P99) if needed
- Consider adding resource utilization metrics

## Implementation Notes

### Error Code Enhancements
- `ActionException`: Add constructor accepting error name (string)
- `SqlException`: Add constructor accepting error code (string)
- Update all throwing sites to provide appropriate error identifiers

### Timing Implementation
- Wrap action execution with timing measurements
- Separate SQL execution time from overall action time
- Use high-resolution clocks for accurate measurements

### Statistics Collection
- Replace current `successfulActions` and `failedActions` counters in `RandomWorker`
- Implement statistics collection at the same points where current counters are updated
- Add reporting method that outputs detailed statistics instead of simple totals

This design provides comprehensive error categorization for known exception types while maintaining simplicity for generic exceptions and avoiding thread safety complexity.