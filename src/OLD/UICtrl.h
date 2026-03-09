#ifndef UICtrl_h
#define UICtrl_h

#include "LCDCtrl.h"
#include "SoundCtrl.h"
#include "VibCtrl.h"
#include "Arduino.h"

class UICtrl {
public:
    class UIEntry {
    private:
        String texts[6];
        int coordinates[6][2]; // coordinates[i][0] = x, coordinates[i][1] = y
        int currentTextCount = 0;

    public:
        int vibTimePattern[4];
        int freqs[4];
        int durations[4];
        int time;

        // Constructor to initialize vibration pattern, frequencies, durations, and time
        Message(const int vib[4], const int fr[4], const int dur[4], int t);

        // Method to add a string with coordinates (x, y)
        bool addString(const String& text, int x, int y);

        // Print the message content to Serial
        void print() const;
    };

private:
    UIEntry UIEntry[5]; // Storage for up to 5 messages
    int messageCount = 0;

public:
    // Stores a message if space is available
    bool storeMessage(const Message& msg);

    // Prints all stored messages
    void printAllMessages() const;
};

#endif
