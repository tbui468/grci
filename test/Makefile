grci.o: ./../src/grci.c
	$(CC) -std=c99 -DNDEBUG -Wunused-result -Wall -Wno-unused-function -c -o grci.o -fpic ./../src/grci.c -O2

test: shared
	python3 test.py

shared: grci.o
	$(CC) -shared -o libgrci.so grci.o

clean:
	rm -rf grci.o libgrci.so __pycache__
