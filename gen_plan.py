#!/usr/bin/env python3
import random
import sys
n = int(sys.argv[1])
random.seed(int(sys.argv[2]) if len(sys.argv) > 2 else 1)
max_ms = int(sys.argv[3]) if len(sys.argv) > 3 else 5
p_vacio = float(sys.argv[4]) if len(sys.argv) > 4 else 0.0
for i in range(1, n + 1):
    deps = sorted(random.sample(range(1, i), min(i - 1, random.randint(0, 3))))
    tiempo = "" if random.random() < p_vacio else str(random.randint(1, max_ms))
    print(f"{i}: act_{i}: {tiempo}: {', '.join(map(str, deps))}")
