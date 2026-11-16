limit = 20000000
sieve = [True] * (limit + 1)
found = 0
for i in range(2, limit + 1):
    if sieve[i]:
        found += 1
        j = i * i
        while j <= limit:
            sieve[j] = False
            j += i
print(found)
