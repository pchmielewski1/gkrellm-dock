CFLAGS ?= -O2 -fPIC -Wall -Wextra -std=c17
GKCFLAGS := $(shell pkg-config gkrellm --cflags)
GKLIBS := $(shell pkg-config gkrellm --libs)
PLUGIN_DIR ?= $(HOME)/.gkrellm2/plugins
THEME_DIR ?= $(HOME)/.gkrellm2/themes

LIB_SRCS := lib/cpu_map.c lib/cpu_stat.c lib/thermal_map.c
LIB_OBJS := $(LIB_SRCS:.c=.o)

.PHONY: all clean test install unit plugins

all: plugins

lib/%.o: lib/%.c
	$(CC) $(CFLAGS) -Ilib -c -o $@ $<

build/libgkdock_test.so: $(LIB_SRCS)
	mkdir -p build
	$(CC) -shared -fPIC $(CFLAGS) -Ilib -o $@ $(LIB_SRCS)

unit: build/libgkdock_test.so
	python3 tests/test_cpu_map.py

plugins/cpu_clusters/cpu_clusters.so: plugins/cpu_clusters/cpu_clusters.c $(LIB_OBJS)
	$(CC) $(CFLAGS) $(GKCFLAGS) -Ilib -shared -o $@ \
		plugins/cpu_clusters/cpu_clusters.c lib/cpu_map.c lib/cpu_stat.c $(GKLIBS)

plugins/board_acpi/board_acpi.so: plugins/board_acpi/board_acpi.c lib/thermal_map.o
	$(CC) $(CFLAGS) $(GKCFLAGS) -Ilib -shared -o $@ \
		plugins/board_acpi/board_acpi.c lib/thermal_map.c $(GKLIBS)

plugins/uma_dram/uma_dram.so: plugins/uma_dram/uma_dram.c
	$(CC) $(CFLAGS) $(GKCFLAGS) -shared -o $@ plugins/uma_dram/uma_dram.c $(GKLIBS)

plugins/llm_nim/llm_nim.so: plugins/llm_nim/llm_nim.c
	$(CC) $(CFLAGS) $(GKCFLAGS) $(shell pkg-config --cflags libcurl) -shared \
		-o $@ plugins/llm_nim/llm_nim.c $(GKLIBS) $(shell pkg-config --libs libcurl)

plugins/nvidia/nvidia.so:
	$(MAKE) -C plugins/nvidia

plugins: plugins/cpu_clusters/cpu_clusters.so \
	plugins/board_acpi/board_acpi.so \
	plugins/uma_dram/uma_dram.so \
	plugins/llm_nim/llm_nim.so \
	plugins/nvidia/nvidia.so

test: plugins

install: plugins
	mkdir -p $(PLUGIN_DIR) $(THEME_DIR)
	install -m755 plugins/cpu_clusters/cpu_clusters.so $(PLUGIN_DIR)/
	install -m755 plugins/board_acpi/board_acpi.so $(PLUGIN_DIR)/
	install -m755 plugins/uma_dram/uma_dram.so $(PLUGIN_DIR)/
	install -m755 plugins/llm_nim/llm_nim.so $(PLUGIN_DIR)/
	install -m755 plugins/nvidia/nvidia.so $(PLUGIN_DIR)/
	rm -rf $(THEME_DIR)/gb10-blue
	cp -a themes/gb10-blue $(THEME_DIR)/
	./scripts/install_config.sh

clean:
	rm -f lib/*.o plugins/*/*.so plugins/nvidia/*.o build/*.so
	$(MAKE) -C plugins/nvidia clean || true
