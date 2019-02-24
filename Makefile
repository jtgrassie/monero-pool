TARGET = monero-pool

TYPE = debug

ifeq ($(MAKECMDGOALS),release)
TYPE = release
endif

ifeq ($(MAKECMDGOALS),profile)
TYPE = profile
endif

DIRS = src data rxi/log/src monero monero/epee/include monero/epee/src monero/common monero/crypto \
       monero/cryptonote_basic monero/cryptonote_core monero/device monero/ringdt monero/easylogging++

OS := $(shell uname -s)

CPPDEFS = _GNU_SOURCE AUTO_INITIALIZE_EASYLOGGINGPP LOG_USE_COLOR

CCPARAM = -Wall $(CFLAGS) -maes
CXXFLAGS = -std=c++11

ifeq ($(OS), Darwin)
CXXFLAGS += -stdlib=libc++
CPPDEFS += HAVE_MEMSET_S
endif

ifeq ($(OS),Darwin)
LDPARAM = -flat_namespace -undefined warning
else
LDPARAM = -rdynamic -Wl,-warn-unresolved-symbols
endif

ifeq ($(TYPE),debug)
CCPARAM += -g
CPPDEFS += DEBUG
endif

ifeq ($(TYPE),profile)
LDPARAM = -pg /lib/libc.so.5
CCPARAM += -pg
endif

ifeq ($(TYPE), release)
CCPARAM += -O3 -Wno-unused-variable
ifneq ($(OS), Darwin)
LDPARAM = -rdynamic -Wl,--unresolved-symbols=ignore-in-object-files
endif
endif

LDPARAM += $(LDFLAGS)

LIBS := lmdb pthread microhttpd
ifeq ($(OS), Darwin)
LIBS += c++ boost_system-mt boost_date_time-mt
else
LIBS += boost_system boost_date_time uuid
endif

PKG_LIBS := $(shell pkg-config \
    libevent \
    json-c \
    openssl \
    --libs)

STATIC_LIBS = 
DLIBS =

INCPATH := $(DIRS) /opt/local/include /usr/local/include

PKG_INC := $(shell pkg-config \
    libevent \
    json-c \
    openssl \
    --cflags)

LIBPATH := /opt/local/lib/ /usr/local/lib

# Which files to add to backups, apart from the source code
EXTRA_FILES = Makefile

C++ = g++
CC = gcc

STORE = build/$(TYPE)
SOURCE := $(foreach DIR,$(DIRS),$(wildcard $(DIR)/*.cpp))
CSOURCE := $(foreach DIR,$(DIRS),$(wildcard $(DIR)/*.c))
SSOURCE := $(foreach DIR,$(DIRS),$(wildcard $(DIR)/*.S))
HTMLSOURCE := $(foreach DIR,$(DIRS),$(wildcard $(DIR)/*.html))
HEADERS := $(foreach DIR,$(DIRS),$(wildcard $(DIR)/*.h))
OBJECTS := $(addprefix $(STORE)/, $(SOURCE:.cpp=.o))
COBJECTS := $(addprefix $(STORE)/, $(CSOURCE:.c=.o))
SOBJECTS := $(addprefix $(STORE)/, $(SSOURCE:.S=.o))
HTMLOBJECTS := $(addprefix $(STORE)/, $(HTMLSOURCE:.html=.o))
DFILES := $(addprefix $(STORE)/,$(SOURCE:.cpp=.d))
CDFILES := $(addprefix $(STORE)/,$(CSOURCE:.c=.d))
SDFILES := $(addprefix $(STORE)/,$(CSOURCE:.S=.d))


.PHONY: clean backup dirs debug release profile

$(TARGET): dirs $(OBJECTS) $(COBJECTS) $(SOBJECTS) $(HTMLOBJECTS)
	@echo Linking $(OBJECTS)...
	$(C++) -o $(STORE)/$(TARGET) $(OBJECTS) $(COBJECTS) $(SOBJECTS) $(HTMLOBJECTS) $(LDPARAM) $(foreach LIBRARY, $(LIBS),-l$(LIBRARY)) $(foreach LIB,$(LIBPATH),-L$(LIB)) $(PKG_LIBS) $(STATIC_LIBS)
	@cp pool.conf $(STORE)/

debug release profile: $(TARGET)

$(STORE)/%.o: %.cpp
	@echo Creating object file for $*...
	$(C++) -Wp,-MMD,$(STORE)/$*.dd $(CCPARAM) $(CXXFLAGS) $(foreach INC,$(INCPATH),-I$(INC)) $(PKG_INC)\
		$(foreach CPPDEF,$(CPPDEFS),-D$(CPPDEF)) -c $< -o $@
	@sed -e '1s/^\(.*\)$$/$(subst /,\/,$(dir $@))\1/' $(STORE)/$*.dd > $(STORE)/$*.d
	@rm -f $(STORE)/$*.dd

$(STORE)/%.o: %.c
	@echo Creating object file for $*...
	$(CC) -Wp,-MMD,$(STORE)/$*.dd $(CCPARAM) $(foreach INC,$(INCPATH),-I$(INC)) $(PKG_INC)\
		$(foreach CPPDEF,$(CPPDEFS),-D$(CPPDEF)) -c $< -o $@
	@sed -e '1s/^\(.*\)$$/$(subst /,\/,$(dir $@))\1/' $(STORE)/$*.dd > $(STORE)/$*.d
	@rm -f $(STORE)/$*.dd

$(STORE)/%.o: %.S
	@echo Creating object file for $*...
	$(CC) -Wp,-MMD,$(STORE)/$*.dd $(CCPARAM) $(foreach INC,$(INCPATH),-I$(INC)) $(PKG_INC)\
		$(foreach CPPDEF,$(CPPDEFS),-D$(CPPDEF)) -c $< -o $@
	@sed -e '1s/^\(.*\)$$/$(subst /,\/,$(dir $@))\1/' $(STORE)/$*.dd > $(STORE)/$*.d
	@rm -f $(STORE)/$*.dd

$(STORE)/%.o: %.html
	@echo Creating object file for $*...
	xxd -i $< | sed -e 's/src_//' -e 's/embed_//' > $(STORE)/$*.c
	$(CC) $(CCPARAM)  -c $(STORE)/$*.c -o $@
	@rm -f $(STORE)/$*.c

# Empty rule to prevent problems when a header is deleted.
%.h: ;

clean:
	@echo Making clean.
	@find ./build -type f -name '*.o' -delete
	@find ./build -type f -name '*.d' -delete
	@find ./build -type f -name $(TARGET) -delete

backup:
	@-if [ ! -e build/backup ]; then mkdir -p build/backup; fi;
	@zip build/backup/backup_`date +%d-%m-%y_%H.%M`.zip $(SOURCE) $(HEADERS) $(EXTRA_FILES)

dirs:
	@-if [ ! -e $(STORE) ]; then mkdir -p $(STORE); fi;
	@-$(foreach DIR,$(DIRS), if [ ! -e $(STORE)/$(DIR) ]; then mkdir -p $(STORE)/$(DIR); fi; )

-include $(DFILES)
-include $(CDFILES)
-include $(SDFILES)

