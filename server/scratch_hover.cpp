#include <iostream>
#include <fstream>
#include <string>

int main() {
    std::ifstream file("server/tests/features/ComplexHoverTests.cpp");
    std::string line;
    while(std::getline(file, line)) {
        // Nothing, just check we can read it.
    }
    return 0;
}
