
FILES = *.cpp *.h ./threadpool/* ./http/*  ./lock/* ./log/* ./timer/timer* ./mysqlpool/*
src := $(wildcard $(FILES))

target=server

CXXFLAGS=-Wall -g 


$(target):$(src)
	g++ $(src) -o $(target) $(CXXFLAGS) -pthread -lmysqlclient


.PHONY:clean all
clean:
	rm -rf $(target)
