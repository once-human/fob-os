# Laptop simulator: runs the same renderer and UI sources as the watch and writes PPM screenshots to out/.
SRC = ../firmware/overlay/src/app/silent_ota
sim: sim.c $(wildcard $(SRC)/*.inc $(SRC)/*.h)
	mkdir -p out
	$(CC) -O2 -I$(SRC) sim.c -lm -o sim
clean:
	rm -rf sim out
