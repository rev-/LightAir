#include <LightAir.h>
#include <assert.h>

// ----------------------------------------------------------------
// AllGames.cpp — central game registry for this sketch.
//
// Declare each game descriptor as extern, then call
// mgr.registerGame() for each.  Games are shown in the selection
// menu in the order they are registered here.
//
// registerGame() returns false on typeId collision or a full table.
// The assert() below catches either mistake at boot in debug builds.
// ----------------------------------------------------------------

extern const LightAir_Game game_ffa;
extern const LightAir_Game game_outflow;
extern const LightAir_Game game_teams;
extern const LightAir_Game game_upkeep;
extern const LightAir_Game game_flag;

void registerAllGames(LightAir_GameManager& mgr) {
    assert(mgr.registerGame(game_ffa));
    assert(mgr.registerGame(game_outflow));
    assert(mgr.registerGame(game_teams));
    assert(mgr.registerGame(game_upkeep));
    assert(mgr.registerGame(game_flag));
}
