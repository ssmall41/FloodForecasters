#Compilers
PCC = mpicc

#Basic flags for compiling
FLAGS = -std=gnu99 -D_FILE_OFFSET_BITS=64

#Flags for performance
OPTFLAGS = -O3 -march=nocona

#Flags for debugging
#DBFLAGS = -g
#DBFLAGS = -g -Wunused-parameter -Wall -Wextra #-Wpadded

#Locations
OBJDIR = ./objects

#Header and libraries
HEADERS = -I/Groups/IFC/Asynch/
LIBSLOC = -L/Groups/IFC/Asynch/libs/ -Wl,-rpath=/Groups/IFC/Asynch/libs/
LIBS = $(LIBSLOC) -lm -lpq -lasynch_helium
FORECASTER_HEADERS = -I/Groups/IFC/libssh2-1.6.0/include/
FORECASTER_LIBS = -L/Groups/IFC/libssh2-1.6.0/lib/ -Wl,-rpath=/Groups/IFC/libssh2-1.6.0/lib -lssh2

#Objects
FORECASTEROBJS = $(addprefix $(OBJDIR)/,forecaster_methods.o)
FORECASTER_MAPSOBJS = $(addprefix $(OBJDIR)/,forecaster_maps.o)
FORECASTER_MAPS_END_OBJS = $(addprefix $(OBJDIR)/,forecaster_maps_end.o)
ASYNCHPERSISOBJS = $(addprefix $(OBJDIR)/,asynchpersis.o)
ASYNCHPERSIS_END_OBJS = $(addprefix $(OBJDIR)/,asynchpersis_end.o)

#How to compile and link
$(OBJDIR)/%.o: %.c
	$(PCC) -c $*.c $(HEADERS) $(FLAGS) $(DBFLAGS) $(OPTFLAGS) $(EXTRA_FLAGS) -o $(OBJDIR)/$*.o

FORECASTER_MAPS: EXTRA_FLAGS=$(FORECASTER_HEADERS)
FORECASTER_MAPS: $(FORECASTEROBJS) $(FORECASTER_MAPSOBJS)
	$(PCC) $(FORECASTEROBJS) $(FORECASTER_MAPSOBJS) $(LIBS) $(FLAGS) $(DBFLAGS) $(OPTFLAGS) $(EXTRA_FLAGS) $(FORECASTER_LIBS) -o FORECASTER_MAPS

FORECASTER_MAPS_END: EXTRA_FLAGS=$(FORECASTER_HEADERS)
FORECASTER_MAPS_END: $(FORECASTEROBJS) $(FORECASTER_MAPS_END_OBJS)
	$(PCC) $(FORECASTEROBJS) $(FORECASTER_MAPS_END_OBJS) $(LIBS) $(FLAGS) $(DBFLAGS) $(OPTFLAGS) $(EXTRA_FLAGS) $(FORECASTER_LIBS) -o FORECASTER_MAPS_END

ASYNCHPERSIS: EXTRA_FLAGS=$(FORECASTER_HEADERS)
ASYNCHPERSIS: $(FORECASTEROBJS) $(ASYNCHPERSISOBJS)
	$(PCC) $(FORECASTEROBJS) $(ASYNCHPERSISOBJS) $(LIBS) $(FLAGS) $(DBFLAGS) $(OPTFLAGS) $(EXTRA_FLAGS) $(FORECASTER_LIBS) -o ASYNCHPERSIS

ASYNCHPERSIS_END: EXTRA_FLAGS=$(FORECASTER_HEADERS)
ASYNCHPERSIS_END: $(FORECASTEROBJS) $(ASYNCHPERSIS_END_OBJS)
	$(PCC) $(FORECASTEROBJS) $(ASYNCHPERSIS_END_OBJS) $(LIBS) $(FLAGS) $(DBFLAGS) $(OPTFLAGS) $(EXTRA_FLAGS) $(FORECASTER_LIBS) -o ASYNCHPERSIS_END

clean:
	rm -f $(OBJDIR)/*.o
	rm -f ASYNCHPERSIS
	rm -f FORECASTER_MAPS
	rm -f ASYNCHPERSIS_END
	rm -f FORECASTER_MAPS_END

