EXEC_NAME := http_proxy
CC = clang-15
CXX = clang++-15
UWS_PATH=./src/deps/uWebSockets
USOCKETS_PATH=$(UWS_PATH)/uSockets
ARCH := amd64
ifeq ($(PLATFORM), arm64)
	ARCH := arm64
endif
ifeq ($(PLATFORM), aarch64)
	ARCH := arm64
endif
ifeq ($(PLATFORM), arm)
	ARCH := arm64
endif

clean:
	cd $(USOCKETS_PATH) && rm -f *.o *.a *.so *.obj *.lib *.dll
	cd $(UWS_PATH)/ && rm -f *.o *.a *.so *.obj *.lib *.dll
	rm -f *.o *.a *.so *.dll *.obj *.lib

linux-exec:
	$(CXX) -I ./src -I $(UWS_PATH)/src -I $(USOCKETS_PATH)/src -I -DLIBUS_USE_OPENSSL -lssl -lcrypto -lstdc++ -pthread -fPIC -std=c++17 -c -O3 ./src/*.cpp 
	$(CXX) *.o $(USOCKETS_PATH)/usockets_linux_$(ARCH).a -lssl -lcrypto -lstdc++ -pthread -fPIC -lz -std=c++17 -o $(EXEC_NAME)

usockets:
	cd $(USOCKETS_PATH) && $(CC) -I ./src -DLIBUS_USE_OPENSSL -lssl -lcrypto -pthread -fPIC -std=c11 -O3 -c src/*.c src/eventing/*.c src/crypto/*.c
	cd $(USOCKETS_PATH) && $(CXX) -I -DLIBUS_USE_OPENSSL -lssl -lcrypto -lstdc++ -pthread -fPIC -std=c++17 -O3 -c src/crypto/*.cpp
	cd $(USOCKETS_PATH) && $(AR) rvs usockets_linux_$(ARCH).a *.o

linux:
# requires libssl-dev
	$(MAKE) clean

	$(MAKE) usockets
	
	$(MAKE) linux-exec
