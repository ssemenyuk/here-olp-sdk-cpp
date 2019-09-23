ALL_PLOTS = plots/ReadNPartitions_0.svg \
              plots/ReadNPartitions_1.svg \
              plots/ReadNPartitions_2.svg \
              plots/ReadNPartitions_3.svg \
              plots/ReadNPartitions_4.svg \
              plots/ReadNPartitions_5.svg \
              plots/ReadNPartitions_6.svg \
              plots/ReadNPartitions_7.svg \
              plots/ReadNPartitions_8.svg \


all: ${ALL_PLOTS}

plots/ReadNPartitions_0.svg: massif/ReadNPartitions_0.massif
	python3 scripts/plot_heap_usage.py $< $@

plots/ReadNPartitions_1.svg: massif/ReadNPartitions_1.massif
	python3 scripts/plot_heap_usage.py $< $@

plots/ReadNPartitions_2.svg: massif/ReadNPartitions_2.massif
	python3 scripts/plot_heap_usage.py $< $@

plots/ReadNPartitions_3.svg: massif/ReadNPartitions_3.massif
	python3 scripts/plot_heap_usage.py $< $@

plots/ReadNPartitions_4.svg: massif/ReadNPartitions_4.massif
	python3 scripts/plot_heap_usage.py $< $@

plots/ReadNPartitions_5.svg: massif/ReadNPartitions_5.massif
	python3 scripts/plot_heap_usage.py $< $@

plots/ReadNPartitions_6.svg: massif/ReadNPartitions_6.massif
	python3 scripts/plot_heap_usage.py $< $@

plots/ReadNPartitions_7.svg: massif/ReadNPartitions_7.massif
	python3 scripts/plot_heap_usage.py $< $@

plots/ReadNPartitions_8.svg: massif/ReadNPartitions_8.massif
	python3 scripts/plot_heap_usage.py $< $@


massif/ReadNPartitions_0.massif: massif/ReadNPartitions_1.massif
	valgrind --tool=massif \
              --time-unit=ms \
              --depth=1 \
              --massif-out-file=massif/ReadNPartitions_0.massif \
              ./build/tests/performance/olp-cpp-sdk-performance-tests \
                     --gtest_filter="*ReadNPartitions/0"

massif/ReadNPartitions_1.massif: massif/ReadNPartitions_2.massif
	valgrind --tool=massif \
              --time-unit=ms \
              --depth=1 \
              --massif-out-file=massif/ReadNPartitions_1.massif \
              ./build/tests/performance/olp-cpp-sdk-performance-tests \
                     --gtest_filter="*ReadNPartitions/1"

massif/ReadNPartitions_2.massif: massif/ReadNPartitions_3.massif
	valgrind --tool=massif \
              --time-unit=ms \
              --depth=1 \
              --massif-out-file=massif/ReadNPartitions_2.massif \
              ./build/tests/performance/olp-cpp-sdk-performance-tests \
                     --gtest_filter="*ReadNPartitions/2"

massif/ReadNPartitions_3.massif: massif/ReadNPartitions_4.massif
	valgrind --tool=massif \
              --time-unit=ms \
              --depth=1 \
              --massif-out-file=massif/ReadNPartitions_3.massif \
              ./build/tests/performance/olp-cpp-sdk-performance-tests \
                     --gtest_filter="*ReadNPartitions/3"

massif/ReadNPartitions_4.massif: massif/ReadNPartitions_5.massif
	valgrind --tool=massif \
              --time-unit=ms \
              --depth=1 \
              --massif-out-file=massif/ReadNPartitions_4.massif \
              ./build/tests/performance/olp-cpp-sdk-performance-tests \
                     --gtest_filter="*ReadNPartitions/4"

massif/ReadNPartitions_5.massif: massif/ReadNPartitions_6.massif
	valgrind --tool=massif \
              --time-unit=ms \
              --depth=1 \
              --massif-out-file=massif/ReadNPartitions_5.massif \
              ./build/tests/performance/olp-cpp-sdk-performance-tests \
                     --gtest_filter="*ReadNPartitions/5"

massif/ReadNPartitions_6.massif: massif/ReadNPartitions_7.massif
	valgrind --tool=massif \
              --time-unit=ms \
              --depth=1 \
              --massif-out-file=massif/ReadNPartitions_6.massif \
              ./build/tests/performance/olp-cpp-sdk-performance-tests \
                     --gtest_filter="*ReadNPartitions/6"


massif/ReadNPartitions_7.massif: massif/ReadNPartitions_8.massif
	valgrind --tool=massif \
              --time-unit=ms \
              --depth=1 \
              --massif-out-file=massif/ReadNPartitions_7.massif \
              ./build/tests/performance/olp-cpp-sdk-performance-tests \
                     --gtest_filter="*ReadNPartitions/7"

massif/ReadNPartitions_8.massif:
	valgrind --tool=massif \
              --time-unit=ms \
              --depth=1 \
              --massif-out-file=massif/ReadNPartitions_8.massif \
              ./build/tests/performance/olp-cpp-sdk-performance-tests \
                     --gtest_filter="*ReadNPartitions/8"
