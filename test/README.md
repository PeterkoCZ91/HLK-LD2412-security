# LD2412 Tests

## Structure

```
test/
└── test_native/           # Native tests (run on PC)
    ├── ArduinoMock.h      # Minimal Arduino mocks
    └── test_parser.cpp    # UART parser unit tests
```

## Running Tests

```bash
# Run all native tests (on PC, no hardware needed!)
pio test -e test_native

# Verbose output
pio test -e test_native -v
```

## Test Coverage

### Ring Buffer Tests
- `test_ring_buffer_push_pop` - Basic push/pop operations
- `test_ring_buffer_wrap_around` - Circular buffer wrap behavior
- `test_ring_buffer_overflow` - Full buffer handling

### Frame Parser Tests
- `test_valid_frame_parsing` - Parse valid LD2412 data frame
- `test_garbage_before_frame` - Noise resilience, find sync
- `test_corrupted_footer` - Invalid footer detection
- `test_invalid_length` - Invalid length field handling
- `test_fragmented_frame` - Partial packet reassembly
- `test_multiple_frames` - Sequential frame processing

## Adding New Tests

1. Create test file in `test/test_native/`
2. Use Unity framework (`#include <unity.h>`)
3. Implement `main()` with `UNITY_BEGIN()` / `UNITY_END()`
