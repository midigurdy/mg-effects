BUILD_DIR = build
CFLAGS		=	$(INCLUDES) -Wall -Werror -O3 -fPIC -ffast-math
PLUGINS		=	src/sympathetic.so

src/%.so:	src/%.c
	mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $(BUILD_DIR)/$*.o -c src/$*.c
	$(LD) -o $(BUILD_DIR)/$*.so $(BUILD_DIR)/$*.o -shared

targets:	$(PLUGINS)

clean:
	rm -rf $(BUILD_DIR)
