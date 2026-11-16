# То же на Python (1 млн чисел вместо 10 млн): время ×10
nums = [(i % 1000) / 1000.0 for i in range(1000000)]
total = 0.0
for r in range(20):
    for x in nums:
        total += x * x
print(total)
