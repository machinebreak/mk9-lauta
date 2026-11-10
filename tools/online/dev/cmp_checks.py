"""The first line where two check logs (mk9_check_log) differ, with the lines around it.

    python cmp_checks.py <a> <b> [context]
"""
import sys

a = open(sys.argv[1]).read().splitlines()
b = open(sys.argv[2]).read().splitlines()
ctx = int(sys.argv[3]) if len(sys.argv) > 3 else 3
print(len(a), 'lines vs', len(b))
for i, (x, y) in enumerate(zip(a, b)):
    if x != y:
        print('first difference at line', i)
        for j in range(max(0, i - ctx), min(i + ctx + 1, max(len(a), len(b)))):
            print('A:', a[j][:200] if j < len(a) else '')
            print('B:', b[j][:200] if j < len(b) else '')
        break
else:
    print('no difference in the common part')
