#include <LightAir.h>

// ----------------------------------------------------------------
// AllGames.cpp — central game registry for this sketch.
//
// Declare each game descriptor as extern, then call
// mgr.registerGame() for each.  Games are shown in the selection
// menu in the order they are registered here.
// ----------------------------------------------------------------

extern const LightAir_Game game_ffa;
extern const LightAir_Game game_teams;

void registerAllGames(LightAir_GameManager& mgr) {
    mgr.registerGame(game_ffa);
    mgr.registerGame(game_teams);
}
