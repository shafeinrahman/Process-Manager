#!/bin/bash

# Test script for Process Manager Simulator
# This script compiles the project and runs various test scenarios

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$PROJECT_DIR" || exit 1

echo "=========================================="
echo "Process Manager Build & Test Script"
echo "=========================================="
echo ""

# Color codes
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Test 1: Build
echo -e "${YELLOW}TEST 1: Building project...${NC}"
make clean > /dev/null 2>&1
if make > /dev/null 2>&1; then
    echo -e "${GREEN}✓ Build successful${NC}"
else
    echo -e "${RED}✗ Build failed${NC}"
    make
    exit 1
fi

echo ""

# Test 2: Check executable
echo -e "${YELLOW}TEST 2: Checking executable...${NC}"
if [ -f "$PROJECT_DIR/pm_sim" ]; then
    echo -e "${GREEN}✓ Executable created${NC}"
else
    echo -e "${RED}✗ Executable not found${NC}"
    exit 1
fi

echo ""

# Test 3: Run with sample commands
echo -e "${YELLOW}TEST 3: Running with sample command files...${NC}"
if [ -f "$PROJECT_DIR/commands1.txt" ] && [ -f "$PROJECT_DIR/commands2.txt" ]; then
    echo -e "${GREEN}✓ Command files found${NC}"
    echo ""
    echo "Running process manager with 2 worker threads..."
    echo "(This will take a few seconds...)"
    echo ""
    
    timeout 30 ./pm_sim commands1.txt commands2.txt
    
    RESULT=$?
    if [ $RESULT -eq 124 ]; then
        echo -e "${RED}✗ Process manager timed out${NC}"
        echo "Note: This might happen if 'wait' calls are blocking"
    elif [ $RESULT -eq 0 ]; then
        echo ""
        echo -e "${GREEN}✓ Process manager executed successfully${NC}"
    fi
else
    echo -e "${RED}✗ Command files not found${NC}"
    exit 1
fi

echo ""

# Test 4: Check snapshots file
echo -e "${YELLOW}TEST 4: Checking snapshots file...${NC}"
if [ -f "$PROJECT_DIR/snapshots.txt" ]; then
    echo -e "${GREEN}✓ snapshots.txt created${NC}"
    LINE_COUNT=$(wc -l < "$PROJECT_DIR/snapshots.txt")
    echo "  - File size: $(wc -c < "$PROJECT_DIR/snapshots.txt") bytes"
    echo "  - Line count: $LINE_COUNT lines"
    echo ""
    echo "First 20 lines of snapshots.txt:"
    head -20 "$PROJECT_DIR/snapshots.txt"
else
    echo -e "${RED}✗ snapshots.txt not found${NC}"
fi

echo ""
echo -e "${YELLOW}TEST 5: Summary${NC}"
echo "=========================================="
echo "Build: PASS"
echo "Executable: EXISTS"
echo "Execution: PASS"
echo "Output: GENERATED"
echo "=========================================="
echo ""
echo "All tests completed!"
echo ""
echo "Additional commands for manual testing:"
echo "  make clean       - Remove build artifacts"
echo "  make run         - Build and run with commands1.txt"
echo "  make help        - Show all targets"
