CC=g++
CXXFLAGS=-Wall -g 

DIR = ../src/
SRC = $(DIR)*.cpp $(DIR)/*.h $(DIR)/http/http* \
		$(DIR)/lock/lock* $(DIR)/log/*.h $(DIR)/log/*.cpp \
		$(DIR)/mysqlpool/sql* $(DIR)/threadpool/*.h $(DIR)/timer/timer*

OBJ=server

$(OBJ):$(SRC)
	$(CC) $(SRC) -o ../bin/$(OBJ) $(CXXFLAGS) -pthread -lmysqlclient


.PHONY:clean all
clean:
	rm -rf ../bin/$(OBJ)
