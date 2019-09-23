import sys
import matplotlib.pyplot as plt

assert(len(sys.argv) == 3)
input_file_name = sys.argv[1]
output_image_name = sys.argv[2]

times = list()
heap_sizes = list()

for line in open(input_file_name, 'r').readlines():
    if line.find("time=") != -1:
        time = int(line.split('=')[1])
        times.append(time)
    elif line.find("mem_heap_B=") != -1:
        heap_size = int(line.split('=')[1])
        heap_sizes.append(heap_size)

plt.plot(times, heap_sizes)
plt.xlabel("Time (ms)")
plt.ylabel("Heap size (bytes)")
plt.savefig(output_image_name)
