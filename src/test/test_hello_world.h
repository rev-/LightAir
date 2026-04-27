#ifndef LIGHTAIR_TEST_HW_H
#define LIGHTAIR_TEST_HW_H

#include <ArduinoLog.h>
#include <AUnit.h>

test(test_hello_world) {
    Log.infoln("Hello, AUnit!");
    passTestNow();
}

#endif // LIGHTAIR_TEST_HW_H