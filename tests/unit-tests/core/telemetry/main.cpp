//
// Telemetry module tests: self-contained, no krkr2 core loggers required.
//

#include <catch2/catch_session.hpp>

int main(int argc, char *argv[]) {
    return Catch::Session().run(argc, argv);
}
