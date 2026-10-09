.PHONY: test
test:
	go test -race . ./hosted
	$(CXX) -std=c++17 -Wall -Wextra -Werror tests/host.cpp -o /tmp/r2-memento-host-test
	/tmp/r2-memento-host-test
