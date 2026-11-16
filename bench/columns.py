# То же на Python (2 млн объектов вместо 20 млн, иначе слишком долго): время ×10
class Particle:
    __slots__ = ("x", "y", "z", "mass", "id", "alive")
    def __init__(self, i):
        self.x, self.y, self.z, self.mass, self.id, self.alive = 1.0, 2.0, 3.0, (i % 100) / 10.0, i, True
parts = [Particle(i) for i in range(2000000)]
total = 0.0
for _ in range(10):
    for p in parts:
        total += p.mass
print(total)
