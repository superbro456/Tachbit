# То же на Python (одно ядро)
def is_prime(n):
    if n < 2:
        return False
    d = 2
    while d * d <= n:
        if n % d == 0:
            return False
        d += 1
    return True

print(sum(1 for n in range(1, 5000001) if is_prime(n)))
