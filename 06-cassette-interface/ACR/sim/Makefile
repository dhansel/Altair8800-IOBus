ACR.exe: ACR.o Arduino.o main.o
	g++ ACR.o Arduino.o main.o -o ACR.exe

ACR.o: ../ACR.ino Arduino.h
	g++ $(CFLAGS) -I . -c -o ACR.o -x c++ ../ACR.ino

Arduino.o: Arduino.cpp Arduino.h
	g++ $(CFLAGS) -c -o Arduino.o Arduino.cpp

main.o: main.cpp Arduino.h
	g++ $(CFLAGS) -c -o main.o main.cpp

test: .PHONY
	make -C test

clean:
	rm *.o ACR.exe

.PHONY: