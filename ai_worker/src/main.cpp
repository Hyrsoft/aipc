#include <exception>
#include <iostream>

#include "application.h"

int main(int argc, char* argv[]) {
    try {
        return ai_worker::Run(argc, argv);
    } catch (const std::exception& exception) {
        std::cerr << "ai_worker fatal: " << exception.what() << '\n';
        return 2;
    }
}
