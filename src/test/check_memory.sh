#!/bin/bash

# Logger 内存泄漏检测脚本

set -e

echo "================================="
echo "  Logger Memory Leak Detection"
echo "================================="
echo ""

# 编译测试程序
echo "Building test..."
xmake build test_log

TEST_BIN="./build/linux/x86_64/debug/test_log"

if [ ! -f "$TEST_BIN" ]; then
    echo "Error: Test binary not found at $TEST_BIN"
    exit 1
fi

echo ""
echo "1. Running built-in memory leak test..."
echo "   (Monitors RSS memory over multiple iterations)"
echo ""
$TEST_BIN --stress

echo ""
echo "================================="
echo ""

# 检查是否安装了valgrind
if command -v valgrind &> /dev/null; then
    echo "2. Running Valgrind memory leak detection..."
    echo "   (This may take a few minutes...)"
    echo ""
    
    # 运行基础测试套件，不运行压力测试（太慢）
    valgrind --leak-check=full \
             --show-leak-kinds=all \
             --track-origins=yes \
             --verbose \
             --log-file=valgrind_report.txt \
             $TEST_BIN
    
    echo ""
    echo "Valgrind report saved to: valgrind_report.txt"
    echo ""
    echo "=== Valgrind Summary ==="
    grep -A 10 "LEAK SUMMARY" valgrind_report.txt || echo "No leak summary found"
    grep -A 5 "ERROR SUMMARY" valgrind_report.txt || echo "No error summary found"
else
    echo "2. Valgrind not found - skipping Valgrind test"
    echo "   Install with: sudo apt-get install valgrind"
fi

echo ""
echo "================================="
echo "  Memory Check Completed"
echo "================================="
echo ""
echo "Tips:"
echo "  - View full report: cat valgrind_report.txt"
echo "  - Check test logs: ls -lh test_logs/"
echo "  - Run stress test only: $TEST_BIN --stress"
echo ""
