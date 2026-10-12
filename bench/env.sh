#!/bin/sh
# Prints the machine and toolchain description that must accompany any number.
echo "date: $(date -u +%Y-%m-%dT%H:%MZ)"
echo "cpu: $(lscpu | sed -n 's/^Model name: *//p')"
echo "cpus visible: $(nproc), threads/core: $(lscpu | sed -n 's/^Thread(s) per core: *//p'), hypervisor: $(lscpu | sed -n 's/^Hypervisor vendor: *//p')"
echo "L1d/L2/L3: $(lscpu | sed -n 's/^L1d cache: *//p') / $(lscpu | sed -n 's/^L2 cache: *//p') / $(lscpu | sed -n 's/^L3 cache: *//p')"
echo "kernel: $(uname -sr)"
echo "governor: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo 'not exposed (VM)')"
echo "tsc flags: $(grep -o 'constant_tsc\|nonstop_tsc' /proc/cpuinfo | sort -u | tr '\n' ' ')"
echo "compiler: $(${CXX:-c++} --version | head -1)"
echo "load average: $(cut -d' ' -f1-3 /proc/loadavg)"
