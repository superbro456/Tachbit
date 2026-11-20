# То же на Python
total = 0
for r in range(100):
    lines = ["строка " + str(i) for i in range(1, 100001)]
    for s in lines:
        total += len(s.encode())
print(total)
