# LightAir
LightAir is a Laser Tag game designed to be open source and DIY-able, meaning feasible with a consumer type 3d printer (ref. Prusa MK3S).
It features an innovative hardware design using passive retroreflecting targets instead of active jackets, meaning it is less cumbersome to prepare and play with respect to the traditional active projector-active receiver setup.
It works even in full outdoor environments, with about 40m range. Features a very precise and visible light beam, uses an ESP32-S3 to share information allowing for complex game rules, for example implementing roles and interaction with other active in-game objects (totems).

This is the software library for LightAir. It is thought to allow for virtualization and testing and to hide the complex part inside objects.
It tries to make new game definitions as easy as possible, with the scope to promote the developing of new rule sets that work on the same platform.
It has to be compiled for ESP32-S3 and is written to be flashed using Arduino IDE, which is more simple than ESP-IDF.
