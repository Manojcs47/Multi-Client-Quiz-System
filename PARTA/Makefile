## CS3530 Assignment-1  --  cs23btech11047
## Build:  make            (builds ./server and ./client)
##         make clean
##         make json       (downloads json.hpp if it is missing)

CXX      = g++
CXXFLAGS = -g -O2 -Wall -Wextra -Wno-unused-parameter -std=c++11 -pthread
LOCAL    = $(HOME)/local

# json.hpp is looked for next to the sources first, then in ~/local/include
CXXFLAGS += -I. -I$(LOCAL)/include

LDFLAGS  = -L$(LOCAL)/lib -Wl,-rpath,$(LOCAL)/lib
SERVER_LIBS = -lpthread -lcurl
CLIENT_LIBS = -lpthread

SERVER = server
CLIENT = client

.PHONY: all clean json

all: $(SERVER) $(CLIENT)

$(SERVER): server.cpp protocol.h llm.h json.hpp
	$(CXX) $(CXXFLAGS) -o $(SERVER) server.cpp $(LDFLAGS) $(SERVER_LIBS)

$(CLIENT): client.cpp protocol.h json.hpp
	$(CXX) $(CXXFLAGS) -o $(CLIENT) client.cpp $(LDFLAGS) $(CLIENT_LIBS)

json:
	@test -f json.hpp || curl -sSL -o json.hpp \
	  https://raw.githubusercontent.com/nlohmann/json/v3.11.3/single_include/nlohmann/json.hpp
	@echo "json.hpp ready"

clean:
	rm -f $(SERVER) $(CLIENT)
