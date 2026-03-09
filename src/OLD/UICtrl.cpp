#include <Arduino.h>  // Needed for String on embedded platforms

class UICtrl {
public:
    class UIEntry {
    private:
        String texts[6];
        int coordinates[6][2]; // [i][0] = x, [i][1] = y
        int currentTextCount = 0;

    public:
        int vibTimePattern[4];
        int freqs[4];
        int durations[4];
        int time;

        // Constructor to initialize vibration and sound patterns, and time
        Message(const int vib[4], const int fr[4], const int dur[4], int t)
        : time(t)
        {
            for (int i = 0; i < 4; ++i) {
                vibTimePattern[i] = vib[i];
                freqs[i] = fr[i];
                durations[i] = dur[i];
            }
        }

        // Method to add a string with its coordinates
        bool addString(const String& text, int x, int y) {
            if (currentTextCount >= 6) {
                return false; // No more space
            }
            texts[currentTextCount] = text;
            coordinates[currentTextCount][0] = x;
            coordinates[currentTextCount][1] = y;
            currentTextCount++;
            return true;
        }
    };

private:
    UIEntry UIEntry[5]; // For example, up to 5 messages
    int messageCount = 0;

public:
    // Store a message
    bool storeMessage(const Message& msg) {
        if (messageCount >= 5) {
            return false; // Storage full
        }
        messages[messageCount++] = msg;
        return true;
    }

    void printAllMessages() const {
        for (int i = 0; i < messageCount; ++i) {
            Serial.print("Message #");
            Serial.println(i + 1);
            messages[i].print();
        }
    }
};

