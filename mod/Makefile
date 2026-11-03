MAKEFLAGS += -s -j

DIST_DIR = ./dist
SRC_DIR = ./src

SRC_DIRS = $(SRC_DIR) $(wildcard $(SRC_DIR)/*/)

C_SRC = $(wildcard $(SRC_DIR)/*.c $(SRC_DIR)/*/*.c)
CPP_SRC = $(wildcard $(SRC_DIR)/*.cpp $(SRC_DIR)/*/*.cpp)

C_OBJ = $(addprefix $(DIST_DIR)/, $(notdir $(C_SRC:.c=.o)))
CPP_OBJ = $(addprefix $(DIST_DIR)/, $(notdir $(CPP_SRC:.cpp=.o)))

CXX_HEADER = $(wildcard $(SRC_DIR)/*.h $(SRC_DIR)/*/*.h)

VERSION_SCRIPT = $(SRC_DIR)/exports.txt

TARGET = sky-blueprint.dll
BIN_TARGET = $(DIST_DIR)/$(TARGET)

CC = gcc
CXX = g++

CFLAGS = -O2 -ffunction-sections -fdata-sections -static -s -mavx -msse
CFLAGS += -Wall -Wno-unused-const-variable -Wno-unused-function -Wno-missing-braces
CFLAGS += -I./src
CFLAGS += -I./libraries/htmodloader/includes/imgui-1.92.2b -I./libraries/htmodloader/includes/imgui-1.92.2b/backends
CFLAGS += -I./libraries/htmodloader/includes/htmodloader

LFLAGS = -Wl,--gc-sections,-O2,--as-needed,--version-script=$(VERSION_SCRIPT)
LFLAGS += -L./libraries/htmodloader/lib -lhtmodloader -lshell32

vpath %.c $(SRC_DIRS)
vpath %.cpp $(SRC_DIRS)

.PHONY: all clean

all: $(DIST_DIR)
	-@$(MAKE) $(BIN_TARGET)

$(BIN_TARGET): $(C_OBJ) $(CPP_OBJ)
	@echo Linking ...
	@$(CXX) $(CFLAGS) $^ -shared -o $@ $(LFLAGS)
	@echo Done.

$(DIST_DIR)/%.o: %.c $(CXX_HEADER)
	@echo Compiling "$<" ...
	@$(CC) $(CFLAGS) -c $< -o $@

$(DIST_DIR)/%.o: %.cpp $(CXX_HEADER)
	@echo Compiling "$<" ...
	@$(CXX) $(CFLAGS) -c $< -o $@

$(DIST_DIR):
	-@mkdir dist

clean:
	-@rm -f ./dist/*.o
	-@rm -f ./dist/*.dll
