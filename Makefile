TARGETS = smtp pop3 echoserver

all: $(TARGETS)

echoserver: echoserver.cc
	g++ -std=c++11 $^ -lpthread -g -o $@

smtp: smtp.cc
	g++ -std=c++11 $< -lpthread -g -o $@

pop3: pop3.cc
	g++ -std=c++11 $^ -I/opt/local/include/ -L/opt/local/bin/openssl -lcrypto -lpthread -g -o $@

pack:
	rm -f submit-hw2.zip
	zip -r submit-hw2.zip *.cc README Makefile

clean::
	rm -fv $(TARGETS) *~

realclean:: clean
	rm -fv cis505-hw2.zip
