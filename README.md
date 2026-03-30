# LightAir
LightAir is a Laser Tag game designed to be open source and DIY-able, meaning feasible with a consumer type 3d printer (ref. Prusa MK3S).
It features an innovative hardware design using passive retroreflecting targets instead of active jackets, meaning it is less cumbersome to prepare and play with respect to the traditional active projector-active receiver setup.
It works even in full outdoor environments, with about 40m range. Features a very precise and visible light beam, uses an ESP32-S3 to share information allowing for complex game rules, for example implementing roles and interaction with other active in-game objects (totems).

This is the software library for LightAir. It is thought to allow for virtualization and testing and to hide the complex part inside objects.
It tries to make new game definitions as easy as possible, with the scope to promote the developing of new rule sets that work on the same platform.
It has to be compiled for ESP32-S3 and is written to be flashed using Arduino IDE, which is more simple than ESP-IDF.

## Design guidelines
### Nonviolent semantics
While a tag game is normally associated to a war simulation, LightAir wants to drop this label. We want to make clear a ray of light **is** a ray of light, not a metaphor for an ammunition or other means to offend people. This choice shows in many parts of the code, for example by the use of terms like LIT, SHONE, ENLIGHT instead of the common counterparts used in other tag games. Anyway, these terms have clear meanings and keep them throughout the code.
LightAir still represents conflict, but in a non-violent way where the interactions are a way to communicate and "recognize" each other, instead of submitting them.
This is not only an ethical choice, but mainly the base for more functional real-life interactions between players and also for the game lore.
### Open sourceness and participation
The whole LightAir project is based on open source - code is shared on github with GPL licence, while hardware parts are designed and realized with open source programs.
Participation is considered an important asset, so for example the software is structured to define a ruleset (a game type) with a file that is as simple as practically feasible.
### Game ruleset and configuration sharing
1. Game ruleset and configuration is decided by one designed player (NVM settings) right after boot.
2. once defined, the game and its configuration is shared with all the other players via ESP-NOW packets
3. therefore, players must all be within radio reach, while totems are not required to
4. Totems are "activated" by custom messages by the players. This allows totems to be planted outside radio reach at the beginning of the game
